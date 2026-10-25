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
