# Plan de cierre — dbengine como proyecto de portfolio

Objetivo: dejar dbengine **cerrado como producto de aprendizaje**: completo
en lo funcional dentro de un alcance declarado, correcto, rápido donde se
pueda medir, y fácil de probar para alguien que llega al repo por primera
vez.

Mismas reglas que el plan original: correctitud antes que velocidad, cada
optimización con un número antes y después, y `make installcheck` en verde
al terminar cada etapa.

Las estimaciones son de esfuerzo aproximado para una persona, no
compromisos.

---

## C0 — Base y riesgos de correctitud (≈ 3–5 días) ✅

Antes de agregar nada, cerrar lo que hoy puede devolver datos incorrectos o
perder trabajo.

- [x] **Trabajo en curso**: correr `installcheck` completo (regress +
      isolation) sobre los cambios sin commitear (`row_reservation`,
      `scancols`, `columnar_ddl`) y commitearlos.
- [x] **pg_dump / restore — probable bug.** `stripe`, `stripe_column` y
      `relation_order` están marcadas con `pg_extension_config_dump`, pero
      pg_dump ya vuelca los datos de la tabla columnar con `COPY` (que pasa
      por el AM) y el restore los reescribe en stripes nuevos. Las filas del
      catálogo restauradas quedan indexadas por los relfilenode **del
      cluster de origen**: en el mejor caso son basura; si un relfilenode
      coincide con uno nuevo, el scan listaría stripes ajenos. Escribir el
      test (dump + restore a una base nueva + comparar resultados) y, si se
      confirma, sacar los `config_dump`.
- [x] **Remote + CI corriendo** (GitHub). Hoy el workflow nunca se ejecutó.
- [x] **LICENSE** del proyecto (PostgreSQL License o MIT; FSST ya es MIT).
- [x] `doc/recuperacion/` fuera del repo: tiene rutas y datos de la máquina
      local, no documentación del proyecto.
- [x] **Tests de merge/timetravel deterministas**: detectar que el xmin
      global está retenido y fallar con un mensaje que lo diga, en vez de
      `fusiono_algo = f`.
- [x] **CI más estricto**:
  - job con PostgreSQL compilado con `--enable-cassert` (ya encontró
    corrupción silenciosa una vez; tiene que correr siempre);
  - ASan/UBSan sobre `check-engine`;
  - `check-crash` en CI;
  - el **gate de F1 automatizado**: la suite de Postgres con el AM como
    default, comparada contra una lista de fallos esperados — un fallo
    nuevo rompe el build.

**Resultado de C0** (2026-10-24). El bug de pg_dump se confirmó: el
restore en un cluster nuevo fallaba y la tabla quedaba ilegible. El gate
automatizado, revisando a mano los fallos que no traían ningún error de
dbengine, encontró seis bugs más de resultados incorrectos (varlena
desalineados, herencia, `tableoid`, `TABLESAMPLE`, agregados polimórficos,
`ON COMMIT DELETE ROWS`), y el build con aserciones uno del propio arreglo.
La primera corrida real del CI encontró dos más del arnés: el sello
`.pgbuild-18` versionado y las salidas sin `shared_preload_libraries` que
nunca se habían generado. Todos cubiertos por tests.

## C1 — Funcionalidad que falta (≈ 3–5 semanas)

Lo que separa "storage experimental" de "table AM completo". Es lo que más
pesa en un portfolio: hoy la primera pregunta de cualquiera es "¿y si
borro una fila?".

### DELETE y UPDATE con vectores de borrado (ADR-007)

- [ ] ADR: dónde viven los borrados y cómo se versionan. Propuesta: tabla
      de catálogo `stripe_delete (relfilenode, stripe_id, chunk, bitmap)`
      en heap, con MVCC de Postgres, igual que el resto del catálogo
      (ADR-002). Los stripes siguen inmutables.
- [ ] `tuple_delete`: marca el bit; si otra transacción en curso borró la
      misma fila, `XactLockTableWait` y devolver `TM_Updated`/`TM_Deleted`
      como heap, para que funcione `EvalPlanQual`.
- [ ] `tuple_update` = borrado + inserción (el TID cambia; los índices se
      actualizan como en un UPDATE no-HOT).
- [ ] `tuple_lock` → habilita `SELECT ... FOR UPDATE/SHARE`.
- [ ] `INSERT ... ON CONFLICT` (DO NOTHING y DO UPDATE) sobre la inserción
      especulativa que ya existe.
- [ ] Todos los atajos por metadatos **restan los borrados visibles**:
      `count(*)` desde `row_count`, min/max desde zone maps (los zone maps
      quedan conservadores: un borrado los afloja, nunca los invalida).
- [ ] `VACUUM` real: reescribe los stripes con más de X% borrado y
      descarta los vectores que ya no ve nadie (horizonte de
      `GetOldestNonRemovableTransactionId`). Interactúa con time travel:
      respetar `retention_window`.
- [ ] Tests de aislamiento: update/update, delete/update, lock/update,
      ON CONFLICT con dos sesiones.
- [ ] Re-correr el gate de F1: los tests de Postgres que hoy caen por
      `DELETE` deberían pasar, y `vacuum` volver a ser ejecutable.

### El resto de la tabla "Falla claro"

- [ ] `TABLESAMPLE` (SYSTEM sobre los bloques lógicos de TID, BERNOULLI
      por fila) — barato.
- [ ] Scan hacia atrás (cursores `SCROLL`) — barato: recorrer stripes en
      orden inverso.
- [ ] `CREATE INDEX CONCURRENTLY` (`index_validate_scan`) — medio.

### Empaquetado de la extensión

- [ ] Primer release `1.0.0`; desde ahí, scripts de upgrade
      `dbengine--A--B.sql` y test de `ALTER EXTENSION UPDATE`.
- [ ] Test de `pg_upgrade` 18 → 19 (los relfilenode se preservan; verificar
      que el catálogo viaja bien).

## C2 — Rendimiento (≈ 6–10 semanas, iterativo)

Objetivo: que dbengine mejore el rendimiento de verdad, no solo que sea
correcto. Primero **medir dónde se va el tiempo** y después atacar en orden
de ganancia esperada sobre esfuerzo. Cada punto es una hipótesis hasta que
su benchmark diga otra cosa, con la misma regla de siempre: número antes y
después, y las 43 queries de ClickBench iguales a heap.

Máquina de referencia: aarch64, 4 núcleos. SIMD tiene que ser NEON o
auto-vectorizable, no AVX.

Tres hechos del código que ordenan la lista:

- `dbe_storage_read` lee **bloque por bloque y sincrónico** (`ReadBuffer`
  por página, sin prefetch) y llama a `RelationGetNumberOfBlocks` en cada
  lectura. PG 18 trae I/O asíncrono y `read_stream` está disponible.
- En caliente cada valor cruza el airlock de a uno (`dbe_segreader_get`) y
  los predicados se evalúan fila por fila con `ExecQual`.
- Las queries más lentas son agregaciones de alta cardinalidad (q9, q10,
  q19, q33–q35: 6 a 12 s cada una), donde el hash gana apenas
  1,08×–1,23× sobre PostgreSQL.

### C2.0 — Línea base ✅

- [x] ClickBench completo sin índices como referencia (3,97× media
      geométrica, 1,83× suma) y `perf` sobre q3, q24, q29, q33 y q21 en
      frío. Hallazgos en [`doc/perf-linea-base-c2.md`](perf-linea-base-c2.md):
      **abrir un segmento lo decodifica entero** (48% de q24), el `LIKE` y
      el regexp dominan la CPU de q21–q24 y q29, y en frío el 75% es
      espera de I/O. El orden de abajo ya está ajustado a eso.

### C2.1 — Lo que encontró el perfil

0. [x] **Apertura perezosa de segmentos**: el header al abrir; FOR y RLE
       se expanden cuando se pide el vector, `get(fila)` decodifica solo esa
       fila (FOR en O(1), RLE por búsqueda binaria con pista de corrida), y
       el diccionario FSST se descomprime por entrada, en un arena que se
       llena en orden. Expansión especializada por ancho, sin rellenar con
       ceros. A/B contra el binario anterior: **q24 3,25 → 1,63 s (2,0×)**,
       q3 471 → 395 ms (1,19×), el resto dentro del ruido (±1,5%). Test
       nuevo del segmento en `check-engine` (`test/engine/segment_test.cpp`).
       - Primera versión: cada entrada FSST en un tramo fijo de 8× su
         tamaño comprimido. Tocaba ~5× más páginas y q15/q26 salían 3%
         más lentas; con el arena secuencial quedaron en el ruido.
       - Encontrado de paso: **FOR mira los valores como enteros sin
         signo**, así que una columna con negativos y positivos mezclados
         (-1 y 1 quedan en los extremos del rango) sale cruda. Arreglo
         posible: probar también con el bit de signo invertido y marcarlo
         en el header. Cambia el formato (versión 3 del segmento).

### C2.1b — Victorias rápidas

1. [~] **Agregados precalculados por stripe (SMA).** Ref.: Moerkotte,
       *Small Materialized Aggregates*, VLDB 1998.
       - [x] `stripe_column.sum_value` (numeric, suma exacta en int128) para
         smallint, integer y bigint; extensión 0.2.0 con upgrade desde
         0.1.0 (los stripes viejos quedan con NULL y se leen fila por
         fila). En VectorAgg sin `WHERE`, un stripe visible entero avanza
         `sum`/`avg` de int2/int4 y `count(col)` de cualquier tipo con su
         estado parcial, como lo combinaría un Finalize; si todos los
         agregados salen así, el stripe ni se lee. **q3 473 → 14,5 ms**.
         Las 43 contra la línea base: media geométrica 0,815.
       - [ ] `sum`/`avg` de int8 (q4): el estado es interno (int128); hay
         que armarlo con su serialización.
       - [x] **Argumentos de expresión** en el pushdown: una expresión sobre
         una sola columna se evalúa fila por fila contra el slot. Y `col ± k`
         sale del catálogo: `sum(col) + k·count(col)`, solo si el zone map
         prueba que `min + k` y `max + k` entran en el tipo (si no, la fila
         por fila da el mismo overflow que heap). **q30 5.088 → 9,8 ms**.
         Las 43: media geométrica 0,699, suma 91,1 s.
       - [ ] La regla del **stripe que cumple entero**: si el zone map
         prueba que todas sus filas pasan el `WHERE`, se usan los metadatos
         sin evaluar nada (q2, q8).
2. [x] **`read_stream` + AIO de PG 18** en la carga del stripe: todos los
       segmentos que pide la query en un solo stream (`READ_STREAM_FULL`),
       con `dbe_storage_read_ranges`. Lo que más pesaba en frío no era la
       latencia sino el **volumen**: con el `read_ahead_kb` de 2 MB de la
       VM, el kernel traía de más las columnas vecinas (q21: 559 MB leídos
       para usar 271). Ahora cada lectura avisa `POSIX_FADV_RANDOM`, que
       solo sirve con `io_method` sync o io_uring (con worker leen los
       procesos de I/O). En frío, 20 queries: media geométrica 0,882 con
       worker y **0,735 con io_uring**; q21 3,0×, q22 2,8×, q25 4,6×, q4
       3,5×. En caliente, sin cambios. La `BufferAccessStrategy` queda
       pendiente: con 128 MB de `shared_buffers` la tabla no entra igual.
3. [~] **`count(DISTINCT)` por unión de diccionarios**: en un segmento con
       diccionario, las entradas *son* los valores distintos del stripe; sin
       filtro y con todo visible, alcanza con unir diccionarios (q6). Y
       combinar los conjuntos parciales para correr en paralelo (q5, que
       hoy pierde 0,79×). Hecho a medias: dentro de un stripe cada código
       se busca una vez y sin diccionario se saltea la fila que repite la
       anterior (q5 1,16 → 0,61 s, q6 1,62 → 0,53 s). Falta el paralelo.
4. [ ] **Experimento: clustering multicolumna (Z-order / Hilbert)** en
       `relation_copy_for_cluster`, con las columnas normalizadas por
       cuantiles. Revisa "una tabla se ordena por una sola cosa":
       `bench/f6-clustering.sh` ya tiene el caso (`uid` contra `ts`). Si no
       gana, se documenta como negativo.

### C2.2 — Trabajar sobre los datos comprimidos

5. [x] **Evaluar una vez por entrada del diccionario**: predicados (`LIKE`,
       `<> ''`, `=`) y expresiones inmutables (`length`, `REGEXP_REPLACE`,
       `extract`) se calculan por entrada y se guardan en un arreglo
       indexado por código. Ataca q21–q23, q28, q29 (20,8 s; 4,7 s son el
       regexp), q11–q15, q37–q43.
       - [x] **Predicados** (`src/pg/dictqual.c`, Custom Scan y pushdown de
         agregados): una cláusula sin funciones volátiles que mira una sola
         columna de largo variable se evalúa la primera vez que aparece cada
         código del stripe; las demás filas leen un byte. Si ninguna otra
         cláusula mira esa columna, ni siquiera se lee el valor de las filas
         cuyo código ya se conoce. GUC `dbengine.enable_dict_quals`. A/B con
         la GUC: q21 1,59×, q22 1,78×, **q23 2,2×**, q24 1,34×, q26 1,4×,
         q27 1,35×. Contra la línea base, las 43: media geométrica 0,887.
       - [x] **Expresiones de la salida del Custom Scan**: si alguna
         expresión del targetlist califica, el nodo proyecta por su cuenta
         (sin EvalPlanQual) y la calcula una vez por código; el valor se
         copia a un contexto que dura el stripe. **q29 21,0 → 11,85 s
         (1,77×)**, el resto sin cambios. Las 43: media geométrica 0,879
         contra la línea base, suma 109,9 → 96,8 s.
       - [x] `length(URL)` de q28: no hacía falta cachear por entrada. Lo
         que pesaba era el HAVING, que dejaba q28 afuera del agregado
         vectorizado, y `pg_mbstrlen`; ahora el HAVING lo evalúa el
         Finalize y `length()` cuenta UTF-8 de a 8 bytes. q28 1,97 → 1,05 s.
       - De paso: el EXPLAIN de `DbengineVectorAgg` dentro de una subconsulta
         fallaba ("invalid attnum 2 for relation v"): setrefs.c no corrige
         los Var de `custom_private`.
       - [x] **Filas repetidas descartadas en el scan**
         (`src/pg/rowdedup.c`): debajo de un GROUP BY con agregados que
         ignoran duplicados, exacto por stripe y con Gather Merge en
         paralelo. GUC `dbengine.enable_scan_dedup`. **q9 6,26 → 1,07 s**,
         q11 y q12 2,2×. Las 43: media geométrica 0,655, suma 85,7 s.
       - [x] **GROUP BY por hash aunque no entre en work_mem**: el parcial
         vuelca solo, así que se ofrece si entra en memoria o si reduce al
         menos a la mitad. q16 1,73×, q18 1,54×, q34 1,39×, q39 2,8×. Las
         43: media geométrica 0,604, suma 80,4 s.
6. [x] **`LIKE` sobre FSST sin descomprimir** (autómata sobre los códigos
       de símbolo): 2,5×–17× sobre descomprimir y comparar según Pop, Riedl
       y Neumann (TUM). Vortex lo implementa para `prefix%` y `%needle%`,
       que son los de ClickBench. Revisa "FSST es solo tamaño".
       Hecho para `%aguja%` (`SegmentReader::dict_contains`), con los demás
       patrones de literales y `%` comparados por bytes con `memmem`. q21
       2,4×, q22 2,2×, q23 2,3×, q24 1,6× sobre lo que había; las 43: media
       geométrica 0,544, suma 75,7 s. Ver perf-linea-base-c2.md.
7. [~] **Predicados sobre enteros comprimidos**: comparar contra
       `const − min` en FOR sin reconstruir el valor, y salir con un vector
       de selección; `ExecQual` queda para el residuo. Hecho el vector de
       selección: `col op const` sobre enteros y fechas se evalúa sobre el
       arreglo del stripe (q2 4,2×). Falta compararlo sin descomprimir y
       aplicarlo también en `DbengineColumnarScan`, que sigue fila por fila.

### C2.3 — Lo grueso

8. [~] **Top-N con materialización tardía y umbral dinámico** para
       `ORDER BY ... LIMIT k`: calcular solo la clave de orden y el filtro,
       mantener los k mejores, descartar gránulos cuyo min/max no puede
       entrar y leer las demás columnas **solo de las k filas** por TID.
       Ataca q24 (3,3 s, pierde 0,64×), q25–q27 y `SELECT * LIMIT`. Revisa
       el veredicto "estructural" sobre q24. Ref.: DuckDB 1.3 (3–10× con
       LIMIT), Doris y DataFusion (filtros dinámicos al scan).
       Hecho el top-N sobre los grupos de la tabla propia: cada
       participante emite sus k mejores (q33 1,9×, q19 1,2×); las 43:
       media geométrica 0,344, suma 39,0 s. Falta el del scan (q24–q27).
9. [~] **Tabla de hash propia** para claves `int2/4/8` y códigos de
       diccionario: direccionamiento abierto, estados de agregado en un
       arreglo plano, sin `TupleHashTable` ni `MinimalTuple`; agregación
       parcial por worker. Ataca las queries de mayor tiempo absoluto (q9,
       q10, q16, q17, q19, q31–q36). Revisa "el hash domina": lo que domina
       es la tabla genérica. Ref.: DuckDB; Xue y Marcus 2025.
       Primera parte hecha (`src/pg/flathash.c`): claves de ancho fijo,
       count/sum/avg/min/max de enteros, serial y con volcado propio. q33
       1,83×, q31 1,20×, q32 1,14×; las 43: media geométrica 0,529, suma
       69,6 s. Después, claves de texto con estado parcial por código al
       volcar, y ANALYZE con la muestra repartida por toda la tabla: q35
       1,89×; las 43: media geométrica 0,511, suma 66,6 s. Hecho también
       el camino paralelo que reparte los grupos por hash entre
       participantes (grupos completos, Gather sin Finalize): q33 2,1×,
       q34 2,1×, q16 1,8×; las 43: media geométrica 0,471, suma 57,3 s.
       Después, varias claves con texto, `count(DISTINCT)` por grupo en dos
       niveles, corridas de claves repetidas y el WHERE de diccionario
       decidido por stripe: q10 6,6×, q17 1,6×, q15 1,7×; las 43: media
       geométrica 0,432, suma 49,1 s. Después, claves que son expresiones
       sobre una columna (con el numeric de `extract()` de campos enteros)
       y las comparaciones de enteros contra constantes sobre el arreglo
       del stripe: q19 1,6×, q36 1,8×, q2 4,2×; las 43: media geométrica
       0,364, suma 43,7 s.
10. [ ] **Decodificación por lotes** (`decode_range` de 1024 filas a través
       del airlock) con desempaquetado de bits por ancho estilo FastLanes,
       escrito para que el compilador lo vectorice en NEON.

### C2.4 — Si queda tiempo

11. [ ] **Codificaciones nuevas**: delta / delta-of-delta para enteros y
        fechas ordenadas (después de CLUSTER), ALP para `float8`, `numeric`
        como entero escalado para que entre en FOR.
12. [ ] **Catálogo compacto + caché por backend** (una fila por stripe con
        los zone maps empaquetados): overhead fijo de las queries chicas
        (q20: 2,4 ms contra 1,1 ms).
13. [ ] **Caché compartida de vectores decodificados** (los stripes son
        inmutables: invalidar es trivial). Útil para dashboards; se reporta
        aparte para no inflar ClickBench en caliente.

### Descartado o para más adelante

- **Filtros Bloom de join en tiempo de ejecución**: ClickBench no tiene
  joins y el executor no deja pasar el lado de construcción al scan sin
  reemplazar el HashJoin.
- **HyperLogLog**: cambia la semántica de `count(DISTINCT)`.
- **PG 19 eager aggregation**: ofrecer `DbengineVectorAgg` como agregado
  parcial debajo de un join cuando PG 19 sea el target.
- **Ejecución por lotes del executor** (parche de Amit Langote, apunta a
  PG 20): el airlock ya está pensado para eso; seguirlo.
- **Salida en Arrow (ADBC)** para llevar columnas a pandas o polars: es un
  protocolo de cliente, no algo que una extensión del servidor resuelva
  bien. Para eso está `COPY` binario.
- **Función `dbengine.convert()`**: `SET ACCESS METHOD` ya convierte con
  índices y permisos (ver C5.0, ítem 2).

**Criterio de cierre**: ClickBench a 10M con la media geométrica en frío y
en caliente contra heap, junto a Citus columnar y pg_duckdb **en la misma
máquina**, con las 43 queries verificadas contra heap. Las que sigan
perdiendo, explicadas.

## C5 — BI y data warehouse

El objetivo del motor es acelerar consultas analíticas de un data
warehouse, y las herramientas de BI (Power BI en DirectQuery, Tableau,
Metabase, Superset) no mandan SQL con la forma de ClickBench: modelo
estrella, expresiones de fecha, montos `numeric`, `IN` largos y top N. El
bloque sale de una revisión externa del proyecto, contrastada con el código
el 2026-10-26.

**Lo que ya está y no hace falta tocar.** Los clientes externos (psycopg,
Npgsql, JDBC) hablan el protocolo de siempre: el motor corre dentro del
servidor y el planner elige sus caminos solo. `shared_preload_libraries`
hace falta solo para el background worker de mantenimiento; los scans y
la agregación andan sin él. Los datos van al WAL (`log_newpage_buffer`),
y hay tests de recuperación tras un crash (`test/crash`) y de
`pg_dump`/restore entre clusters (`make check-dump`). Las tablas columnares
funcionan como particiones (`columnar_compat`, `columnar_scan`) y guardan
`jsonb`. `ALTER TABLE ... SET ACCESS METHOD dbengine_columnar` convierte una
tabla heap existente conservando la clave primaria (probado a mano; falta
el test). Ya hechos en C2: claves que son expresiones sobre una columna
(`date_trunc`, `extract`, aritmética) y `count(DISTINCT)` por grupo y
global.

**Cómo se trabaja este bloque.** Lo mismo que en C2: cada cambio se mide
contra las 43 de ClickBench y contra el set de BI-0, no solo contra lo que
toca; cada camino nuevo lleva su GUC; la generalidad nueva va en caminos
aparte para no cargar el lazo caliente (q18 tardaba 11% más con una rama
de más); y los caminos que dependen de estimaciones arrancan con umbrales
conservadores.

### C5.0 — Medir antes de construir

1. [ ] **Benchmarks de BI estándar en vez de inventar el set.** SSB (Star
       Schema Benchmark: 13 consultas sobre un esquema estrella, con
       `sum(lo_extendedprice * lo_discount)` y filtros de dimensión) y de
       TPC-H las de una sola tabla que más se parecen a un tablero, Q1 y
       Q6, con montos `decimal`. Son conocidos, reproducibles y comparables
       con otros motores; un set armado a mano mide lo que uno ya espera.
       Más 4 o 5 consultas con la forma de Power BI (subconsultas con
       alias, `IN` de cientos de valores, top N) sobre las tablas de SSB.
       Línea base propia, y la lista de las que caen al plan de Postgres.
       Capturar el SQL real de Power BI queda para cuando haya una
       instancia de Power BI con la que probar: acá no hay.
2. [ ] **Test de conversión** en la suite: `SET ACCESS METHOD` ida y
       vuelta, con índices, permisos, defaults y vistas dependientes. Es lo
       que hace "plug and play" la adopción y hoy está probado solo a mano.
3. [ ] **Test con psycopg 3**: conexión, `COPY` de texto y binario,
       prepared statements y cursores del lado del servidor. Chico y a
       nivel de servidor no prueba nada nuevo —el protocolo es el de
       Postgres—, pero `COPY` binario sí pasa por el AM por otro camino, y
       vale como evidencia para el README.
4. [ ] **Concurrencia**: 10 a 30 consultas a la vez con pgbench. Lo que
       importa es la memoria: la tabla propia en paralelo usa `hash_mem`
       por participante y por nivel (dos con `count(DISTINCT)`), así que un
       tablero puede pedir 30 × 3 × `hash_mem`. Medir antes de calibrar.

### C5.1 — Lo que saca a las consultas de BI del camino rápido

Por orden de cuántas consultas de BI desbloquea; se reordena con lo que
muestre C5.0.

5. [ ] **Agregados sobre expresiones de varias columnas**:
       `sum(precio * cantidad)`, `sum(CASE WHEN ... THEN monto END)`. Es la
       medida central de SSB (`lo_extendedprice * lo_discount`). Hoy el
       argumento puede mirar una sola columna; la evaluación ya pasa por el
       slot, así que es cargar varias. Lo mismo para las claves (`CASE
       WHEN` sobre varias columnas).
6. [ ] **`sum`/`avg` de `bigint`** en la tabla propia y por SMA (q4). El
       estado es int128, como en Postgres.
7. [ ] **`sum`/`avg` de `numeric`** (TPC-H Q1, montos de dinero) como
       enteros escalados en int128, mientras el valor entre: si no, se
       pasa a la suma de Postgres para ese grupo. La escala del resultado
       tiene que ser la de Postgres, la mayor de las de entrada, y no la del
       typmod.
8. [ ] **`IN (lista)` sobre enteros** en el arreglo del stripe
       (`ScalarArrayOpExpr`), y descartar stripes con el Bloom para cada
       valor. Sobre texto ya se decide una vez por código del diccionario.
9. [ ] **`extract` sobre `date`** como clave (hoy solo sobre timestamp), y
       `date_trunc` sobre `date`, que da timestamp.
10. [ ] **Agregados de `float8`/`real`**: menos frecuentes en BI que en
        series de tiempo. El resultado depende del orden de suma; Postgres
        en paralelo tampoco lo fija, pero hay que dejarlo escrito.

### C5.2 — Modelo estrella

Revisa dos decisiones de "Descartado o para más adelante" de C2. En BI son
el caso normal, pero también lo más caro de construir: hay que medir C5.0
primero y saber cuánto de SSB se pierde en el join y cuánto en el scan.

11. [ ] **Tamaño del diccionario en el catálogo** para estimar ndistinct
        sin muestrear (también pedido en C2). Base de lo que sigue.
12. [ ] **Agregación debajo del join en PG 19.** PG 19 trae eager
        aggregation (`enable_eager_aggregate`): arma un `grouped_rel` por
        tabla base y le pone paths de Agg arriba del scan. Este nodo se
        puede ofrecer ahí desde `set_rel_pathlist_hook`, que corre antes
        de que se llene ese rel. En PG 18 no hay dónde engancharlo sin
        reimplementar el planner de joins: queda solo para 19.
13. [ ] **Filtro de la dimensión empujado al scan de hechos.** El executor
        no le pasa el lado de construcción del HashJoin al scan, así que
        hace falta otro mecanismo: un path parametrizado (el scan recibe la
        clave como parámetro y usa el Bloom) sirve solo con pocas claves;
        para muchas, un nodo propio de join. Empezar por un experimento
        que diga cuánto de SSB se gana; si es poco, se descarta.

### C5.3 — Después

14. [ ] **Regex rápido** para `regexp_replace`, `~` y `regexp_match` con
        patrón constante. Hoy solo lo pide q29 (11,9 s, todo en el motor de
        regex de Postgres), y en BI es raro; por eso va acá. RE2, versión
        estable más reciente, del lado C++. Su semántica no es la de
        Postgres (ARE): solo para un subconjunto que un analizador propio
        garantice equivalente —sin backreferences, sin clases que dependan
        del locale, sin alternativas donde "la más larga" y "la primera"
        difieran—; el resto, con el motor de Postgres. Tests que comparen
        los dos y GUC. Suma una dependencia externa a la CI y al
        empaquetado.
15. [ ] **Funciones de ventana: verificar**, no construir. El scan ya
        declara el orden del clustering como pathkeys, así que un `OVER
        (ORDER BY ...)` por esa columna no debería necesitar Sort.
        Comprobarlo con EXPLAIN y dejarlo en un test.
16. [ ] **Time travel**: verificar que las consultas sobre una versión
        pasada usen VectorAgg y el pruning.
17. [ ] **Ingesta continua**: lotes chicos dejan stripes chicos;
        compactación automática en el background worker, o documentar
        `merge_stripes`.

La compresión de floats y timestamps casi ordenados ya está como ítem 11
de C2.

### C5.4 — Adopción en una instancia existente

Instalar ya es "plug and play" en una instancia propia con PG 18 o 19: se
copia el `.so`, se agrega a `shared_preload_libraries` si se quiere el
background worker, y `CREATE EXTENSION`. Las tablas heap no cambian, y
`SET ACCESS METHOD` convierte una existente con sus índices: no hace
falta una función de conversión propia. No sirve en servicios gestionados
(RDS, Cloud SQL, Azure), que no dejan instalar extensiones propias.

18. [ ] **C1** (DELETE, UPDATE, MERGE): sin eso solo convienen las tablas de
        solo inserción o históricas.
19. [ ] **Imagen Docker** con PG 18 y la extensión, además del release
        `1.0.0` de C1. Es lo que permite probarlo en cinco minutos.
20. [ ] **Asesor**: extender `cluster_advice` con `workload_stats` para
        decir qué tablas conviene pasar a columnar (muchas lecturas de pocas
        columnas, pocas modificaciones) y cuáles no.
21. [ ] **Réplica por streaming y PITR**: test TAP con un standby que lea
        tablas columnares. Los datos van al WAL, pero no está probado.
22. [ ] **Un GUC para apagar todo** (`dbengine.enable`), además de los de
        cada camino, documentado como el interruptor de emergencia.

## C3 — Calidad de código (≈ 1–2 semanas, en paralelo)

- [ ] **Rediseñar `scancols.c`**: las columnas del plan viajan con el scan
      concreto (desde el Custom Scan o vía `SO_*`), no en un mapa global
      prendido y apagado por hooks. Ya causó una pérdida de datos.
- [ ] Partir `vectoragg.c` (2.906 líneas) por camino: min/max, hash,
      agrupación ordenada, planner. Ídem `columnar_write.c`.
- [ ] **Fuzzing del decodificador de segmentos** (libFuzzer sobre
      `segreader_open`/`get`): lee bytes del disco; un segmento corrupto
      tiene que dar ERROR, nunca segfault.
- [ ] Cobertura (`gcov`/`lcov`) en CI, para ver qué caminos de
      `vectoragg`/`customscan` no ejercita la suite.

## C4 — Presentación (≈ 1 semana)

Lo que convierte el trabajo en portfolio.

- [ ] **README corto**: qué es, el diagrama de capas, tres números,
      quickstart, límites. Todo lo demás a `doc/` (diario de benchmarks,
      bugs, descartes) — hoy son 1.348 líneas.
- [ ] **Docker**: `docker compose up` con PG 18 + dbengine precargado +
      un dataset chico, para probarlo en un minuto.
- [ ] Script de benchmark reproducible de punta a punta, con gráfico.
- [ ] Release con tag, CHANGELOG y (opcional) META.json para PGXN.
- [ ] 2–3 artículos: contratos no documentados del Table AM; el airlock
      C/C++ y `longjmp`; dónde gana y dónde pierde un columnar contra heap.
- [ ] Decidir idioma: README en inglés amplía el público; los ADR pueden
      quedar en español.

---

## Fuera de alcance (decidido)

- Distribución / sharding, formatos de lakehouse (Iceberg, Delta, Parquet).
- Reemplazar el executor por DuckDB: es justamente lo que dbengine **no**
  hace, y su diferencial es mantener la semántica exacta de Postgres.
- F6b (sideways cracking): resultado negativo ya documentado.
- Reordenamiento incremental de CLUSTER: solo si aparece una medición que
  lo pida.

## Orden sugerido

C0 ✅ → **C2 (rendimiento)** → C5.0 y C5.1 (BI) → C1 (DELETE/UPDATE) →
C5.2 (estrella) → C4, con C3 intercalado cuando un cambio toque el archivo
en cuestión. Lo que queda de C2 que más sirve a BI va primero: top-N (8),
el prefiltro en `DbengineColumnarScan` (7) y `sum(bigint)` (1). El foco pasó a ser que el
motor mejore el rendimiento de verdad, así que C2 va primero; C1 después,
porque DELETE obliga a revisar todos los atajos por metadatos (SMA, zone
maps, `count(*)`), y conviene hacerlo una vez con la lista completa de
atajos ya escrita. C4 al final para que los números y el README reflejen el
estado definitivo.
