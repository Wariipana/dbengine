# dbengine

Extensión de PostgreSQL que agrega storage columnar, ejecución vectorizada,
índices aprendidos y time travel, de forma transparente para cualquier
cliente estándar de Postgres.

Un solo `.so` que registra internamente 1 Table AM, 1 Index AM, los hooks
de planner, un background worker y las funciones SQL (ver R1 del plan).

**Estado: F0–F7 cerradas**, más una vuelta posterior que revisó cada
componente que se había descartado, para medirlo en el régimen correcto en
vez de en el que había quedado. De ahí salieron FSST, el zone map por
gránulo, el arreglo del fetch por TID y cuatro bugs — entre ellos uno de
resultados incorrectos. El plan completo está en
[`dbengine-plan-claude-code.md`](dbengine-plan-claude-code.md).

Sobre ClickBench (10M filas, contra heap con seis índices): **2,06×** en
caliente, **13,97×** en frío, **3,73×** menos disco, **1,88×** más rápido de
cargar, y las 43 queries dando lo mismo que heap.

```sql
CREATE EXTENSION dbengine;
CREATE TABLE eventos (id bigint, ts timestamptz, payload jsonb)
    USING dbengine_columnar;
```

## Requisitos

- PostgreSQL **18 o superior** (`postgresql-server-dev-18`)
- GCC o Clang con soporte de C++20
- Python 3 (para la guarda de arquitectura)

Verificado contra **18.6** y **19beta3**, con los mismos archivos
`expected/`. PG 19 todavía no tiene versión final (está en RC; la GA está
prevista para noviembre de 2026), así que el CI lo compila desde el tarball
de fuentes y no bloquea el merge. El 18.6 también se compila desde fuente,
con `--enable-cassert`, y ahí corre el gate de F1.

## Build

```sh
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
sudo make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config install
```

Si `pg_config` en el `PATH` ya apunta a PG 18+, alcanza con `make`.

## Cluster de desarrollo y tests

`tools/devcluster.sh` levanta un cluster propio (puerto 54318) que corre
como el usuario actual, así `make install` y `make installcheck` no pelean
con los permisos del usuario `postgres`.

```sh
tools/devcluster.sh init
tools/devcluster.sh start

export PGHOST=$HOME/.dbengine/pg18/run PGPORT=54318
make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config installcheck
```

Otros comandos: `stop`, `status`, `psql`, `destroy`.

**El cluster tiene que estar quieto.** Los tests `merge` y `timetravel`
dependen de que la fusión de stripes ocurra, y `dbe_stripe_mergeable()` exige
que el stripe preceda al xmin global: cualquier otra sesión con una
transacción larga abierta lo mantiene atrás y la fusión no toca nada. Los
tests fallan con `fusiono_algo = f` sin que haya nada roto. No es una
fragilidad del motor —la fusión hace bien en esperar— pero sí de los tests.

El motor C++ se prueba aparte, sin levantar nada:

```sh
make check-engine     # PGM y Bloom contra una referencia exacta
make check-layering   # la frontera C/C++ sigue intacta
make check-warnings   # el build no emite ni un warning
PGDATA=$HOME/.dbengine/pg18/data make check-crash   # crash y recuperación
make check-dump       # pg_dump en un cluster nuevo y restore en otro
PGSRC=~/src/postgresql-18.6 tools/f1-gate.sh   # suite de PostgreSQL contra el AM
```

`check-dump` y `f1-gate.sh` levantan sus propios clusters temporales; solo
necesitan la extensión instalada. El gate necesita además un árbol de fuentes
de PostgreSQL configurado, con `make -C src/test/regress` hecho.

`check-warnings` no es cosmético: los dos últimos warnings que aparecieron
eran símbolos que PostgreSQL 19 movió de header, y el `.so` los resolvía
recién al cargarse — o sea que el servidor no arrancaba.

## Qué anda hoy

El storage columnar es **append-only**. Lo que no está implementado falla
con un error claro; nunca devuelve un resultado incorrecto.

| | |
|---|---|
| **Anda** | `CREATE TABLE ... USING dbengine_columnar`, particionado, `INSERT`, `INSERT ... SELECT`, `COPY`, `CREATE TABLE AS`, `SELECT` (secuencial y paralelo), `CREATE INDEX` (btree y `dbengine_pgm`) + index scans, **bitmap scans**, `ANALYZE`, `VACUUM` (no-op), `VACUUM FULL`, `CLUSTER`, `TRUNCATE`, `DROP`, todos los tipos escalares y varlena, `NULL`s, valores de más de 8 kB |
| **Falla claro** | `UPDATE`, `DELETE`, `SELECT ... FOR UPDATE/SHARE`, `INSERT ... ON CONFLICT`, `TABLESAMPLE`, `CREATE INDEX CONCURRENTLY`, scan hacia atrás |

El default del cluster sigue siendo `heap` (R5): lo columnar se activa por
tabla, por esquema o por base.

**Sobre los bitmap scans**: andan y devuelven lo correcto, pero el planner
casi nunca los elige, y hace bien. Medido en frío sobre ClickBench, la misma
query resuelta por el Custom Scan tarda **29,7 ms** y por bitmap scan **920,8
ms**. Un bitmap ordena TIDs para convertir accesos aleatorios en secuenciales,
que es el problema de un heap; en columnar el scan ya es secuencial y lo caro
es reconstruir filas sueltas.

Que anduvieran costó un detalle no documentado: `DBE_ROWS_PER_TIDBLOCK` —
cuántas filas se mapean a un "bloque" ficticio para fabricar TIDs— estaba en
2047, el máximo que acepta GIN. Pero `TIDBitmap` es más estricto y valida los
offsets contra `MaxHeapTuplesPerPage`, que con páginas de 8 kB son 291. Con
2047 el bitmap scan no era lento: era **imposible**.

### Cómo está construido

- **Un stripe** es un lote de filas escrito de una vez, con **un segmento
  por columna** (`[seg col1][seg col2]...`). Leer una columna es leer su
  rango de bytes: eso es el column pruning.
- Cada segmento se **comprime solo**: se arman las candidatas, se comparan
  los tamaños reales y gana la menor, con el formato crudo siempre como
  piso. Un byte en el header dice cuál quedó.
- Los segmentos viven en un **archivo lógico direccionable por bytes sobre
  el fork `main`**, vía buffer manager, así siguen valiendo WAL, recovery,
  `DROP`, `TRUNCATE` y tablespaces
  ([ADR-001](doc/adr-001-storage-fisico.md)).
- Las escrituras pasan por un **buffer orientado a filas** que se vacía a un
  stripe (R8), porque Postgres llama a `tuple_insert()` una vez por fila. El
  buffer junta filas de **toda la transacción**, no de un comando: el stripe
  guarda el command id de cada fila para que la visibilidad siga siendo
  exacta. Un `COPY` o un `INSERT ... SELECT` tienen un solo comando y no
  pagan ese segmento.
- La **visibilidad es por stripe**, decidida con `insert_xid`/`insert_cid`
  y no con el MVCC de la fila de metadatos
  ([ADR-002](doc/adr-002-visibilidad-de-stripes.md)). Es lo que permite
  escribir tarde y verse temprano.
- Los **TIDs son lógicos**: se derivan del número de fila, no del bloque
  físico. Los números de fila se reservan al **abrir** el buffer de
  escritura, no al flushearlo, para que `tuple_insert()` pueda dejar el TID
  definitivo en el slot — el executor lo usa enseguida para las entradas de
  índice. Los stripes son inmutables, así que un TID nunca cambia de
  significado.
- El catálogo interno se indexa por **relfilenode**, no por OID de
  `pg_class`: una reescritura (`ALTER TABLE ... TYPE`, `TRUNCATE`) cambia el
  relfilenode dejando el OID igual. Para introspección:
  `pg_relation_filenode('tabla'::regclass)`.

### Contratos del Table AM que la API no documenta

Los encontró el gate de F1 (la suite de regresión de Postgres corriendo con
`default_table_access_method = 'dbengine_columnar'`), no los tests propios.

| Contrato | Qué pasa si no se cumple |
|---|---|
| Los valores se interpretan con el descriptor **del slot**, no con `RelationGetDescr()` | `ATRewriteTable` usa `tab->oldDesc`: un segmento de `text` se lee como `integer` y el Datum apunta a cualquier lado |
| `SnapshotDirty` obliga al AM a escribir `xmin`/`xmax` en cada fila | `InitDirtySnapshot` solo fija el tipo; con la basura del stack, `ON CONFLICT` gira para siempre |
| `tuple_insert()` tiene que dejar el TID definitivo en el slot | las entradas de índice apuntan todas a la misma fila |
| `scan_end()` desregistra el snapshot si hay `SO_TEMP_SNAPSHOT` | "snapshot reference was not closed" al cerrar la transacción |
| Los catálogos internos van `USING heap` explícito | con el AM como default del cluster, `CREATE EXTENSION` no puede ni instalarse |

## La frontera C / C++

Es la restricción más importante del código y la que rompe todo si se
ignora (R2 del plan). Postgres maneja errores con `ereport`, que hace
`longjmp`: **un `longjmp` que atraviesa frames de C++ no ejecuta
destructores**. En el sentido inverso, una excepción de C++ que escapa a un
frame de C es comportamiento indefinido.

El árbol está partido en tres piezas:

| Pieza | Archivos | Puede usar |
|---|---|---|
| **Capa C** | `src/pg/*.c` | APIs de Postgres, `ereport`, `PG_TRY`, `palloc` |
| **Airlock C → C++** | `src/pg/bridge.cpp` | `try`/`catch`. Nada de Postgres. |
| **Capa C++** | `src/engine/**` | Solo C++ y `dbengine/pgshim.h` |

Reglas, todas verificadas por `tools/check_layering.py` en cada `make`:

1. `src/engine/` no incluye headers de Postgres.
2. `ereport`/`elog` no aparecen en ninguna unidad de traducción de C++.
3. `PG_TRY` vive **solo** en `src/pg/pgshim.c`, que es C puro — así el
   `setjmp`/`longjmp` nunca puede saltar por encima de un destructor.
4. La capa C++ no usa `palloc`/`pfree` (fallan con `ereport`), usa
   `dbe_shim_alloc`/`dbe_shim_free`.
5. La capa C no usa `malloc` directo; la excepción es `pgshim.c`, que lo
   envuelve a propósito.

Los errores cruzan la frontera como `DbeStatus` **por valor** (POD, sin
asignación dinámica). La conversión a `ereport(ERROR)` ocurre en un único
lugar, `src/pg/errors.c`, siempre con los frames de C++ ya desenrollados.

### Cómo se verifica

`dbengine_bridge_selftest()` ejecuta la batería completa. Cada caso mide
**cuántos destructores de C++ corrieron**, que es la evidencia directa de
que el `longjmp` no cruzó:

```
         test_name          | passed
----------------------------+--------
 cxx/anidada_3_frames       | t
 cxx/bad_alloc              | t
 cxx/no_estandar            | t
 cxx/runtime_error          | t
 cxx/sin_excepcion          | t
 motor/sigue_respondiendo   | t
 pg/ereport_no_hace_longjmp | t
 pg/ida_y_vuelta_sin_error  | t
```

El caso `pg/ereport_no_hace_longjmp` es el central: el motor C++ llama a un
shim de C que hace `ereport(ERROR)`; el `longjmp` se detiene en el shim, el
error vuelve como valor y el frame de C++ desenrolla con una excepción
normal. Si el `longjmp` cruzara, el contador de destructores daría 0.

### Contrato al capturar un error de Postgres

`pgshim.c` hace `CopyErrorData()` + `FlushErrorState()`, que **no** libera
los recursos que Postgres tenía tomados (buffers, locks). Por eso el motor
está obligado a tratar `DBE_ERR_PG` como *desenrollar ya mismo*: propagarlo
hasta el airlock sin volver a llamar a ningún shim. La capa C re-lanza con
`ereport(ERROR)` y ahí el abort de transacción libera todo por resource
owner. El test de `bridge.sql` cubre este camino, incluido el caso de
subtransacción.

## Compresión (F2)

| codificación | cuándo gana | ejemplo |
|---|---|---|
| **frame of reference** + bit-packing | enteros de rango acotado | ids, fechas, precios |
| **RLE** | corridas de valores iguales | columnas ordenadas o constantes |
| **diccionario** | largo variable de baja cardinalidad | estados, países, categorías |
| **diccionario + FSST** | ídem, con el diccionario grande | URLs, títulos, referers |
| **crudo** | cuando ninguna gana ≥10% | datos de alta entropía |

500k filas, 8 columnas (`bench/f2-compresion.sh`):

| | tamaño | bloques leídos | scan `count(*)` |
|---|---|---|---|
| columnar comprimido | **12 MB** | **1476** | 138 ms |
| columnar crudo | 37 MB | 4786 | 130 ms |
| heap nativo | 40 MB | 5155 | 27 ms |

**3,5× menos espacio y menos I/O que heap.** El tiempo de scan todavía no
mejora: con los datos en cache el scan es CPU-bound reconstruyendo fila por
fila (R4), así que la ganancia de I/O no aparece en el reloj. Eso lo
resuelve F3.

`SET dbengine.enable_compression = off` guarda los valores crudos, para
medir la ganancia sin cambiar nada más.

### FSST sobre el diccionario

El diccionario resuelve la repetición de valores enteros, pero no la
redundancia *dentro* de cada valor: 800.000 URLs distintas que empiezan todas
con `http://` siguen ocupando lo que ocupan. FSST (Fast Static Symbol Table)
comprime justamente eso — arma una tabla de símbolos de subcadenas frecuentes
y reemplaza cada una por un byte.

Se aplica al **contenido del diccionario**, no valor por valor: se descomprime
entero una vez al abrir el segmento y de ahí en más el acceso es idéntico a
`kEncDict`. Se elige por medición, con un umbral de 10% de ganancia mínima —
sobre ClickBench lo toma en 15 columnas (1028 MB) y lo declina correctamente
en 24 de diccionario chico (36 MB), donde la tabla de símbolos de ~560 bytes
no se amortiza.

| | sin FSST | con FSST | |
|---|---|---|---|
| tamaño de la tabla | 2421 MB | **1787 MB** | 26% menos |
| contra heap | 2,82× | **3,82×** | |
| `GROUP BY URL`, en frío | 12.284 ms | 10.219 ms | 1,20× |
| `URL LIKE '%google%'`, en frío | 7.865 ms | 5.984 ms | **1,31×** |
| `GROUP BY SearchPhrase`, en frío | 1.207 ms | 1.236 ms | sin cambio |
| `min(Title), max(Referer)`, en caliente | | | **0,73×** |

**Es sobre todo tamaño.** El rechazo original decía "sería tamaño, no
velocidad" y esa frase estaba bien; el error fue no medir en frío, donde el
tamaño sí se convierte en tiempo. En caliente es neutro a levemente negativo,
y el caso que pierde —`min(Title), max(Referer)`, que toca cada entrada del
diccionario y casi no hace nada más— es exactamente donde el análisis
predecía que aparecería el costo de descomprimir.

La implementación está vendorizada sin modificar en `src/engine/compress/fsst/`
(MIT). Nota: en aarch64 su camino AVX512 queda inerte.

Los **zone maps** (min/max por segmento) se guardan en
`dbengine.stripe_column` y los usa el Custom Scan de F3 para descartar
stripes enteros. Ver
[ADR-003](doc/adr-003-donde-vive-el-block-pruning.md).

```sql
SELECT attnum, encoding, null_count, min_value, max_value
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('mi_tabla');
```

## Custom Scan (F3)

El seq scan del Table AM nunca se entera de qué columnas ni qué predicados
tiene la query — las condiciones se evalúan *después* de que el AM devolvió
la fila. El Custom Scan sí, y con eso hace tres cosas que el Table AM no
puede:

- **column pruning**: decodifica solo las columnas que la query usa;
- **descarte de stripes**: con los zone maps de F2, saltea los que el
  predicado excluye con certeza — sin leerlos del storage;
- **late materialization**: evalúa el predicado con las columnas que mira, y
  decodifica el resto de la fila solo si la pasa.

```
EXPLAIN (ANALYZE) SELECT id, nota FROM eventos WHERE id BETWEEN 100 AND 200;

 Custom Scan (DbengineColumnarScan) on eventos
   Columnas leídas: id, nota
   Filter: ((eventos.id >= 100) AND (eventos.id <= 200))
   Rows Removed by Filter: 1899
   Predicados sobre zone maps: 2
   Stripes descartados: 9
```

El `Filter:` lo imprime el nodo, no `explain.c`: el predicado no vive en
`plan.qual` sino adentro, que es el único lugar desde donde se puede
evaluar antes de terminar de armar la fila.

500k filas (`bench/f3-customscan.sh`):

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **1 ms** | 63 ms | 22 ms |
| `sum(monto)` | 29 ms | 65 ms | 30 ms |
| `count(DISTINCT region)` | 173 ms | 223 ms | 154 ms |
| `count(*) WHERE id BETWEEN 100 AND 200` | **14 ms** | 60 ms | 27 ms |
| `sum(id) WHERE id > 490000` | **11 ms** | 63 ms | 24 ms |
| `count(*) WHERE nota = 'nota-42'` | **11 ms** | 59 ms | 25 ms |

A 500k filas, sin filtro y sin pushdown posible (`count(DISTINCT)`) queda a
la par de heap — pero eso es un artefacto: a esa escala todo entra en cache
y se mide CPU, no I/O.

**A 100M filas** (2862 MB columnar contra 7211 MB heap), soltando el page
cache antes de cada medición con `posix_fadvise` (`bench/f3-escala.sh`):

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **2,6 ms** | 30,4 s | 76,5 s |
| `sum(monto)` | **8,1 s** | 30,5 s | 76,6 s |
| `WHERE id BETWEEN 100 AND 200` | **18 ms** | 30,4 s | 76,6 s |

Bloques leídos, que no dependen del hardware:

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **38** | 1.466.100 | 3.692.259 |
| `sum(monto)` | **55.237** | 1.466.069 | 3.692.264 |
| `WHERE id BETWEEN 100 AND 200` | **448** | 1.099.476 | 2.769.206 |

En frío la diferencia es de órdenes de magnitud porque casi no hay I/O que
hacer: `count(*)` sale del catálogo y el predicado selectivo descarta los
stripes antes de leerlos.

### Agregación empujada al scan

El nodo `Agg` pide una fila por vez: para agregar 100M filas hay que
construir 100M tuplas y tirarlas. Este nodo alimenta las funciones de
transición directamente, y para `count(*)`, `min` y `max` ni siquiera lee
datos — el `row_count` y el zone map de cada stripe alcanzan.

| consulta (100M filas, en caliente) | F3 | sin pushdown | heap |
|---|---|---|---|
| `count(*)` | **2,2 ms** | 2235 ms | 2697 ms |
| `min(id), max(id)` | **18 ms** | 3805 ms | 2989 ms |
| `sum(monto)` | **1948 ms** | 3914 ms | 4054 ms |
| `sum(id) WHERE id BETWEEN 100 AND 200` | **14 ms** | 53 ms | 2576 ms |
| `count(*) WHERE id > 99000000` | **24 ms** | 90 ms | 2479 ms |
| `sum(monto) WHERE region = 'norte'` | **2633 ms** | 3635 ms | 3830 ms |

Tres mecanismos distintos:

- **`count(*)`, `min` y `max` salen del catálogo** — el `row_count` y el zone
  map de cada stripe alcanzan.
- **El resto se lee por columna**: de cada stripe se pide el arreglo una vez
  y se lo recorre en un lazo propio, sin armar un `TupleTableSlot` por fila
  ni cruzar el airlock por valor. Medido: 54 ns de los 74 ns por fila.
- **En paralelo el nodo hace de Partial Agg**, entrega el estado de
  transición (serializado si es `internal`) y el `Finalize Agg` lo combina.

- **Con `WHERE` el predicado se evalúa adentro del nodo**, contra un slot que
  trae solo las columnas que el predicado mira. Los stripes que el zone map
  descarta ni se miran, y los atajos por metadatos se apagan: el `row_count`
  del stripe cuenta todas sus filas, no las que pasan el filtro.

```
EXPLAIN (ANALYZE) SELECT sum(monto) FROM eventos WHERE region = 'norte';

 Custom Scan (DbengineVectorAgg) on eventos
   Agregados empujados al scan: 1
   Filtro: (eventos.region = 'norte'::text)
   Predicados sobre zone maps: 1
   Filas agregadas: 75000
   Filas descartadas por el filtro: 225000
   Stripes descartados: 0
```

**La semántica no se reimplementa**: se usan las funciones de transición de
`pg_aggregate` con la misma inicialización que `nodeAgg`, así que
`sum(int4)` desborda igual y `avg(numeric)` redondea igual. Sin `GROUP BY`,
`HAVING`, `DISTINCT` ni `FILTER` — todo eso cae al plan normal, igual que
las tablas particionadas.

Ese "cae al plan normal" era la limitación más cara del motor, y ClickBench
le puso número: **37 de sus 43 queries tienen `GROUP BY`**. Medido sobre
cinco de ellas, entre el 74% y el 92% del tiempo se iba en agrupar, no en
leer — el scan columnar entregaba las columnas en 326–568 ms y el resto lo
gastaba `HashAggregate` sobre tuplas rearmadas de a una.

**Con `GROUP BY` ahora hay pushdown, pero sólo sobre la columna de
clustering.** La condición es que el scan ya entregue las filas en orden de
grupo: con eso los grupos quedan contiguos y se agregan de corrido, llevando
un solo grupo abierto. No hay tabla de hash y la memoria no depende de
cuántos grupos haya.

Sobre 5M filas y 200k grupos:

| plan | tiempo |
|---|---|
| `HashAggregate` (sin clusterizar) | 1088 ms |
| `GroupAggregate` sin `Sort` (clusterizada) | 610 ms |
| agregación empujada al scan | **165 ms** |

6,6× entre las puntas, y el resultado final queda por debajo de lo que
costaba sólo leer y materializar la columna (172 ms), porque el nodo nunca
arma una tupla.

Agrupar por cualquier otra columna sigue cayendo al `HashAggregate` de
Postgres, que para eso ya está bien. El camino paralelo también: cada worker
ve un subconjunto de stripes, así que un grupo puede quedar partido entre
varios y haría falta emitir estados de transición parciales.

**Es una ganancia condicionada, y la condición no es gratis.** Depende de que
la tabla esté clusterizada por la columna de agrupación, y una tabla se
ordena por una sola cosa. Sobre ClickBench el efecto es espectacular donde
aplica —q18 pasa de 2376 ms a 4,0 ms— y el balance de las 43 queda igual,
porque reordenar por la columna de agrupación de unas le saca la correlación
a las otras. Está medido en la sección de ClickBench.

### Agrupar sin orden: tabla de hash

Por eso hay una segunda forma, que no pide nada del layout: una tabla de
hash adentro del nodo. Aplica a cualquier columna, en serie o en paralelo
—cada worker agrupa sus stripes y un `Finalize` junta los estados parciales—
y lo que ahorra es armar un `TupleTableSlot` por fila para pasárselo al nodo
de arriba. Contra el `HashAggregate` paralelo de Postgres da **1,08×–1,23×**:
real, pero mucho menos que la versión ordenada, porque el hash domina.

No vuelca a disco. El planner solo la ofrece si la estimación de grupos entra
en `work_mem`; cuando no entra, `nodeAgg` sí sabe volcar y es el plan
correcto.

**Sobre texto con diccionario hay bastante más.** Las filas que comparten
código del diccionario comparten el string, así que se resuelve una vez por
entrada y no una por fila; después cada fila es leer su código y seguir un
puntero. En un stripe de 65.536 filas de ClickBench hay 12.856 URLs
distintas y 2.508 frases de búsqueda: entre **5× y 26× menos búsquedas de
hash**. Medido sobre las 10M filas reales, `GROUP BY SearchPhrase` da 1,39× y
`GROUP BY URL` 1,42×, contra 1,1×–1,2× de las columnas numéricas.

Lo que se cachea es el payload del grupo y **no** la entrada de la tabla: la
tabla de hash guarda sus entradas en un arreglo que se reubica al crecer, así
que un puntero a entrada deja de valer apenas entra otro grupo. El payload
cuelga de la `MinimalTuple`, que no se mueve.

### Un costo que miente gana siempre

La primera versión del nodo con hash resultó **2,3× más lenta** que el plan
normal y el planner la elegía igual. La causa no estaba en el nodo:
`dbe_all_minmax()` recorre el target list suponiendo que todo es `Aggref`, y
con una columna de agrupación adentro contestaba que la query se podía
resolver desde los zone maps. Eso dejaba el costo en `cpu_operator_cost *
100` — **0,2** — contra los ~55.000 del plan correcto.

Un costo mal calculado no se comporta como una optimización que no sirve: no
compite, gana siempre, y gana también contra planes el doble de rápidos. Las
dos formas de agrupar tienen ahora costos distintos y medidos —la ordenada
cuesta una comparación por fila, la de hash una búsqueda— porque darles el
mismo hacía que la barata perdiera contra el plan normal.

### `count(DISTINCT)`

Se resuelve con un conjunto de hash sobre la columna, sin pasar por la
función de transición ni armar una tupla por fila: **2,27×** sobre texto,
1,08×–1,17× sobre enteros.

Sin `GROUP BY` solamente —por grupo haría falta un conjunto por grupo— y sin
paralelismo: combinar dos conteos parciales daría de más, y combinar los
conjuntos es otra cosa que este nodo no emite.

`SET dbengine.enable_agg_pushdown = off` lo desactiva.

### Late materialization

El predicado se evalúa con las columnas que mira; el resto de la fila se
decodifica solo para las que lo pasan. Medido a 100M filas con
`SELECT * FROM eventos WHERE region = 'norte'` —que deja pasar el 25% y **no
descarta ningún stripe**, porque `region` se repite en todos:

| | ms |
|---|---|
| con late materialization | **3202** |
| forzando decodificar `nota` siempre | 4576 |
| el mismo scan por el Table AM | 6692 |
| heap | 3440 |

La contraprueba mete `nota IS NOT NULL` en el predicado: no filtra nada,
solo obliga a leer la columna para las 100M filas en vez de para las 25M que
pasan. **30% menos**, y es lo que separa perder de ganarle a heap en una
consulta que devuelve la fila entera.

El pruning es **conservador por construcción**: solo descarta cuando el
rango `[min, max]` excluye con certeza, y las cláusulas siguen evaluándose
igual — nunca reemplaza el predicado, solo evita leer.

`SET dbengine.enable_vectorized = off` vuelve al seq scan del Table AM.

Dónde vive el predicado —`plan.qual`, `custom_exprs` o `custom_private`— no
es un detalle: `setrefs.c` les hace cosas distintas y cada nodo necesita una.
Está en [ADR-004](doc/adr-004-donde-vive-el-predicado.md).

### Particiones

Cada partición recibe su propio nodo, así que el pruning de particiones y el
de stripes se componen: primero PostgreSQL descarta particiones enteras por
la clave, después el zone map descarta stripes dentro de las que quedan.
Anda también bajo `Parallel Append` y con `enable_partitionwise_join`.

```
EXPLAIN (COSTS OFF) SELECT id FROM p WHERE region = 'norte';

 Append
   ->  Custom Scan (DbengineColumnarScan) on p1 p_1
         Columnas leídas: id, region
         Filter: (p_1.region = 'norte'::text)
         Predicados sobre zone maps: 1
   ->  Custom Scan (DbengineColumnarScan) on p2 p_2
         ...
```

**Lo que falta de F3**: `GROUP BY` y join vectorizados, y que la agregación
empujada cruce un `Append` — hoy un `count(*)` sobre una tabla particionada
usa `Append` + `Agg` normal, aunque cada partición sí se lea con el nodo
columnar.

## Índice aprendido: PGM (F4)

Un btree guarda un árbol de páginas con punteros. Un PGM-Index guarda las
claves ordenadas y un **modelo**: rectas por tramos que predicen en qué
posición está una clave. La predicción tiene un error acotado por
construcción, así que después alcanza con mirar una ventana chica.

```sql
CREATE INDEX ON eventos USING dbengine_pgm (id);
```

2M filas, sobre una tabla heap para que lo que se compare sea el índice
(`bench/f4-pgm.sh`):

| clave | tamaño PGM | tamaño btree | 10k sondas PGM | 10k sondas btree |
|---|---|---|---|---|
| secuencial | **31 MB** | 43 MB | **476 ms** | 602 ms |
| aleatoria | **38 MB** | 43 MB | **500 ms** | 1391 ms |
| clusterizada | **31 MB** | 43 MB | **501 ms** | 601 ms |

El build respeta `maintenance_work_mem`: las claves van a un `tuplesort` que
derrama a disco y el modelo se arma de forma incremental, así que nada
necesita estar entero en RAM. A 100M filas con 64 MB de `maintenance_work_mem`
son 3 min 12 s y 1530 MB de índice.

**El modelo nunca es la autoridad.** Devuelve una ventana; antes de creerle
se verifica con dos comparaciones exactas si la respuesta puede estar
afuera, y si puede, se hace la binaria completa. El resultado es el correcto
ande bien o mal el modelo — lo único que cambia es cuánto tarda. El gate de
la fase es una property: los mismos datos con un índice PGM y con un btree
tienen que devolver exactamente lo mismo, y se comprueba sobre cinco
distribuciones y todos los bordes (`test/regress/sql/pgm.sql`).

Por qué el modelo no puede ser la autoridad, y qué pasa si lo es, está en
[ADR-005](doc/adr-005-el-modelo-nunca-decide.md).

Solo sirve para claves mapeables a un entero conservando el orden —enteros,
`date`, `timestamp`, `timestamptz`, `time`, `oid`—, es de una sola columna,
no ordena, no impone unicidad y no indexa `NULL`. Para todo lo demás está el
btree, y el error lo dice.

## Filtros de Bloom por segmento (F5)

Los zone maps min/max de F2 dejan un solo hueco, y la medición lo señala
exacto (`bench/f5-zonemaps.sh`, 2M filas, 31 stripes). No importa cuánto
podan sino **cuántos stripes se leen para nada**:

| columna | predicado | descartados | leídos | con alguna fila | **para nada** |
|---|---|---|---|---|---|
| correlacionada con la inserción | `= k` | 30 | 1 | 1 | **0** |
| baja cardinalidad | `= k` (40k filas) | 0 | 31 | 31 | **0** |
| alta cardinalidad, sin correlación | `= k` (1 fila) | 0 | 31 | 1 | **30** |

Solo la última fila deja algo que ganar. Taparla es responder "¿está esta
clave entre estas 65.536?" — **pertenencia a un conjunto**, que es lo que un
modelo chico no hace y un filtro de Bloom hace en espacio óptimo.

**La condición no es que el valor sea raro, es que esté agrupado.** Un filtro
por stripe descarta algo solo si el valor buscado *falta* en la mayoría de
los stripes. Si las M filas que matchean estuvieran repartidas sin
correlación entre N stripes, los que contienen al menos una serían
N·(1−(1−1/N)^M), y haría falta **M ≲ N** para descartar algo — un lookup casi
puntual, donde normalmente ya hay un índice.

**Pero esa aritmética supone distribución uniforme, y los datos reales casi
nunca la tienen.** Sobre ClickBench, `RefererHash = 3594120000172545465`
matchea 81.558 filas: por el cálculo de arriba tendría que aparecer en los
153 stripes. Aparece en **11**, porque el dataset viene ordenado por
`CounterID` y eso agrupa muchas otras columnas de paso. El filtro descarta
**142 de 153**:

| | heap + btree | dbengine + Bloom | |
|---|---|---|---|
| `RefererHash = k` | 160,8 ms | **48,7 ms** | 3,31× |
| `URLHash = k` | 138,0 ms | **29,9 ms** | 4,62× |

Sin los filtros, la misma query sobre la misma tabla tarda 149 ms y **pierde**
0,89× contra heap.

### Cómo se llegó a la conclusión contraria

Esta sección decía, con la fórmula y los números de arriba, que el Bloom no
descartaba nada. La medición que lo "probaba" se hizo sobre la tabla
**clusterizada por `userid`** — o sea sobre el único layout que dispersa
todas las demás columnas, el peor caso posible para un filtro por stripe,
tomado como caso general.

Y el error se sostuvo porque la explicación era buena: la fórmula es
correcta, sólo que su premisa era falsa. Una explicación convincente de un
resultado equivocado es más difícil de detectar que un resultado equivocado
solo.

Vale la pena recordar la propiedad que hace barato equivocarse en la
dirección contraria: **el error del filtro es de un solo lado**. Puede decir
"puede estar" cuando no está; nunca lo contrario. No puede cambiar un
resultado, sólo cuánto se lee.

50 lookups por igualdad sobre la columna sin correlación:

| | stripes descartados | buffers | tiempo |
|---|---|---|---|
| con filtro | 1455 de 1550 | 54.894 | **849 ms** |
| solo min/max | 0 | 92.650 | 7192 ms |

Cuesta **+25% de escritura** y **1 byte por fila por columna filtrada**. Por
eso el default —`dbengine.bloom_bits_per_row = 0`— no lo construye en todas:
solo en las columnas que el canal de feedback vio con predicados de
igualdad. Sin historial no se paga nada.

```sql
-- forzarlo para medir
SET dbengine.bloom_bits_per_row = 8;
```

Por qué un modelo aprendido no sirve para este hueco —y sí para el de F4—
está en [ADR-005](doc/adr-005-el-modelo-nunca-decide.md).

## Clustering adaptativo (F6)

Reordenar los datos por la columna que se consulta es lo que hace que los
zone maps poden. La pregunta es por cuál columna, y la única parte del
sistema que ve los predicados es el hook del planner: los anota en memoria
compartida, separando igualdad de rango porque a la reescritura le sirven
distinto —la igualdad se beneficia de agrupar, el rango de ordenar.

**También se anotan las columnas del `GROUP BY`.** Al principio no: lo único
que ganaba con el orden físico era el descarte de stripes, así que alcanzaba
con los predicados. Desde que el scan declara pathkeys y la agregación se
puede empujar, la columna por la que se agrupa es la que más gana de todas.
Sin anotarla, sobre ClickBench el consejo recomendaba `counterid` —columna
de filtro, en queries que ya ganaban— y no veía `searchphrase`, `userid` ni
`url`, que son por las que se agrupa.

```sql
SELECT columna, puntaje, indice, se_puede_clusterizar FROM dbengine.cluster_advice();
 columna | puntaje | indice | se_puede_clusterizar
---------+---------+--------+----------------------
 uid     |       9 | f6_uid | t
 ts      |       1 | f6_ts  | t

CLUSTER f6 USING f6_uid;
```

Es un **consejo**, no una acción: reescribir toma ACCESS EXCLUSIVE y eso lo
decide quien opera la base. La reescritura es la de Postgres —`CLUSTER` y
`VACUUM FULL` andan sobre tablas columnares— y de paso rearma stripes,
reelige compresión con los datos ya agrupados, recalcula zone maps y
construye los filtros de Bloom.

10M filas, 153 stripes (`bench/f6-clustering.sh`). `ts` sigue el orden de
inserción, `uid` no:

| estado | `uid = k` | `ts BETWEEN` | tamaño |
|---|---|---|---|
| como se insertó | 681 ms | 7,4 ms | 60 MB |
| clusterizada por `uid` | **7,0 ms** | 757 ms | 49 MB |
| clusterizada por `ts` | 687 ms | **7,3 ms** | 60 MB |
| por `ts` + Bloom en `uid` | **53 ms** | 7,6 ms | 60 MB |

Reordenar por `uid` lleva la consulta de 681 ms a **7,0 ms: 97×** — y le
saca a `ts` la correlación que tenía, que se va de 7,4 ms a 757 ms. Una
tabla se ordena por una sola cosa. Por eso la última fila importa: el filtro
de Bloom de F5 no depende del orden y recupera 12,8× sobre la columna que el
clustering no pudo favorecer. Son complementarios, no alternativas.

**¿Y si el workload es errático?** Es la pregunta que decide si la
adaptividad vale la pena, y se contesta mirando el consejo antes de
reescribir nada:

| workload | consejo | costo | clusterizado |
|---|---|---|---|
| repetitivo (un dashboard) | una columna, **97,6%** | 43,0M filas | 5,6M — **7,6×** |
| errático (ad-hoc) | **20% cada una** | 45,0M filas | 36,6M — 1,23× |

Un consejo plano no es una falla: es la respuesta correcta, y dice "no
reordenes". La distribución del puntaje lo anticipa antes de pagar el
ACCESS EXCLUSIVE.

Reordenar **no** mejora la compresión en general: colapsa la columna por la
que se ordena (una de baja cardinalidad pasó de 16 MB a 47 kB) y empeora un
53% la que venía ordenada por el orden de inserción. El neto depende de la
tabla.

La parte incremental sí existe, pero para **fusionar** en vez de reordenar:
`dbengine.merge_stripes()` junta stripes adyacentes con un presupuesto por
corrida, y como conserva el orden no mueve ningún TID (más abajo). Lo que
falta es el reordenamiento incremental: mover filas entre stripes sí mueve
TIDs, y eso necesita un `xmax` a nivel de stripe y una forma de sacar las
entradas viejas de los índices.

`dbengine.workload_stats()` muestra los predicados crudos y
`dbengine.workload_reset()` los borra. Todo esto requiere
`shared_preload_libraries`.

## Time travel (F7)

En un storage **append-only el version store ya está**: nada se borra ni se
pisa, así que el estado de la tabla en un momento T es exactamente el
conjunto de stripes que commitearon antes de T. Una estructura lateral que
guardara versiones sería una copia de algo que el storage ya tiene.

```sql
SELECT * FROM dbengine.time_travel('eventos', now() - interval '1 hour')
    AS (id int, val text);

SELECT * FROM dbengine.stripe_timeline('eventos');
 stripe_id | first_row_number | row_count | insert_xid |          commit_ts
-----------+------------------+-----------+------------+------------------------------
         1 |                0 |         1 |       1234 | 2026-09-01 10:00:00.123-05
```

Requiere `track_commit_timestamp`: el momento del commit lo lleva Postgres
en `pg_commit_ts` y guardarlo por stripe sería duplicarlo. La granularidad
es el stripe, que es la transacción — y es la correcta, porque una
transacción commitea entera.

**Dónde se paga**: fusionar stripes coarsa el time travel, así que
`merge_stripes()` no toca nada más nuevo que `dbengine.retention_window`
(un día por defecto). Solo aplica si los commit timestamps están
encendidos; sin ellos no hay time travel que proteger.

Si un stripe no se puede ubicar en el tiempo, `time_travel()` **falla** en
vez de contestar: un resultado que mezcla momentos es peor que no
contestar.

## El orden físico, declarado al planner

F6 sabe reordenar la tabla; lo que faltaba era **decirlo**. Sin eso, un
`ORDER BY` o un `GROUP BY` sobre la columna de clustering pagaba un `Sort`
sobre datos que ya estaban ordenados, y un `LIMIT` no podía cortar temprano.

El scan ahora declara pathkeys. La condición no es
`pg_index.indisclustered`: ese flag dice con qué índice se corrió `CLUSTER`
la última vez y **sobrevive a los `INSERT` posteriores**, que son
justamente los que rompen el orden. Confiar en él daría resultados mal
ordenados sin ningún síntoma.

La condición es una marca propia, `dbengine.relation_order`, que escribe
`CLUSTER` después de volcar los stripes ordenados y borra cualquier
escritura. Es deliberadamente conservadora: perderla de más cuesta un
`Sort`, no perderla cuando había que perderla da resultados incorrectos.

```sql
CLUSTER t USING t_k;
EXPLAIN SELECT k FROM t ORDER BY k LIMIT 5;
--  Limit
--    ->  Custom Scan (DbengineColumnarScan) on t     ← sin Sort

INSERT INTO t VALUES (0, 'nuevo');
EXPLAIN SELECT k FROM t ORDER BY k LIMIT 5;
--  Limit
--    ->  Sort                                        ← vuelve
--          ->  Custom Scan (DbengineColumnarScan) on t
```

La fusión de stripes es la excepción a "cualquier escritura la borra":
reescribe stripes pero respeta el orden de las filas, así que la conserva.

**En paralelo también vale.** Cada worker toma stripes con un `fetch_add`
sobre el mismo contador, así que la secuencia que ve cada uno es creciente y
su salida sale ordenada — que es lo que `Gather Merge` necesita para
intercalarlas. El `Gather` pelado no declara pathkeys, así que tampoco
miente.

**Cuándo conviene usarlo.** Declarar el orden no cuesta nada; *tener* el
orden sí, porque es exclusivo. Vale la pena cuando hay una columna de
agrupación u ordenamiento dominante. Si el workload agrupa por varias
columnas distintas, reordenar por una le saca la correlación a las demás y el
balance da cero — que es exactamente lo que pasa en ClickBench.

## ClickBench

Las mediciones de F2 a F7 son microbenchmarks: cada una aísla la cosa que esa
fase construyó, sobre datos que armé yo. Sirven para saber si una pieza
funciona, no para saber cuánto rinde el motor entero sobre un workload que no
elegí. Para eso está ClickBench, con su DDL y sus 43 queries sin tocar
(`bench/clickbench/`).

**Los números no son comparables con la tabla oficial de ClickBench.** El
dataset completo son 100M filas y ~70 GB descomprimidos, que no entran en el
disco de desarrollo; esto son las **primeras 10M filas del `hits.tsv` real**,
bajado de la fuente. Sirve para comparar dbengine contra heap sobre el mismo
dato, que es la pregunta que importa acá.

Antes de mirar un solo tiempo, las 43 queries tienen que dar lo mismo en las
dos tablas. **Las 43 coinciden.**

Eso costó arreglar el arnés antes que el motor. La comparación ordenaba la
salida de cada query y comparaba las filas devueltas, y con eso marcaba
cuatro o cinco fallos fijos en cada corrida. Ninguno era real: q22, q32, q33,
q39 y q41 son `ORDER BY count(*) DESC LIMIT 10` sobre miles de grupos
empatados —en q33 los 10M grupos tienen `count(*) = 1`, en q32 empatan
1.374.133—, así que los dos motores devuelven **diez filas distintas y las
dos correctas**. Cuatro fallos falsos permanentes son la mejor forma de que
nadie mire el quinto cuando sea de verdad.

Ahora se compara el resultado **completo**, sin el `LIMIT`, con un agregado
que no depende del orden:

```sql
SELECT count(*), sum(hashtext(t.*::text)::bigint) FROM (<query sin LIMIT>) t;
```

Es a la vez más estricto —todas las filas, no diez— y ciego al orden.
Verificado que sigue siendo sensible: sacar **una fila de diez millones**
cambia la suma.

| | heap | dbengine | |
|---|---|---|---|
| tamaño de la tabla | 6824 MB | **1832 MB** | **3,73×** menos |
| tamaño con índices | 7305 MB | **2440 MB** | **2,99×** menos |
| media geométrica, 43 queries en caliente | | | **2,06×** |
| media geométrica, 43 queries en frío | | | **13,97×** |
| ídem, cota conservadora | | | **10,11×** |
| carga completa (`COPY` + `VACUUM ANALYZE`) | 418,1 s | **222,7 s** | **1,88×** |
| gana / pierde (caliente) | | **37 / 6** | |
| gana / pierde (frío) | | **39 / 4** | |

**La carga es 1,88× más rápida**, que da vuelta el número anterior (+13% a
favor de heap, medido antes de FSST y de los gránulos). El `COPY` en sí es
idéntico —214,3 s contra 214,6 s, un 0,1%— porque lo domina parsear 7,5 GB de
TSV, que es el mismo trabajo para los dos. La diferencia está en el
`VACUUM ANALYZE`: **203,8 s contra 8,1 s**. Un heap tiene que recorrer 10M
filas para marcar visibilidad; en un storage append-only la visibilidad es por
stripe y no hay nada que marcar. Es una corrida de cada motor, seguidas.

**Frío y caliente son dos regímenes distintos, no dos precisiones del mismo
número.** En caliente los 6,8 GB del heap están en RAM y la ventaja es sólo
de CPU: leer menos columnas, agregar sin armar tuplas. En frío el heap tiene
que traer del disco las 105 columnas de cada fila que toca, y dbengine dos o
tres. Por eso 2,06× pasa a 13,97×.

En frío pierden sólo cuatro —q5, q16, q18 y q20— porque q24 y q40, que en
caliente perdían, se dan vuelta: `SELECT *` sigue siendo el peor caso de un
columnar, pero leer 1,8 GB del disco le gana a leer 6,8 GB igual.

La cota conservadora compara la **mejor** corrida de heap contra la **peor**
de dbengine. Hace falta porque en frío la dispersión de dbengine llega a 608%
en q25 — pero eso es un artefacto de la escala: q25 tarda 446 ms en su mejor
corrida y 3,2 s en la peor, mientras heap tarda 72 s. Aun la peor corrida le
gana por **22×**. Donde dbengine tarda de verdad —q24 con 19 s, q29 con 21 s—
la dispersión es 0%. Con la cota conservadora el resultado baja de 13,97× a
**10,11×** y pasa de 39/4 a 38/5: ninguna conclusión cambia.

El baseline es heap **con seis índices** —btree en `(CounterID, EventDate)`,
`UserID`, `URLHash`, `RefererHash`, `AdvEngineID` y BRIN en `EventTime`—, no
un heap pelado. Es lo que haría alguien que quiere que estas queries anden, y
es el caso más duro.

De las 43, **ninguna cae a `Seq Scan`**: 13 usan el pushdown de agregados y
28 el scan columnar bajo el executor nativo; dos —q18 y q20— las gana el
btree y el planner elige bien.

### Las seis que pierden

| q | qué es | heap | dbengine | |
|---|---|---|---|---|
| q18 | `GROUP BY UserID, SearchPhrase LIMIT 10`, sin `ORDER BY` | 1,7 ms | 17,7 ms | 0,42× |
| q16 | `GROUP BY UserID ORDER BY count DESC` | 1682 ms | 2635 ms | 0,64× |
| q24 | `SELECT *` con filtro selectivo | 2129 ms | 3318 ms | 0,64× |
| q5 | `COUNT(DISTINCT UserID)` | 913 ms | 1158 ms | 0,79× |
| q20 | `UserID = constante` | 1,1 ms | 2,4 ms | 0,90× |
| q40 | `SELECT * ... LIMIT 10` | 1274 ms | 1276 ms | 1,00× |

Las tres primeras son la misma historia contada de tres formas:

- **q18 y q20** son lo que un btree hace mejor que cualquier scan. Postgres
  camina el índice en orden, agrupa sobre la marcha y corta a los 10 grupos.
  Un scan completo no compite con un early-stop, y 17,7 ms contra 1,7 ms es
  el precio. Vale la pena mirar de dónde viene ese 17,7: **antes eran 2458
  ms**, y bajaron al arreglar el fetch por TID (ver más abajo). El orden de
  magnitud correcto se recuperó; el factor 10 que queda es estructural.
- **q16** agrupa por `UserID` pero con `ORDER BY count(*) DESC`, así que no
  puede cortar temprano: hay que agregar los 10M. Pierde por el costo de
  agrupar una columna de cardinalidad muy alta.
- **q24** pide las **105 columnas** con un filtro selectivo, que es el peor
  caso de un store columnar: hay que rearmar cada fila desde 105 segmentos
  separados. La late materialization no ayuda —difiere el decode, pero el
  segmento se abre igual al cargar el stripe— y tampoco serviría cargarlos
  tarde: pasan 646 filas de 10M repartidas en 153 stripes, así que casi todos
  tienen al menos una y no habría nada que saltear. Bajar de ahí necesita
  acceso aleatorio dentro del segmento, no diferir su carga.

**Ninguna de las seis pierde por sorpresa.** Las tres formas en que un store
columnar pierde contra un heap indexado son: lookup por clave, early-stop
sobre un orden que el índice ya tiene, y `SELECT *`. Están las tres, y no hay
una cuarta.

### De dónde vienen las que ganan

El piso está en q29: de sus 20,8 s, el `REGEXP_REPLACE` sobre `Referer`
explica 4,7 s —la misma query sin el regexp tarda 16,1 s— y el resto se va
agrupando una columna de texto de cardinalidad alta. Es todo CPU sobre valores
ya leídos, donde el layout no cambia nada, y 1,04× es lo que hay que esperar.

El techo está en q1 (`COUNT(*)`), que no lee datos: el conteo sale de los
metadatos de los stripes. 21× con la fórmula oficial, 106× en tiempo crudo.
Es el techo del pushdown, no un número representativo.

### El bug que hacía que los índices no sirvieran

Poner un índice sobre una tabla columnar servía de poco, y el motivo no era
el índice: **traer UNA fila cargaba las 105 columnas del stripe**.

El Table AM no recibe la lista de columnas. `index_fetch_tuple()` y
`scan_bitmap_next_tuple()` sólo ven la `Relation`, así que el storage no tenía
forma de saber que la query pedía tres columnas y traía todas. Medido: **4.932
buffers, unos 38 MB, para devolver una fila** — contra 1 buffer que costó
encontrarla en el índice. Ahí se iban los 63 ms de un lookup que en heap
cuesta 1,3 ms.

La información existe, sólo que en el plan y no en la API del AM. Se saca con
`ExecutorStart_hook`: al arrancar la query se recorre el árbol de `PlanState`
y por cada scan sobre una tabla columnar se anotan los `attnum` que ese nodo
referencia, juntando `targetlist`, `qual`, `bitmapqualorig`, `indexqualorig` y
`tidquals`. Es la misma técnica que usa Citus columnar. Ante cualquier duda
—una columna de sistema, una referencia a la fila entera— se anota "todas":
perder pruning cuesta I/O, podar de más devuelve NULL donde había un valor.

| | antes | después |
|---|---|---|
| buffers para traer una fila | 4.932 | **109** |
| lookup por TID | 63 ms | **4,4 ms** |
| q18 con btree | 2458 ms | **14,0 ms** |

Está en `src/pg/scancols.c`. Sin esto, ningún índice sobre una tabla columnar
rendía lo que debía, y el resto de las mediciones contra heap indexado no
significaban nada.

### Clusterizar: una redistribución, no una mejora

Clusterizar debería atacar justamente las queries que pierden. Y las ataca,
pero le cobra a otras. Medido aislando **sólo** el orden físico: dos tablas
con el mismo motor, los mismos datos y la misma compresión, una sin orden y
otra con `CLUSTER` real por `UserID`. Las diez queries devuelven resultados
idénticos.

| q | qué es | frío | caliente |
|---|---|---|---|
| q18 | `GROUP BY UserID, SearchPhrase LIMIT 10` | **455×** | **461×** |
| q20 | `UserID = constante` | **38,7×** | 33,7× |
| q16 | `GROUP BY UserID ORDER BY count DESC` | 1,97× | 1,89× |
| q34 | `GROUP BY URL` | 1,07× | 1,22× |
| q5, q29 | `COUNT(DISTINCT)`, regexp sobre `Referer` | ~1,0× | ~1,0× |
| q13 | `GROUP BY SearchPhrase` | 0,80× | 0,94× |
| q17 | `GROUP BY UserID, SearchPhrase` con `ORDER BY` | 0,78× | 0,75× |
| **q43** | `CounterID = 62 AND EventDate BETWEEN ...` | **0,17×** | **0,12×** |

q18 y q20 ganan **por estructura**: pathkeys que dejan cortar en el `LIMIT` y
zone maps que podan sobre la columna ordenada. No es I/O — el número es casi
el mismo en frío que en caliente.

q43 pierde 6–8× por el motivo simétrico: la tabla venía ordenada por tiempo
desde la inserción, y ese orden era gratis. Reordenar por `UserID` desparrama
las fechas por todos los stripes y se pierde un pruning que ya existía.

Y clusterizar **agranda la tabla 14,5%** —2046 MB contra 1787 MB—: `title`
+21%, `url` +16%, los timestamps +11%. Ordenar por una columna rompe la
localidad de las otras 104.

**Recomendado para este workload: sin clusterizar.** No porque no funcione
—455× no es poco— sino porque ClickBench agrupa por seis columnas distintas y
una tabla se ordena por una sola.

#### Por qué se había descartado mal

Las corridas viejas clusterizaban **las dos** tablas a la vez y comparaban
dbengine-clusterizado contra heap-clusterizado. Variaban dos cosas y medían
"dbengine vs heap bajo clustering", nunca "clustering vs no clustering dentro
de dbengine". Con eso q18 aparecía como 0,44× —heap con btree hace lo mismo—
y el 455× quedaba invisible.

Por eso `cluster_advice()` devuelve ahora **`es_orden_actual`**: marca la
columna por la que la tabla ya está ordenada. Sin ese dato el consejo muestra
sólo lo que se gana y nunca lo que se pierde, y con q43 cayendo a 0,17× esa
mitad no es un detalle.

### Stripes más chicos: medido y descartado

Todo lo que no se pudo podar en este benchmark viene de la granularidad del
stripe: el filtro de Bloom necesita **M ≲ N** y hay 153 stripes; la
materialización tardía no puede saltear ninguno porque casi todos tienen
alguna fila que pasa. La salida obvia es hacerlos más chicos. No sirve.

Las mismas 10M filas cargadas con `dbengine.staging_threshold_rows` en 8192 y
16384, contra el default de 65536:

| filas por stripe | stripes | tamaño | `CounterID=62` + rango | q37 | q13 |
|---|---|---|---|---|---|
| 65536 | 153 | 2421 MB | **28,8 ms** | **299 ms** | **1244 ms** |
| 16384 | 611 | 2489 MB | 32,5 ms | 347 ms | 1417 ms |
| 8192 | 1221 | 2522 MB | 38,4 ms | 374 ms | 1491 ms |

El descarte mejora, pero poco y en proporción: 92,8% → 93,6% → 93,8%, que en
filas leídas son 721k → 639k → 623k, un 13% menos. Y el costo va en la otra
dirección: ocho veces más stripes son ocho veces más filas de catálogo, más
aperturas de segmento y más armado por stripe. El saldo es **20% a 33% más
lento** y 4% más grande.

**Lo que esto NO dice.** Que el gránulo chico sea malo — ClickHouse indexa
cada 8.192 filas por default y no explota. Dice que **el metadato por stripe
de este formato es demasiado caro**: ocho veces más stripes son ocho veces
más filas de catálogo y más aperturas de segmento. ClickHouse, ORC y Parquet
resuelven lo mismo separando la unidad de almacenamiento de la unidad de
descarte —gránulos de 8.192 filas dentro de un *part*, row-groups de 10.000
dentro de un stripe de 200 MB, page indexes dentro de un row group—. Acá las
dos son la misma cosa, y ahí está el error de diseño.

Una nota sobre la GUC: la que corta stripes al insertar es
`staging_threshold_rows`. `block_rows` es el tamaño objetivo de la **fusión**,
y cambiarla no altera la escritura — la primera tanda de esta medición no
midió nada porque usé esa, y las tres tablas salieron idénticas.

### Granularidad anidada: la salida al problema anterior

La sección de arriba termina en un diagnóstico —**la unidad de escritura y la
unidad de descarte no tienen por qué ser la misma**— y esto es la
implementación de esa salida. El stripe sigue siendo 65.536 filas; adentro se
guarda el min/max de cada tramo de **8.192**, y un stripe que pasó el filtro
puede saltearse la mayoría de sus filas sin decodificarlas.

Primero la oportunidad, antes de escribir nada. Dentro de los stripes que el
zone map deja pasar, ¿cuántos gránulos se podrían descartar?

| predicado | stripes que pasan | gránulos | pasan | descarta |
|---|---|---|---|---|
| `UserID = k` | 46 / 153 | 365 | 108 | **70,4%** |
| `EventDate` en dos días | 57 / 153 | 453 | 344 | 24,1% |
| `CounterID = 62` | 11 / 153 | 88 | 76 | 13,6% |
| `ResolutionWidth = 1638` | 150 / 153 | 1197 | 1182 | 1,3% |
| `RefererHash`, `URLHash`, `AdvEngineID` | 153 / 153 | 1221 | 1221 | **0%** |

Cero sobre columnas de hash es lo esperable y es la mitad útil de la tabla:
dice dónde **no** va a servir.

Y el resultado, sobre la misma tabla con la GUC como única diferencia:

| query | gránulos descartados | caliente | frío |
|---|---|---|---|
| `UserID = k` | 296 | **1,43×** | 1,37× |
| `UserID BETWEEN` | 296 | 1,39× | **1,41×** |
| `EventTime BETWEEN` (una hora) | 287 | 1,25× | 1,22× |
| q43 (`CounterID` + fecha) | 8 | 1,04× | 0,99× |
| `AdvEngineID = 0` | 0 | 0,99× | 1,00× |
| sin filtro | 0 | 1,01× | 1,01× |

**Frío y caliente dan casi lo mismo, y eso es la confirmación de qué ahorra.**
El segmento se lee entero igual: lo que se saltea es decodificar y evaluar el
predicado. Si hubiera ahorrado I/O, el frío sería mucho mejor que el caliente.

Donde no puede podar **no cuesta nada** —0,99× a 1,01×— y el bloque ocupa
776 kB de catálogo para una tabla de 1,8 GB, un 0,04%.

Comparado con clusterizar, que sobre la columna ordenada da 38× a 455×: esto
es un orden de magnitud menos, pero se lo lleva **sobre varias columnas a la
vez**, sin reescribir la tabla, sin `ACCESS EXCLUSIVE`, sin 14,5% más de
disco y sin hundir ninguna otra query.

#### Dónde vive el zone map fino, y por qué

En el **catálogo**, no en el formato de segmento. Dos razones, y las dos son
restricciones reales del diseño:

- **El motor no conoce los tipos.** El min/max se calcula con el operador de
  comparación de la familia btree del tipo, que vive en la capa C. El motor
  C++ sólo ve "valores de ancho fijo de W bytes"; no puede decidir si
  `'2013-07-14'` es menor que `'2013-07-15'`.
- **No rompe tablas existentes.** Una tabla escrita antes simplemente no tiene
  el bloque y no descarta. Meterlo en el segmento habría obligado a reescribir
  todo.

Sólo para tipos por valor de 1, 2, 4 u 8 bytes. Guardar un min/max de largo
variable por gránulo multiplicaría el catálogo por la cantidad de gránulos, y
son justo los tipos donde el descarte por rango sirve menos. Ver
`doc/adr-006-donde-vive-el-zone-map-fino.md`.

El salto vive en **un solo lugar** —`dbe_cursor_next_live_row()`— y los cuatro
recorridos del cursor más los tres de la agregación vectorizada pasan por ahí.
Eso no es prolijidad: el bug de la agregación paralela que se cuenta más abajo
salió exactamente de tener un protocolo copiado en dos lugares y olvidado en
el tercero.

### Un sesgo que casi me como

La primera tanda dio 0,13× en q24: dbengine 7,7× más lento. Era mío, no del
motor. Cualquier seq scan de heap deja sus 6835 MB enteros en page cache, así
que desde q1 heap llegaba caliente a todo; dbengine sólo cachea las columnas
que cada query toca, y q24 es la primera que pide las 105. Estaba comparando
heap caliente contra dbengine frío, y el sesgo pegaba justo donde dbengine ya
era más débil, que es lo que lo hacía creíble.

El harness ahora corre cada query una vez sin medirla antes de las tres que sí
mide. Con eso q24 pasa de 0,13× a 0,75×. Las de columnas anchas —q23, q29,
q34, q35— también se movieron.

### Cuatro bugs que encontró revisar los descartes

Volver sobre cada componente descartado —para medirlo en el régimen correcto
en vez de en el que había quedado— destapó cuatro bugs. Los cuatro
sobrevivieron por la misma razón: **el test que los cubría no comprobaba
nada.**

**1. Resultados incorrectos en agregación paralela sobre tabla clusterizada.**

```
SELECT count(*) FROM hits_clustered                       -> 10.000.000
SELECT sum(c) FROM (SELECT UserID, count(*) c
                    FROM hits_clustered GROUP BY UserID)  -> 30.000.000
```

El nodo `DbengineVectorAgg` reparte los stripes entre participantes con un
contador atómico. Los caminos de min/max y de hash lo hacían; **el de
agrupación ordenada no**, así que el líder y los dos workers recorrían la
tabla entera cada uno. De ahí el 3× exacto. El comentario del planner ya decía
lo correcto —"con `GROUP BY` cada worker agrupa SUS stripes"—, pero el reparto
nunca se escribió en ese bucle.

El camino ordenado se elige sólo con la marca de orden que escribe `CLUSTER`,
así que ninguna medición sobre tablas sin clusterizar estaba contaminada. El
protocolo ahora vive en un solo helper que usan los tres caminos.

*No lo agarraba la suite* porque `grouped.sql` corría entero con
`max_parallel_workers_per_gather = 0`. Tenía un test que comparaba la
agrupación clusterizada contra heap — en serie, donde el bug no existe.

**2. `DROP TABLE` no limpiaba el catálogo.**

El Table AM no tiene callback de DROP: Postgres borra el archivo por smgr, que
es genérico, y nunca avisa al AM. Sobre el banco de ClickBench: **7
relfilenode muertos, 2.597 filas de `stripe` y 272.685 de `stripe_column`**.
El catálogo había crecido a 136 MB; limpiarlo lo dejó en **35 MB**, y eso lo
paga cada scan.

Resuelto con `object_access_hook` en `OAT_DROP`, que se dispara dentro de
`deleteOneObject()` con la relación todavía en `pg_class`.

*No lo agarraba la suite* porque `columnar.sql` tenía una sección titulada
"DROP limpia los metadatos" que borraba las tablas y **no miraba el catálogo**.

**3. `TransactionIdGetCommitTsData()` tira ERROR, no devuelve `false`.**

Un comentario en `catalog.c` afirmaba que devolvía `false` con
`track_commit_timestamp` apagado. No: lanza ERROR. `merge_stripes()` abortaba
en cualquier cluster sin esa opción, con un mensaje que ni menciona a
dbengine.

**4. `tools/devcluster.sh` no habilitaba `track_commit_timestamp`.**

Un cluster creado con el script del propio proyecto fallaba tres tests de su
propia suite.

Y uno más que no es del motor sino del arnés, pero cuenta igual: la
comparación de correctitud marcaba cuatro fallos falsos en cada corrida (ver
arriba). Cuatro fallos permanentes es la mejor forma de que nadie mire el
quinto cuando sea de verdad.

## El gate de F1

Correr la suite de regresión de PostgreSQL con
`default_table_access_method = 'dbengine_columnar'` es lo que valida el AM.
Un storage append-only no puede pasarla entera, así que el número que
importa no es cuántos tests pasan sino **qué falla por otra razón que la
limitación documentada**.

```sh
# baseline: 245/245 con heap
# con el AM columnar: ~110 pasan, y los ERROR introducidos son en su enorme
# mayoría nuestros mensajes de "no soportado" más sus cascadas
```

Encontró siete bugs que ningún test escrito a mano había pensado, incluido
uno que **colgaba el backend para siempre** (`ON CONFLICT` sin el contrato
de `SnapshotDirty`) y dos que devolvían **datos corruptos** (`ALTER TABLE
... TYPE` y `SET TABLESPACE`). Están todos en la tabla de contratos de
arriba y cubiertos por `test/regress/sql/columnar.sql`.

**Sin crashes.** Sobre PostgreSQL 19beta3 compilado con `--enable-cassert`
—que convierte los segfaults intermitentes en aserciones que dicen qué
invariante se rompió— la suite completa corre entera sin un solo `TRAP` ni
`signal 11`, varias veces seguidas. Ese build encontró, entre otros, dos
bugs que en producción serían corrupción silenciosa: el build paralelo de
índices ignoraba el `TableScanDesc` compartido y **cada worker escaneaba la
tabla entera**, y la visibilidad aplicaba la regla MVCC a un
`SnapshotNonVacuumable`, cuyos campos ni siquiera están inicializados
([ADR-002, apéndice 4](doc/adr-002-visibilidad-de-stripes.md)).

Lo que la suite **no** puede ejercitar es su propio test de `vacuum`: como
`DELETE` no está soportado, los `DELETE` fallan y los `INSERT ... SELECT`
que los siguen duplican la tabla en cada vuelta hasta llegar a mil millones
de filas.

## Escritura fila a fila

| caso | stripes | tamaño | scan |
|---|---|---|---|
| 5000 `INSERT` de 1 fila, **una transacción** | 1 | 216 kB | 1,9 ms |
| 2000 `INSERT`, **una transacción cada uno** | 2000 | 16 MB | 16 ms |
| ídem, tras `dbengine.merge_stripes()` ×2 | **1** | 16 MB | — |
| ídem, tras `VACUUM FULL` | 1 | **40 kB** | — |
| 5000 filas en un `INSERT ... SELECT` | 1 | 176 kB | 1,0 ms |
| heap equivalente | — | 288 kB | 0,8 ms |

Todo lo que ocurre dentro de una transacción cae en un solo stripe. Lo que
queda mal es el autocommit fila a fila, donde cada transacción cierra su
stripe — y eso lo arregla la fusión:

```sql
SELECT dbengine.merge_stripes('mi_tabla');   -- devuelve cuántos fusionó
```

Fusionar stripes adyacentes **conservando el orden** conserva el número de
fila de cada fila, y el número de fila es el TID. Así que no mueve ningún
TID y **no toca ningún índice** — a diferencia de reordenar, que los mueve
todos. El presupuesto por corrida es `dbengine.max_blocks_per_compaction`,
así que cada pasada divide la cantidad de stripes por 64 y converge en dos o
tres.

Lo que la fusión no hace es recuperar el espacio: el archivo lógico es
append-only, el stripe nuevo va al final y los bytes viejos quedan como
hueco. `VACUUM FULL` los reclama. La división es deliberada: fusionar
arregla el costo del scan ya mismo y con un lock que no bloquea lecturas ni
escrituras; el espacio se recupera cuando alguien puede pagar el lock
fuerte.

## Layout

```
src/pg/       CAPA C — Table AM, Index AM, Custom Scan, hooks, bgworker
src/pg/tableam.c        los 44 callbacks del TableAmRoutine
src/pg/storage.c        archivo lógico sobre el buffer manager
src/pg/catalog.c        dbengine.stripe / stripe_column + visibilidad
src/pg/columnar_write.c buffer de escritura (R8)
src/pg/columnar_read.c  cursor de lectura
src/pg/customscan.c     Custom Scan: column pruning, zone maps, late materialization
src/pg/vectoragg.c      agregación empujada al scan
src/pg/zonemap.c        descarte de stripes y de gránulos, compartido por los dos nodos
src/pg/scancols.c       qué columnas pide cada scan, para el fetch por TID
src/engine/zonemap/bloom.cpp  filtro de Bloom por segmento
src/engine/compress/fsst/     FSST vendorizado (MIT), compresión del diccionario
src/pg/pgmindex.c       Index AM del PGM
src/pg/workload.c       canal de feedback y consejo de clustering (F6)
src/pg/compact.c        fusión de stripes (F6)
src/pg/timetravel.c     time travel (F7)
src/engine/index/pgm.cpp  construcción y consulta del modelo
src/pg/bridge.cpp   airlock C -> C++
src/pg/pgshim.c     airlock C++ -> C (único PG_TRY del árbol)
src/engine/   CAPA C++ — storage, compresión, zone maps, PGM, exec, versions
src/include/  headers; dbengine/status.h lo comparten las dos capas
test/engine/  test del motor C++, sin Postgres (`make check-engine`)
test/crash/   crash y recuperación (`make check-crash`)
test/regress/ pg_regress
test/isolation/ isolationtester (MVCC)
test/tap/     TAP (crash / recovery)
bench/        benchmarks por fase + ClickBench (`bench/clickbench/`)
tools/        guarda de arquitectura y cluster de desarrollo
```

## GUCs

| GUC | Default | Contexto | Fase |
|---|---|---|---|
| `dbengine.enable_vectorized` | `on` | user | F3 |
| `dbengine.enable_zonemap_pruning` | `on` | user | F2 |
| `dbengine.enable_granule_pruning` | `on` | user | descarte anidado |
| `dbengine.enable_learned_zonemaps` | `off` | user | F5 (experimental) |
| `dbengine.enable_adaptive_clustering` | `off` | sighup | F6 |
| `dbengine.retention_window` | `86400s` | sighup | F7 |
| `dbengine.block_rows` | `65536` | superuser | F1 |
| `dbengine.batch_size` | `1024` | user | F3 |
| `dbengine.staging_threshold_rows` | `65536` | sighup | F1 |
| `dbengine.max_blocks_per_compaction` | `64` | sighup | F6 |
| `dbengine.worker_naptime` | `60s` | sighup | F0 |
| `dbengine.enable_agg_pushdown` | `on` | user | F3 |
| `dbengine.enable_compression` | `on` | user | F2 |
| `dbengine.bloom_bits_per_row` | `0` | user | F5 |
| `dbengine.debug_backtrace` | `off` | sighup | diagnóstico |

`dbengine.debug_backtrace` hace que un backend imprima su propio backtrace
en el log si recibe `SIGSEGV`. Sirve donde no hay core dumps ni `ptrace`
para hacer attach; no es para producción.

El default del cluster sigue siendo `heap` (R5). El columnar se activa por
base, por esquema o por tabla:

```sql
ALTER DATABASE analytics SET default_table_access_method = 'dbengine_columnar';
CREATE TABLE eventos (...) USING dbengine_columnar;
```

Los hooks del planner y el background worker requieren precarga:

```
shared_preload_libraries = 'dbengine'
```

`SELECT dbengine_preloaded();` dice si está activa.

El worker aparece en la tabla de procesos como `postgres: dbengine
maintenance`, pero **no** en `pg_stat_activity`: en F0 se registra con
`BGWORKER_SHMEM_ACCESS` solamente, y un worker sin conexión a base no entra
en la vista. Cuando F7 necesite tocar catálogos para la purga, pasa a
`BGWORKER_BACKEND_DATABASE_CONNECTION` y se vuelve visible junto con su
wait event `DbengineMaintenanceMain`.

## Licencia

MIT, ver [`LICENSE`](LICENSE). FSST (`src/engine/compress/fsst/`) es una
copia sin modificar de la implementación de referencia, también MIT, con su
propia licencia en ese directorio.
