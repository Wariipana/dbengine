# Línea base de C2: dónde se va el tiempo

2026-10-24, commit `507c171`. ClickBench 10M filas, `hits_dbengine` sin
índices ni Bloom, PostgreSQL 18.6 de PGDG (con `postgresql-18-dbgsym`),
aarch64, 4 núcleos. Tiempos de referencia en
[`bench/clickbench/resultado-10m-linea-base-c2.txt`](../bench/clickbench/resultado-10m-linea-base-c2.txt):
media geométrica 3,97× contra heap sin índices, suma 1,83×.

Perfiles con `perf record -a -g -F 999` mientras corre la query (el sistema
entero, porque las queries usan workers paralelos), filtrados por proceso
`postgres`. En caliente salvo q21. `perf` agrega entre 6% y 15% al tiempo.

## Resumen por query

| query | tiempo | dónde se va |
|---|---|---|
| q3 `SUM, COUNT, AVG` | 0,50 s | ~50% avanzar agregados fila por fila (`dbe_agg_advance`, `dbe_vec_value`, `int2_avg_accum`, `int2_sum`); ~14% abrir segmentos |
| q24 `SELECT * ... LIKE ... ORDER BY LIMIT 10` | 3,5 s | **~48% abrir segmentos** (`SegmentReader::SegmentReader` 23%, `memcpy` 25%); 17% `LIKE` |
| q29 `REGEXP_REPLACE` + `GROUP BY` | 22,5 s | ~65% el motor de regexp de PostgreSQL (`miss`, `getvacant`, `longest`, `shortest`) |
| q33 `GROUP BY WatchID, ClientIP` | 14,5 s | ~60% el `HashAggregate` de PostgreSQL volcando a disco (`agg_refill_hash_table`); dbengine ~7% |
| q21 `URL LIKE` en frío | 4,5 s (1,09 s caliente) | **~75% espera de I/O**; de la CPU, 36% `LIKE` |

## Hallazgo principal: abrir un segmento es decodificarlo entero

`SegmentReader::SegmentReader` no solo lee el header: expande FOR y RLE a
un arreglo de 65.536 valores (`materialize_for`, `materialize_rle`) y
descomprime el diccionario FSST completo (`materialize_fsst_dict`), lo use o
no la query. Tres costos encima:

- `store_uint(p, w, v)` copia con un ancho variable: el compilador no puede
  reducirlo a una instrucción y termina en `memcpy@plt` **una vez por
  valor**. Es el `__memcpy_generic` + `memcpy@plt` (25%) de q24.
- `expanded_.assign(n, 0)` pone en cero el buffer antes de sobrescribirlo
  (`__memset_zva64`); en FSST, con 8 veces el tamaño comprimido.
- `bitpack_get` extrae los bits valor por valor, sin desenrollar por ancho.

Es la razón real de que la materialización tardía "no ayudara" en q24 (ver
README, *Las seis que pierden*): el cursor difiere la lectura de las 105
columnas, pero abrir el segmento ya las decodifica. No es estructural.

Otras observaciones:

- `UserID` y `ClientIP` quedan en **RLE** en ~150 de 153 stripes (el
  dataset viene ordenado por `CounterID`), así que también se expanden
  enteras al abrir.
- `Referer` tiene en promedio 22.315 valores distintos por stripe: cada uno
  se repite ~3 veces. Evaluar el regexp una vez por entrada del
  diccionario recorta ~3× las llamadas, del orden de 1,7× para q29. `URL`
  repite bastante más (~5×, 12.856 distintas por stripe).
- q33 casi no pasa por dbengine: es el `HashAggregate` de PostgreSQL con
  `work_mem` de 4 MB volcando a disco, igual que con heap.

## Qué cambia en el orden de C2

1. **Apertura perezosa de segmentos** (nuevo, primero): el header al abrir;
   FOR y RLE se expanden recién cuando alguien pide el vector, y `get(fila)`
   va directo al dato empacado (FOR en O(1), RLE con búsqueda binaria sobre
   las corridas); el diccionario FSST se descomprime por entrada a demanda.
   Más desempaquetado especializado por ancho y sin rellenar con ceros.
   Ataca q24 y toda query que lea muchas columnas con un filtro selectivo,
   y abarata todas las demás.
2. **Evaluación por entrada del diccionario**: el `LIKE` es el costo
   dominante de CPU en q21–q24, y el regexp en q29.
3. **`read_stream` + AIO**: en frío, q21 pasa el 75% esperando I/O.
4. **Agregados precalculados (SMA)**: q3 se va casi entero en avanzar
   agregados fila por fila.
5. La tabla de hash propia baja de prioridad para q33 (el cuello es el
   `HashAggregate` de PostgreSQL), y hay que perfilar q9/q10/q19 antes de
   decidir.

## Resultado: apertura perezosa (C2.1.0)

A/B en la misma sesión, alternando los dos `.so` con el cluster reiniciado
entre medio, mejor de 3 en caliente, dos vueltas:

| query | antes | después | |
|---|---|---|---|
| q24 | 3.252–3.266 ms | 1.626–1.688 ms | **2,0×** |
| q3 | 471 ms | 394–395 ms | 1,19× |
| q14, q15, q19, q22, q26, q33 | | | ±1,5%, ruido |

Las 43 contra la línea base: media geométrica de dbengine 0,993 del tiempo
anterior; la suma no se mueve porque la dominan q29, q33 y q9/q10/q19, que
no pasan por acá.

## Resultado: predicados por entrada del diccionario (C2.2, ítem 5)

Mismo binario, `dbengine.enable_dict_quals` apagado y prendido, dos vueltas
(mejor de 3 en caliente):

| query | apagado | prendido | |
|---|---|---|---|
| q21 `count(*) WHERE URL LIKE` | 1.088–1.096 ms | 687–688 ms | 1,59× |
| q22 | 1.250–1.256 ms | 704–710 ms | 1,78× |
| q23 `SELECT * WHERE Title LIKE ... AND URL NOT LIKE` | 2.111–2.129 ms | 959–971 ms | **2,2×** |
| q24 | 1.602–1.606 ms | 1.198–1.203 ms | 1,34× |
| q26 | 369–370 ms | 262–274 ms | 1,4× |
| q27 | 427–428 ms | 316–317 ms | 1,35× |

Dos partes: el resultado se guarda por código (un byte por entrada del
diccionario) y la columna se lee recién cuando hay que evaluar de verdad.
La primera sola daba 1,25×–1,43×; con la segunda sale `dbe_segreader_get`
(21% de q21) del camino de las filas cuyo código ya se conoce.

Las 43 contra la línea base, con esto y la apertura perezosa: media
geométrica 0,887, suma 109,9 → 105,4 s.

## Resultado: expresiones de la salida por diccionario

El scan de q29 proyecta `REGEXP_REPLACE(Referer, ...)`. Con la proyección
propia del Custom Scan, cada valor distinto de Referer en el stripe pasa una
vez por el motor de regexp en vez de ~3. Misma GUC, dos vueltas:

| query | apagado | prendido | |
|---|---|---|---|
| q29 | 20.996–21.051 ms | 11.831–11.888 ms | **1,77×** |
| q34, q35 | | | ruido |

Las 43 contra la línea base: media geométrica 0,879, suma 109,9 → 96,8 s.

## Resultado: read stream y readahead (C2.1b, ítem 2)

En frío (page cache de la relación soltado antes de cada query), las 20 de
`cold.sh`, solo dbengine. El disco de la VM da ~150 MB/s secuenciales con
bloques de 1 MB y 18 MB/s con lecturas sincrónicas de 8 kB.

| | media geométrica | suma |
|---|---|---|
| antes (`ReadBuffer` por bloque, worker) | 1 | 86,3 s |
| read stream, worker | 0,882 | 81,7 s |
| read stream + `FADV_RANDOM`, io_uring | **0,735** | 72,2 s |

Lo que encontró la medición: q21 pedía 274 MB y el disco leía 532. Con
`read_ahead_kb` = 2048, el readahead del kernel es más grande que un segmento
entero y trae las columnas de al lado. Con `FADV_RANDOM` se lee del disco
exactamente lo pedido (271 MB). q24 (las 105 columnas) no cambia: lee la
tabla entera y está en el techo del disco.
