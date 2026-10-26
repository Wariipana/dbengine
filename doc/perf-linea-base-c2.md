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

## Resultado: agregados precalculados (C2.1b, ítem 1)

q3 (`SUM(AdvEngineID), COUNT(*), AVG(ResolutionWidth)`, los dos smallint):
473 → 14,5 ms. Los 153 stripes salen del catálogo (`Stripes desde
metadatos: 153` en el EXPLAIN ANALYZE) y no se lee un byte de datos; lo que
queda son las lecturas del catálogo.

Para medirlo hubo que recargar la tabla: los stripes escritos con 0.1.0 no
tienen la suma. Primero la copié de `hits_heap` con `INSERT ... SELECT`, y
el orden físico salió distinto al del TSV (9.998.803 de 10M filas en otra
posición, 1830 MB contra 1785 MB) aun con `synchronize_seqscans` apagado.
Recargada desde el TSV como `run.sh`, quedó idéntica fila por fila.

Las 43 contra la línea base: media geométrica 0,815, suma 97,0 s.

## Resultado: `sum(col + k)` desde el catálogo

q30 son noventa `SUM(ResolutionWidth + i)`. Con argumentos de expresión en
el pushdown y la regla afín, sale entera de los metadatos: 5.088 → 9,8 ms.
La primera versión tardaba 413 ms: cada uno de los 90 agregados buscaba el
segmento en el catálogo por separado, 13.770 scans para 153 stripes; ahora
se busca una vez por columna.

Las 43 contra la línea base: media geométrica 0,699, suma 109,9 → 91,1 s.

## Resultado: filas repetidas descartadas en el scan

q9, q10 y q19 pasan casi todo el tiempo en el executor de Postgres, no en
el scan: en q9 el scan tarda 1,5 s y el Sort de 10M filas `(RegionID,
UserID)`, que vuelca 254 MB a disco con `work_mem` de 4 MB, otros 5 s. Con
un `SELECT DISTINCT` previo escrito a mano baja a 2,3 s: hay 1,54M pares
distintos.

Si todos los agregados de un GROUP BY ignoran duplicados (`DISTINCT`,
`min`, `max`), el scan descarta las filas iguales dentro de cada stripe y el
plan de arriba es el de siempre. El descarte es exacto dentro del stripe y
nada más, así que la memoria la acota un stripe; lo que se repite entre
stripes lo resuelve el DISTINCT. En q9 sobreviven 1,61M filas de 10M, casi
el piso. En paralelo cada worker descarta y ordena lo suyo, y un Gather
Merge junta: **q9 6.261 → 1.068 ms (5,9×)**, q11 489 → 219 ms, q12
587 → 244 ms, q14 1.286 → 1.050 ms. q10 y q19 tienen `count(*)` y `sum`,
que sí cuentan repeticiones; siguen siendo Sort/HashAggregate de Postgres.

Las 43 contra la línea base: media geométrica 0,655, suma 109,9 → 85,7 s.

## Resultado: GROUP BY por hash aunque no entre en work_mem

El agregado vectorizado agrupa por hash solo como agregado parcial debajo
de un Finalize, y al pasarse de memoria emite lo que tiene y sigue. Aun así
el planner no lo ofrecía si la estimación de grupos no entraba en
`work_mem`, que dejaba afuera los GROUP BY de texto con muchos valores:
q34 agrupa 10M URLs en 2,6M grupos. Ahora se ofrece si entra en memoria o
si se espera que el parcial reduzca las filas al menos a la mitad; sin eso
el Finalize recibe todo y su hash vuelca a disco (q32, 1,37M grupos en
1,37M filas, fue 1,28× más lento hasta poner la condición).

q16 2,66 → 1,54 s, q18 2,46 → 1,59 s, q34 7,15 → 5,14 s, q15 1,61 → 1,25 s,
q37 524 → 366 ms, q39 128 → 45 ms, q41 93 → 58 ms. Las 43 contra la línea
base: media geométrica 0,604, suma 109,9 → 80,4 s.

## Resultado: HAVING y `length()` en el agregado vectorizado

q28 caía al HashAggregate de Postgres por `HAVING COUNT(*) > 100000`. En el
camino paralelo el HAVING lo evalúa el Finalize, así que alcanza con que el
parcial calcule también los agregados que aparecen solo ahí; el camino
serial sigue sin HAVING. Con eso el 40% de q28 pasó a ser `length(URL)`:
`textlen()` cuenta caracteres con `pg_mbstrlen_with_len()`, una llamada a
`pg_mblen()` por carácter. En UTF-8 válido —y lo guardado lo es, Postgres
lo valida al entrar— los caracteres son los bytes que no son de
continuación (`10xxxxxx`), y eso se cuenta de a 8 bytes con un popcount.
**q28 1.975 → 1.050 ms.**

## Resultado: `LIKE` sobre FSST sin descomprimir (C2.2, ítem 6)

Los predicados ya se evaluaban una vez por entrada del diccionario, pero
cada evaluación cargaba el valor en el slot y llamaba a `textlike()` por
fmgr. Primer paso: un patrón hecho solo de literales y `%` se compara sobre
los bytes que devuelve el lector, con `memmem()`; vale con collation
determinista y en UTF-8 o codificaciones de un byte, y con `_` o escape se
evalúa como antes. q21 689 → 395 ms.

Ahí la descompresión FSST pasó a ser el 27% y `memmem` el 13%. Segundo
paso, para `%aguja%`: un autómata KMP sobre la aguja con la transición de
cada símbolo de la tabla FSST precalculada desde cada estado, así que una
entrada se recorre código por código sin escribir nada (Pop, Riedl y
Neumann; es lo que hace Vortex). Recorrido así no ganó nada: cada paso del
autómata espera la lectura de tabla del anterior. Lo que lo hizo rendir es
que en el estado 0 —casi siempre— un código que deja en 0 no cambia nada, y
saltearlos mira una tabla por código que no depende del estado. La cabecera
varlena, que FSST comprime junto con el texto, se saltea con una tabla de
256 largos que arma el lado de Postgres con sus macros: el motor no aprende
nada de varlena.

**q21 689 → 286 ms, q22 725 → 314 ms, q23 953 → 422 ms, q24 1.182 → 752
ms.** Las 43 contra la línea base: media geométrica 0,544, suma 109,9 →
75,7 s.

## Resultado: tabla de hash propia (C2.3, ítem 9, primera parte)

Las queries más lentas agrupan millones de grupos, y lo que pesa no es el
disco: q33 tarda 12,3 s con `work_mem` de 4 MB y 13,5 s con 2 GB. Con 2 GB
el HashAggregate de Postgres usa 1,75 GB para 10M grupos (175 bytes por
grupo) y tarda más de un microsegundo por grupo, entre la MinimalTuple, el
estado de cada agregado colgando y una llamada por fmgr por agregado.

`src/pg/flathash.c` es una tabla plana para claves de ancho fijo con
igualdad binaria (enteros, fechas, timestamps, bool) y agregados de estado
entero: `count`, `sum` y `avg` de smallint/integer, `min` y `max` de enteros
y fechas. Una entrada son unas pocas palabras de 64 bits en un arreglo
denso con un directorio de direccionamiento abierto, y `avg` sale con el
mismo `int8_avg()` del core. Es un agregado completo y serial: vuelca a
disco por su cuenta con el hash híbrido de nodeAgg (los grupos que entran
se siguen agregando, las filas de grupos nuevos van a particiones por hash,
y cada partición se procesa después con otros bits). Por eso, a diferencia
del camino serial con `TupleHashTable`, sí se puede ofrecer sin un Finalize
arriba.

Dos ajustes que se vieron en el EXPLAIN:

- La capacidad redondeada a potencia de dos dejaba sin usar hasta la mitad
  del presupuesto, y cada partición de q33 quedaba apenas más grande que la
  tabla: un segundo nivel entero, 11,5M filas volcadas para 10M de entrada.
  Solo el directorio tiene que ser potencia de dos.
- Elegir cuántas particiones abrir según la estimación de grupos sale caro
  justo cuando la estimación es mala: en q16 dice 39.309 y hay 1,53M, se
  abrían 4 y se volcaban 14M filas. Ahora se abren siempre todas las que
  entran en el presupuesto; una de más cuesta un buffer de 8 KB.

**q33 12,27 → 6,71 s, q31 1,35 → 1,13 s, q32 1,69 → 1,49 s**, el resto sin
cambios. Las 43 contra la línea base: media geométrica 0,529, suma 109,9 →
69,6 s. GUC `dbengine.enable_flat_hash`.

Lo que no cubre todavía: claves de texto (q34, q35), `count(DISTINCT)` (q10),
claves que son expresiones (q19, q36) y el camino paralelo. En q33 queda
además 1,5 s del Sort top-N de Postgres sobre los 10M grupos.

## Resultado: texto en la tabla propia y ANALYZE repartido

**Claves de texto.** La tabla propia acepta texto con collation
determinista, donde `texteq` compara bytes: la entrada guarda un puntero a
una copia en un arena que comparte el presupuesto. Con una sola clave de
texto y diccionario, el grupo se busca una vez por código y stripe. Cuando
el grupo no entra, sus filas del stripe se suman a un estado parcial propio
y se vuelca un registro por código al terminar el stripe, no uno por fila:
en q34 las filas volcadas bajan de 9,5M a 3,8M. Los registros de una
partición pueden traer filas o estados, y al leerlos se combinan como en un
Finalize. Con texto el registro lleva también el hash, que recalcular es la
mitad de la búsqueda; con claves enteras no, porque 8 bytes más por fila
costaban más que recalcularlo. Con más de una clave, si alguna es texto,
no se ofrece: sin caché por código se hashea el texto en cada fila y el
camino paralelo gana (q17 y q18).

**ANALYZE.** La muestra salía recorriendo el cursor desde el principio, y
como analyze.c muestrea el 13% de los bloques, era el primer 13% de la
tabla. n_distinct daba la mitad que heap con las mismas columnas (39.249
UserID contra 78.851; hay 1,53M). Ahora cada bloque muestreado se lleva a
la misma posición proporcional en el orden de las filas: n_distinct queda
como en heap y el ANALYZE de hits pasa de 5,4 a 6,5 s. Con eso el planner
elige la tabla propia en q35.

**q35 7,24 → 3,84 s, q40 1,23 → 0,94 s, q42 124 → 43 ms**; q43 90 → 105 ms
por un plan distinto con las estadísticas nuevas. Las 43 contra la línea
base: media geométrica 0,511, suma 109,9 → 66,6 s.

q34 sigue eligiendo el camino paralelo con Finalize (5,5 s; la tabla propia
tarda 3,7 s): el estimador de Postgres con 30.000 filas de muestra cree que
hay 96K URLs y hay 2,6M, y con esa cuenta el Finalize parece barato.

## Resultado: la tabla propia en paralelo, repartiendo grupos

El agregado parcial paralelo reparte stripes: cada worker agrega lo suyo y
un Finalize junta los estados. Con millones de grupos ese Finalize es la
mitad de la query (q34: 2,1 de 5,5 s), porque recibe casi una fila por fila
de entrada.

La tabla propia en paralelo reparte **grupos**: cada participante toma un
balde del hash de la clave con un contador compartido, recorre la tabla
entera y agrega solo las filas de su balde. Los grupos salen completos y el
Gather solo concatena. Lo que se paga es leer la tabla una vez por balde,
y leer es lo barato. Hay un balde por participante planeado; si se lanzan
menos workers, los que sobran los toma quien termine antes, así que el
resultado no depende de cuántos arranquen (el test lo prueba sin workers y
sin el líder). Con diccionario, el balde se calcula una vez por código.

El costo tuvo dos ajustes. El recorrido se cobra como en los demás caminos
del nodo, el 35% del scan, porque no arma tuplas. Y el Gather: con
`parallel_tuple_cost` = 0,1 por fila, 10M grupos costaban más que todo el
plan, cuando medido cuestan 0,75 s, menos que un `cpu_tuple_cost` por fila
en estas unidades. Se cobra un `cpu_tuple_cost`.

**q33 7,14 → 3,39 s, q34 5,21 → 2,50 s, q35 3,84 → 2,54 s, q16 1,55 → 0,85
s, q31 1,19 → 0,80 s, q32 1,55 → 1,06 s.** Las 43 contra la línea base:
media geométrica 0,471, suma 109,9 → 57,3 s.

## Resultado: varias claves con texto, `count(DISTINCT)` por grupo y el WHERE por stripe

**Varias claves con alguna de texto.** Estaban afuera porque el texto se
hasheaba en cada fila y el camino paralelo con Finalize ganaba. Ahora el
hash de cada texto con diccionario se calcula una vez por código y se
combina con el de las demás claves. Con eso apareció otro problema: al
procesar una partición la tabla conservaba el tamaño que había alcanzado
con la entrada entera, y le dejaba al arena de textos lugar para 8 mil de
los 150 mil grupos que cabían, así que cada partición volvía a volcar casi
todo. La tabla ahora se achica cuando al arena le falta lugar y está a
menos de la mitad, y para crecer proyecta el arena con lo que ocupan en
promedio los textos que ya tiene, en vez de reservar 64 bytes por entrada.
q17 en serie: dos niveles de particiones y 16M filas volcadas → un nivel
y 8,9M.

**`count(DISTINCT col)` por grupo**, en dos niveles. El primero agrupa por
las claves más la columna, con los demás agregados. El segundo agrupa por
las claves solas: combina esos estados y suma 1 por cada valor no nulo.
En paralelo el balde sale del hash de las claves solas, así que cada
participante emite grupos completos. Sirve si todos los `DISTINCT` son
sobre la misma columna y esa columna puede ser clave; si no, queda el plan
de Postgres. El segundo nivel vuelca estados con claves enteras, cosa que
antes no pasaba: la lectura de una partición suponía que, sin texto, todos
los registros eran filas del mismo largo, y perdía grupos. Hay un test
para ese caso.

**Corridas.** La tabla de ClickBench está ordenada por CounterID, fecha y
UserID: en q9, 8,4M de las 10M filas repiten el par (RegionID, UserID) de
la anterior, y cuando el par no entraba en la tabla se volcaba una vez por
fila. Las filas seguidas con las mismas claves de ancho fijo se acumulan
en un estado local y van a la tabla una sola vez.

**El WHERE de diccionario, por stripe.** En q11 el 70% del tiempo era
evaluar `MobilePhoneModel <> ''` fila por fila: una llamada al bridge por
código y la consulta a la caché. Ahora se leen los códigos del stripe
entero de una vez (`SegmentReader::codes`), cada código nuevo se resuelve
evaluando la cláusula sobre el valor del diccionario, y queda un byte por
fila: no pasa, pasa, o hay que evaluarla (cuando queda una cláusula que no
es de diccionario). Solo se miran las filas que el cursor va a visitar,
para no resolver códigos de gránulos que el zone map descarta. Vale para
los cuatro recorridos del nodo. Con la tabla propia en paralelo cada
participante filtra la tabla entera, y sin esto q11 y q12 quedaban más
lentas que el plan de Postgres (326 contra 214 ms).

**q10 6,73 → 1,02 s, q9 0,97 → 0,77 s, q17 3,07 → 1,87 s, q18 1,58 → 1,38
s, q15 1,22 → 0,71 s, q14 1,06 → 0,82 s, q16 0,85 → 0,69 s, q31 0,80 →
0,64 s, q32 1,06 → 0,91 s, q11 214 → 191 ms.** Las 43 contra la línea
base: media geométrica 0,432, suma 109,9 → 49,1 s. q2 y q8 (WHERE sobre un
entero) quedaron 1–2% arriba en la comparación alternando los dos `.so`,
dentro del ruido.

**`count(DISTINCT)` sin GROUP BY** sigue con el conjunto de hash de
Postgres, pero dentro de un stripe no busca lo que ya buscó: con
diccionario, cada código una vez (los códigos se leen de una vez y ni
siquiera hace falta leer el valor de las filas repetidas); sin
diccionario, la fila que repite el valor de la anterior se saltea.
**q5 1,16 → 0,62 s, q6 1,62 → 0,54 s.**

q29 (11,9 s) no cambia: la regex ya se calcula una vez por código (52 s
sin eso, 25 s con eso, en serie), pero son 3,4M valores distintos por
stripe a unos 6,5 µs cada uno en el motor de regex de Postgres.

## Resultado: claves que son expresiones y comparaciones sobre el arreglo

**Claves que son expresiones.** Una expresión del GROUP BY sobre una
columna —`ClientIP - 1` en q36, `extract(minute FROM EventTime)` en q19—
se calcula fila por fila contra el slot de la relación, en un
`ExprContext` propio: el de los argumentos de los agregados se resetea en
cada uno y liberaba la clave antes de que la tabla la copiara. El tipo
tiene que servir de clave, con una excepción: el `numeric` de `extract()`
con un campo entero (minute, hour, day, dow…). En general dos numeric
iguales pueden tener bytes distintos (1.0 y 1.00), pero Postgres arma esos
con `int64_to_numeric()`, siempre con la misma forma. Con second o epoch no:
tienen decimales, y quedan en el plan de Postgres. Para que el camino se
ofreciera hubo que mirar debajo del `ProjectionPath` que el planner pone
encima del scan para calcular la expresión. La lectura de las claves con
expresión va en una función aparte: con la rama adentro del lazo de
siempre, q18 tardaba 11% más.

**Comparaciones sobre el arreglo.** `col op const` sobre int2/int4/int8 o
date, con el operador en la familia btree del tipo (o su `<>`), se evalúa
en el prefiltro sobre el arreglo del stripe entero, sin armar el slot por
fila. La cláusula sigue en la evaluación fila por fila, que es la que usan
el scan y las filas que el prefiltro no decide; el prefiltro solo descarta,
o marca "pasa" cuando todo lo que queda del WHERE es de este tipo. q43
agrupa por `date_trunc('minute', EventTime)` y con la tabla propia en
serie tardaba 155 ms contra 135 del plan paralelo de Postgres: el tiempo
era el WHERE de cinco cláusulas enteras, no la expresión.

**q19 7,02 → 4,28 s, q36 2,68 → 1,47 s, q2 122 → 29 ms, q8 128 → 35 ms,
q42 42 → 25 ms, q41 58 → 40 ms, q39 44 → 30 ms, q37 364 → 311 ms.** Las 43
contra la línea base: media geométrica 0,364, suma 109,9 → 43,7 s.

## Resultado: top-N adentro de la tabla propia

En q33 los participantes terminaban de agrupar a los 1,7 s, y el resto
hasta 4,8 s era emitir 10M grupos por el Gather y ordenarlos para quedarse
con 10. Con `ORDER BY ... LIMIT k` constantes (sin `WITH TIES`, k hasta
10.000), cada participante guarda sus k mejores grupos en un heap y emite
solo esos; el Sort y el Limit de arriba quedan como estaban. Es correcto
porque en la tabla propia cada participante emite grupos completos: los k
primeros del total están entre los k primeros de alguno. De cada grupo se
calculan solo los valores de orden —comparados con el `SortSupport` del
operador del ORDER BY, así que vale para cualquier tipo y collation—; los
demás agregados, como el numeric de `avg`, se calculan al emitir. Con
agregados el planner no calcula el límite (`root->limit_tuples` queda en
-1): sale de las constantes del LIMIT y el OFFSET.

**q33 3,63 → 1,90 s, q19 4,28 → 3,53 s, q32 0,91 → 0,68 s, q17 2,03 → 1,62
s, q16 0,71 → 0,58 s, q31 0,64 → 0,52 s, q15 0,75 → 0,61 s, q34/q35 2,6 →
2,26 s.** Las 43 contra la línea base: media geométrica 0,344, suma 109,9 →
39,0 s. Con empates en el límite el conjunto puede diferir del de
Postgres, que tampoco lo fija (q33: todos los grupos tienen `c = 1`).
