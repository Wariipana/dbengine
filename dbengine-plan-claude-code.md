# DBEngine — Plan de implementación para Claude Code

Extensión de PostgreSQL que agrega storage columnar, ejecución vectorizada, índices aprendidos y time travel, funcionando de forma transparente para cualquier cliente estándar de Postgres.

---

## Parte 1 — Revisión crítica de la propuesta anterior

Ocho problemas detectados al revisar el documento de extensiones. Cada uno cambia el diseño.

### R1 — Empaquetar como una sola extensión, no cuatro

El documento anterior planteaba `dbengine_columnar`, `dbengine_pgm`, `dbengine_vector` y `dbengine_timetravel` como extensiones separadas. Son demasiado interdependientes para eso:

- El clustering adaptativo (Table AM) necesita que el Custom Scan le pase los predicados observados.
- Los zone maps aprendidos (Index AM) necesitan las estadísticas de bloque del Table AM.
- El motor vectorizado necesita una vía de lectura directa al storage columnar.

Cuatro `.so` separados implican dependencias de símbolos C entre extensiones, versionado independiente y problemas de orden de instalación, sin ningún beneficio a cambio.

**Decisión:** una sola extensión `dbengine`, un solo `.so`, que registra internamente: 1 Table AM, 1 Index AM, los hooks de planner, el background worker y las funciones SQL.

### R2 — C++ y el manejo de errores de Postgres es un riesgo real

El plan original era C++23 en todo el motor. Problema: Postgres maneja errores con `ereport`/`elog`, que hacen `longjmp` al `PG_TRY` más cercano. **Un `longjmp` que atraviesa frames de C++ no ejecuta destructores** → memory leaks y estado corrupto. En el sentido inverso, una excepción de C++ que escapa a un frame de C es comportamiento indefinido.

**Decisión:** frontera dura entre los dos mundos.
- Capa C (o `extern "C"` fina): todo lo que toca APIs de Postgres — callbacks del AM, hooks, funciones SQL.
- Capa C++: el motor interno — vectorización, PGM, compresión, cracking. No llama a APIs de Postgres directamente.
- Todo cruce se envuelve: `PG_TRY`/`PG_CATCH` al entrar desde C, `try`/`catch` al salir desde C++. Ninguna excepción cruza, ningún `longjmp` cruza.

### R3 — Database Cracking rompe la alineación posicional del column store

El síntoma visible en Postgres: los índices guardan TIDs (`ctid` = bloque + offset), el cracking reordena físicamente las filas, y si los TIDs se mueven todos los índices quedan inválidos.

Pero el problema de fondo es más profundo y **no lo causa Postgres, lo causa el modelo columnar en sí**. Un column store asume que "posición *j* de cada columna = misma fila". Si crackeás la columna X in-place, sus valores se mueven pero los de Y no: la alineación posicional se rompe y cada reconstrucción de tupla pasa a ser un join entre columnas por tuple ID, que es un componente de costo significativo. Idreos identificó exactamente esto en *Self-organizing tuple reconstruction in column-stores* (SIGMOD 2009).

Consecuencia: cualquier solución que solo arregle los TIDs de Postgres esconde el síntoma sin resolver el problema.

Opciones evaluadas:

| Opción | Qué resuelve | Costo |
|---|---|---|
| (a) TIDs lógicos + mapa de indirección `rowid → posición física` | Solo el síntoma de Postgres | 8 B/fila, actualización en cada crack, indirección por acceso — **y no arregla la alineación entre columnas** |
| (b) *Partial sideways cracking* — cracker maps que mantienen alineados los pares de atributos que se usan juntos | El problema real | Espacio controlado (los maps se materializan solo parcialmente según el workload) |
| (c) Reordenar en **compactación**, no en tiempo de query | El problema real, sin tocar TIDs | Se pierde el pago incremental por query; la reorganización se batchea |
| (d) Refinamiento adaptativo de zone maps, sin mover datos | Lo evita por completo | Se pierde la convergencia hacia ordenado — es la variante más débil |

**Decisión:** (c) como mecanismo principal, (b) como fase experimental opcional.

- **(c)** encaja con el staging buffer + compactación que ya está en R8: se juntan estadísticas de qué predicados llegan, y la compactación reordena bloques según el workload observado. Los TIDs cambian solo ahí, que es un punto de reescritura controlado donde ya se actualizan índices igual. Latencia de query predecible. Es lo que hacen en producción Snowflake (automatic clustering) y Databricks (liquid clustering).
- **(b)** queda como opción para adaptividad real en tiempo de query sobre columnas calientes. Los cracker maps dan un mapeo directo entre pares de atributos usados juntos, reorganizados como parte de la evaluación de la query, y alcanzan performance similar a datos pre-ordenados sin pagar el pre-ordenamiento inicial. Cada map es una colección de chunks que se reorganizan, descartan o recrean individualmente, así que el costo de espacio es controlable.
- **(a) descartada:** más compleja y no resuelve la alineación entre columnas.

Lectura obligada antes de implementar cualquiera de las dos: *Updating a Cracked Database* (SIGMOD 2007), que da los algoritmos para inserciones, borrados y updates de alto volumen manteniendo las propiedades de performance cuando los updates se intercalan con queries. El cracking clásico asume datos estáticos; este caso no lo es.

### R4 — El Table AM tiene interfaz row-at-a-time

`table_scan_getnextslot()` devuelve **un `TupleTableSlot` por vez**. Un storage columnar debajo de un Table AM te da column pruning y compresión, pero **no te da vectorización**: la interfaz obliga a materializar fila por fila. Esta es exactamente la razón por la que pg_duckdb no usa el camino del Table AM para ejecutar.

**Decisión:** agregar un *back channel* que no estaba en la propuesta. Dos caminos de lectura sobre el mismo storage:

1. **Camino compatible** (Table AM estándar): fila por fila. Sirve para que la tabla funcione con cualquier query, DML, `COPY`, herramientas de terceros. Correcto, no rápido.
2. **Camino rápido** (API privada de lectura por bloques): el Custom Scan lee vectores de columna directamente del storage, sin pasar por la interfaz de tuplas.

Sin este back channel explícito, F3 no puede dar el beneficio esperado.

### R5 — `default_table_access_method` global es peligroso

Poner columnar como default de todo el cluster convierte también las tablas chicas, las tablas de cola, las de unión y cualquier tabla OLTP. Los `UPDATE`/`DELETE` puntuales sobre columnar son malísimos.

**Decisión:** el default del cluster queda en `heap`. El columnar se activa por base de datos o esquema (`ALTER DATABASE x SET default_table_access_method = 'dbengine_columnar'`) o explícitamente por tabla. El objetivo de "funciona por defecto en todos lados" se mantiene igual para el lado de **ejecución** (el hook del planner vía `shared_preload_libraries`), que es donde está la mayor parte de la ganancia.

### R6 — Falta soporte de estadísticas y costeo

La propuesta no menciona `relation_estimate_size`, soporte de `ANALYZE` (`relation_analyze`), ni funciones de costo para los paths custom. Sin esto **el planner está ciego** y va a elegir mal cuándo usar el motor vectorizado. Es un requisito, no un extra.

Nota: PG 18 agregó una API pluggable de estadísticas acumulativas que sirve para feedback en runtime.

### R7 — Faltan zone maps clásicos como prerequisito

La propuesta salta directo a los zone maps *aprendidos*. Pero los min/max clásicos son la ganancia dominante (podan ~90% de bloques en la práctica) y los aprendidos son un refinamiento marginal encima. Construir los aprendidos primero es al revés.

**Decisión:** zone maps min/max + null count + row count como parte de primera clase del Table AM (F2). Los aprendidos quedan como capa opcional (F5), medida **contra ese baseline**, con permiso explícito de descartarlos si no justifican la complejidad.

### R8 — Falta estrategia de escritura

Columnar, compresión y cracking asumen escrituras append/bulk. La propuesta no dice qué pasa con un `INSERT` de una fila. Sin resolverlo, el DML puntual sobre una tabla columnar es catastrófico.

**Decisión:** staging buffer orientado a filas (delta store) que absorbe las escrituras, y compactación periódica a bloques columnares. Es el mismo concepto del MemTable del plan original, reubicado.

---

## Parte 2 — Plan de implementación

### Target y stack

- **PostgreSQL 18** como mínimo (18.6 es el stable actual; 19 está en beta 3 — mantener el código compilando contra ambos en CI).
- **Build:** PGXS (`USE_PGXS=1`), estándar para extensiones fuera del árbol de Postgres.
- **Lenguajes:** C para la capa de integración, C++20/23 para el motor interno, con la frontera de R2.
- **Tests:** `pg_regress` (`make installcheck`), `isolationtester` para MVCC, TAP tests para recovery.

### Estructura del repositorio

```
dbengine/
├── Makefile                    # PGXS
├── dbengine.control
├── sql/
│   └── dbengine--0.1.0.sql     # CREATE EXTENSION script
├── src/
│   ├── pg/                     # CAPA C — toca APIs de Postgres
│   │   ├── init.c              # _PG_init, GUCs, registro de hooks
│   │   ├── tableam.c           # TableAmRoutine
│   │   ├── indexam.c           # IndexAmRoutine (PGM)
│   │   ├── customscan.c        # Custom Scan node + set_rel_pathlist_hook
│   │   ├── bgworker.c          # worker de retención/purga
│   │   ├── funcs.c             # funciones SQL (time_travel, etc.)
│   │   └── bridge.cpp          # ÚNICO punto de cruce C ↔ C++
│   ├── engine/                 # CAPA C++ — no llama a Postgres
│   │   ├── storage/            # bloques columnares, staging buffer
│   │   ├── compress/           # delta, RLE, bit-packing, dictionary, FSST
│   │   ├── zonemap/            # min/max + (luego) learned
│   │   ├── pgm/                # PGM-Index
│   │   ├── clustering/         # feedback de predicados + reorden en compactación
│   │   ├── exec/               # operadores vectorizados
│   │   └── versions/           # version store
│   └── include/
├── test/
│   ├── regress/                # pg_regress
│   ├── isolation/              # isolationtester
│   └── tap/                    # crash / recovery
└── bench/
    ├── clickbench/
    └── tpch/
```

### Fases

Orden pensado para que cada fase deje algo medible y funcionando, y para que lo experimental venga **después** de tener un baseline contra el cual justificarlo.

---

#### F0 — Esqueleto de la extensión ✅

*Objetivo: `CREATE EXTENSION dbengine;` funciona y no hace nada.*

- [x] `Makefile` con PGXS, `dbengine.control`, script SQL de instalación
- [x] `PG_MODULE_MAGIC_EXT` con nombre y versión (disponible desde PG 18)
- [x] `_PG_init()` con registro de GUCs (`dbengine.enable_vectorized`, `dbengine.retention_window`, etc.)
- [x] Detección de si está en `shared_preload_libraries`; error claro si un hook lo requiere y no lo está
- [x] Stub del background worker que arranca y loguea
- [x] Esqueleto de `bridge.cpp` con el patrón `PG_TRY`/`try` de R2, y un test que verifique que ni excepciones ni longjmp cruzan
- [x] CI: build contra PG 18 y 19-beta, `make installcheck` en verde

**Notas de cierre de F0**

- La frontera de R2 quedó en tres piezas, no dos: capa C (`src/pg/*.c`),
  airlock C→C++ (`src/pg/bridge.cpp`, solo `try`/`catch`) y airlock C++→C
  (`src/pg/pgshim.c`, **el único** `PG_TRY` del árbol). Mantener el `PG_TRY`
  fuera de toda unidad de traducción de C++ es lo que garantiza que el
  `longjmp` no pueda saltar por encima de un destructor.
- La invariante no se documenta, se verifica: `tools/check_layering.py`
  corre en cada `make` y falla el build ante headers de Postgres en
  `src/engine/`, `ereport`/`elog` o `PG_TRY` en C++, `palloc` en C++, o
  `malloc` en la capa C.
- `dbengine_bridge_selftest()` mide **cuántos destructores de C++ corrieron**
  en cada caso. Ese contador es la evidencia directa de que el `longjmp` no
  cruzó; si cruzara, daría 0.
- `dbe_require_preload()` está implementada pero todavía no tiene caller:
  el primer consumidor es el hook del planner en F3.
- PG 19 no está empaquetado en PGDG (ni `pgdg-testing` ni `pgdg-snapshot`
  traen `postgresql-19`), así que el job de 19-beta del CI lo compila desde
  el tarball de fuentes y no bloquea el merge. Cuando PGDG publique el
  paquete, pasa a ser una entrada más de la matriz estable.

---

#### F1 — Table AM mínimo, correcto antes que rápido

*Objetivo: una tabla columnar que se comporta correctamente ante cualquier query de Postgres.*

- [x] `TableAmRoutine` completo (44 callbacks en PG 18; los no implementados fallan con error claro)
- [x] Storage: un segmento por columna, null bitmap por columna — **lógicos, no archivos sueltos** (ver ADR-001)
- [x] Staging buffer orientado a filas para escrituras + compactación a bloques columnares (R8) — *resuelto dentro de la transacción; falta autocommit fila a fila*
- [~] Column pruning: implementado en la capa de storage; **el seq scan del Table AM no puede usarlo** (ver abajo)
- [x] `relation_estimate_size` y soporte de `ANALYZE` (R6)
- [~] **Gate de correctitud:** corrido y medido; sin crashes ni aserciones, pero append-only no puede pasarlo entero (ver abajo)

*No optimizar nada en esta fase. Lento está bien; incorrecto no.*

**Notas de F1**

- **Append-only.** `UPDATE`, `DELETE`, `FOR UPDATE`, `ON CONFLICT`,
  `TABLESAMPLE` y el scan hacia atrás fallan con error claro. Andan:
  `INSERT`, `COPY`, `CTAS`, `SELECT` secuencial y paralelo, `CREATE INDEX` +
  index scans, `ANALYZE`, `TRUNCATE`, `DROP`, y desde F6 también
  `VACUUM FULL` y `CLUSTER`, que reescriben la tabla entera.
- **ADR-001 — "un archivo por columna" no es literal.** Los forks de
  Postgres son un conjunto cerrado; escribir archivos propios rompe
  `DROP`, `TRUNCATE`, recovery y tablespaces, o sea el gate de esta misma
  fase. Los segmentos son lógicos, sobre el fork `main` vía buffer manager.
- **ADR-002 — la visibilidad es por stripe, con `(insert_xid, insert_cid)`
  explícitos.** No se puede delegar en el MVCC de la fila de metadatos: el
  buffer escribe tarde y un flush dentro del mismo comando quedaría
  invisible para el scan que lo disparó.
- **El column pruning no cabe en el Table AM.** `scan_begin` no recibe qué
  columnas se piden — la API no lo expresa. Es el mismo problema que R4
  identifica para la vectorización, pero para la proyección. La maquinaria
  (`wanted[]`) está hecha y probada; la usa el Custom Scan de **F3**, que
  sí conoce la lista de columnas.
- **El buffer junta toda la transacción, no un comando.** Antes producía un
  stripe por (transacción, comando): 5000 `INSERT` de una fila costaban
  39 MB y 35 ms de scan contra los 288 kB y 0,8 ms del heap. La causa era
  propia, no de Postgres: el stripe tenía un solo `insert_cid`. Guardando el
  command id **por fila** (segmento `attnum 0`, escrito solo cuando el
  stripe abarca más de un comando) baja a **216 kB, 1 stripe y 1,9 ms** —
  menos que heap. Ver ADR-002, apéndice 3.
- **Autocommit fila a fila: cerrado en F6.** Una transacción por fila sigue
  dando un stripe por fila (2000 filas = 2000 stripes, 16 MB), y un buffer en
  memoria no lo puede arreglar porque muere en el commit. Lo arregla
  `dbengine.merge_stripes()`, que fusiona stripes adyacentes conservando el
  orden y por lo tanto los números de fila: 2000 → 1 en dos pasadas, sin
  mover un TID y sin tocar los índices. No hizo falta el `xid` por fila que
  se había anticipado: alcanza con fusionar solo stripes ya visibles para
  todos y escribir el resultado congelado. Lo que sí hizo falta son los
  números de fila densos, y para eso el flush ya devolvía la cola reservada
  que no usaba.
- **Sin tabla TOAST**: los varlena se guardan materializados dentro del
  segmento y el archivo lógico los reparte entre páginas. Probado con un
  valor de 100 kB.

**El gate no crashea: la suite completa de PostgreSQL 19beta3 con
assertions corre entera contra el AM columnar sin un solo `TRAP` ni
`signal 11`** (4 corridas seguidas; las fallas que quedan son diferencias de
salida esperadas por ser append-only). Llegar ahí costó una cacería aparte,
y lo que la hizo posible fue **compilar Postgres con `--enable-cassert`**:
convierte segfaults intermitentes en aserciones que dicen exactamente qué
invariante se rompió. Los bugs que aparecieron, por si sirven de mapa:

| Síntoma | Causa |
|---|---|
| `Assert(HaveRegisteredOrActiveSnapshot())` en heapam | El flush escribe en un catálogo con columnas `bytea`, y tocar TOAST necesita snapshot |
| `Assert(RelFileLocatorEquals(...phs_locator))` | `parallelscan_initialize` no llenaba `phs_locator` |
| `Assert(ItemPointerCompare(...) < 0)` en GIN | El build paralelo de índices ignoraba el `TableScanDesc` compartido: **cada worker escaneaba la tabla entera** |
| GIN con TIDs inválidos | GIN empaqueta el offset en 11 bits: el máximo es 2047, no 2048 |
| `Assert(!(IsA(node, Var) && varno == ROWID_VAR))` | El Custom Scan aceptaba relaciones con `ROWID_VAR` en el target |
| `Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin))` en subtrans | Sin hint bits, la visibilidad consultaba `pg_subtrans` para xids ya fuera de rango |
| `Assert(ItemPointerIsValid())` en ANALYZE | `TTSOpsVirtual.copy_heap_tuple()` arma el tuple con `heap_form_tuple()`, que deja `t_self` inválido, y `compare_rows()` lo necesita |
| `Assert(var->varno == varno)` en `ExecInitCustomScan` | El path del Custom Scan no seteaba `param_info`, así que las referencias laterales quedaban como `Var` en vez de `Param` |
| segfault en planning, solo con estadísticas | `get_actual_variable_range()` usa `SnapshotNonVacuumable`, cuyo `xmin`/`xip` **no están inicializados**; la regla MVCC leía basura |

Dos de estos —el build paralelo de índices y el `SnapshotNonVacuumable`—
son corrupción o crash silencioso en producción, no rarezas de laboratorio.
El último se cazó con un handler de `SIGSEGV` propio
(`dbengine.debug_backtrace`), porque en este entorno no hay ni core dumps ni
`ptrace` para hacer attach.

---

#### F2 — Zone maps y compresión adaptativa ✅

*Objetivo: primera ganancia medible de I/O y espacio.*

- [x] Estadísticas por bloque: min, max, null count, row count (R7)
- [~] Block pruning en el scan usando esas estadísticas — **no se puede cablear acá** (ADR-003), va en F3
- [x] Dictionary encoding para `VARCHAR`
- [x] Compresión adaptativa por bloque: heurística que elige entre RLE / frame-of-reference con bit-packing / dictionary según el contenido real del bloque
- [x] Byte de esquema en el header del bloque para que el lector sepa decodificar
- [x] **Benchmark obligatorio:** ratio de compresión y tiempo de scan vs. F1 y vs. heap nativo

**Resultado del benchmark** (500k filas, 8 columnas; `bench/f2-compresion.sh`)

| | tamaño | bloques leídos | scan `count(*)` |
|---|---|---|---|
| columnar F2 | **12 MB** | **1476** | 138 ms |
| columnar F1 (crudo) | 37 MB | 4786 | 130 ms |
| heap nativo | 40 MB | 5155 | 27 ms |

**3,25× contra F1 y 3,50× contra heap** en espacio y en bloques leídos. La
GUC `dbengine.enable_compression` permite medir F2 contra F1 con
exactamente el mismo camino de código.

**Notas de F2**

- **El block pruning no cabe en el Table AM** (ADR-003). `scan_begin()` tiene
  parámetros `nkeys`/`key`, pero el executor los deja en cero para un seq
  scan: las condiciones se evalúan con `ExecQual()` *después* de que el AM
  devolvió la fila. Es el mismo límite de R4, ahora aplicado al filtrado.
  F2 construye y guarda los zone maps; **F3 los usa**, junto con el column
  pruning que quedó pendiente de F1.
- **La heurística mide, no adivina.** Se arman las candidatas, se comparan
  los tamaños reales y gana la menor, con el formato crudo siempre como
  piso y un umbral de ganancia mínima del 10%. A esa altura los datos ya
  están en memoria, así que medir sale casi gratis. En el benchmark eligió
  FOR para los correlativos, diccionario para las columnas de baja
  cardinalidad y crudo para las de alta entropía.
- **FSST quedó afuera.** Es un algoritmo con paper propio; el diccionario ya
  cubre el caso que motivaba VARCHAR (baja cardinalidad) con 30× en esa
  columna. FSST gana en texto de alta cardinalidad, que es donde el
  diccionario no sirve — vale la pena, pero como trabajo propio y medido
  contra este baseline.
- **Los zone maps se calculan con la comparación btree del tipo**, no
  comparando bytes: para `text` con collation o para `numeric`, comparar
  bytes daría un rango incorrecto y el pruning descartaría bloques que sí
  hacían falta. Por eso viven en la capa C y no en el motor.
- **El scan sigue costando lo mismo.** F2 apenas suma 6% sobre F1 (138 ms
  contra 130), pero las dos están 4-5× por encima del heap. No es la
  compresión: es R4 — la interfaz del Table AM obliga a reconstruir fila por
  fila, y con los datos en cache el scan es CPU-bound, así que la ganancia
  de I/O no aparece en el reloj. Ahí es donde entra F3.
- **El bit unpacking se midió y se arregló.** La primera versión leía bit a
  bit y la compresión costaba 37% del tiempo de scan; leyendo de a una
  palabra de 64 bits bajó a 6%.

---

#### F3 — Back channel + Custom Scan vectorizado ✅

*La fase de mayor impacto. Objetivo: order-of-magnitude en agregaciones.*

- [x] API privada de lectura por bloques hacia el storage columnar, saltando la interfaz de tuplas (R4) — el cursor con `wanted[]` y filtro de stripes
- [x] Registro del nodo Custom Scan + `set_rel_pathlist_hook`, sobre relaciones base **y particiones**
- [x] Funciones de costo para que el planner decida si conviene (depende de F1/R6)
- [~] Operadores vectorizados: agregación empujada al scan (count, sum, min, max, avg), con y sin `WHERE`, en serie y en paralelo. **HashAggregate con GROUP BY y HashJoin no** — caen al plan normal
- [x] Late materialization: el predicado se evalúa con las columnas que mira, y el resto de la fila se decodifica solo si la pasa
- [x] Frontera de conversión única — la agregación no arma ninguna tupla intermedia
- [x] Scan paralelo (reparto por stripe con contador atómico)
- [x] Fallback limpio al executor nativo para formas de query no soportadas
- [x] **Benchmark obligatorio** — a 500k y a 100M filas (ver abajo)

**Resultado** (500k filas, `bench/f3-customscan.sh`)

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **1 ms** | 63 ms | 22 ms |
| `sum(monto)` | 29 ms | 65 ms | 30 ms |
| `count(DISTINCT region)` | 173 ms | 223 ms | 154 ms |
| `count(*) WHERE id BETWEEN 100 AND 200` | **14 ms** | 60 ms | 27 ms |
| `sum(id) WHERE id > 490000` | **11 ms** | 63 ms | 24 ms |
| `count(*) WHERE nota = 'nota-42'` | **11 ms** | 59 ms | 25 ms |

A esta escala todo entra en cache, así que se mide CPU y no I/O; la
comparación que importa está a 100M filas, más abajo. Aun así se ven los
tres mecanismos separados:

- **`count(*)` sale del catálogo**: 1 ms contra 22 ms de heap, sin leer un
  byte de datos.
- **Con predicado sobre una columna correlacionada**, 2× más rápido que heap
  y 5× que F2, descartando 19 de 20 stripes. Acá el pruning no acelera el
  trabajo: lo elimina.
- **Sin filtro y sin pushdown posible** (`count(DISTINCT)`, `sum(monto)`)
  queda a la par de heap: lo único que aporta el nodo es el column pruning,
  y a 500k filas en cache eso no alcanza para despegarse.

**Notas de F3**

- **Se saldaron las dos deudas anteriores.** El column pruning que F1 no
  podía expresar y el block pruning que F2 no podía cablear salen los dos
  del mismo lugar: el Custom Scan es el único que ve el target list y los
  predicados.
- **El costo se calcula con el I/O real**, no con una fracción del tamaño de
  la tabla: es la suma de los segmentos de las columnas pedidas, que el
  catálogo interno sabe exacto.
- **`cost_seqscan()` subestima el seq scan sobre columnar** — cobra
  `cpu_tuple_cost` por fila suponiendo un heap, cuando el Table AM
  decodifica todas las columnas (5× más caro por fila, medido). El planner
  no tiene cómo saberlo, así que el costo de este nodo se expresa como
  fracción del de una fila completa, proporcional a las columnas pedidas.
  Leyendo todas queda a la par y gana el seq scan, que es lo correcto.
- **El pruning es conservador por construcción:** solo descarta cuando el
  rango `[min, max]` excluye con certeza. Sin zone map, con un tipo sin
  orden btree o con un operador que no está en la familia, se lee el stripe.
  Y las cláusulas siguen en `plan.qual`: el pruning no reemplaza la
  evaluación del predicado, solo evita leer. 16 consultas del test comparan
  el resultado con y sin Custom Scan.
- **Los tipos cruzados eran el caso normal, no un borde.** `bigint >= 100`
  compara int8 con int4. La primera versión exigía tipos iguales y no podaba
  nada. Se resolvió usando los operadores cross-type de la propia familia
  btree en vez de la función de comparación del tipo.
- **Sin path paralelo el nodo no se elegía.** Con la configuración por
  defecto el planner prefería el Parallel Seq Scan y todo el column pruning
  se perdía. Además, para decidir si vale paralelizar hay que mirar el
  tamaño de la TABLA, no el de las columnas leídas: una query que pide una
  columna angosta nunca llegaría al umbral.
- **El benchmark de 500k engañaba.** A esa escala todo entra en cache y se
  mide CPU, no I/O. Medido a 100M (abajo), F3 le gana a heap por entre 10×
  y 1200×. La conclusión de "empate con heap" era un artefacto del tamaño.

**A escala real: 100M filas** (`bench/f3-escala.sh`)

2862 MB columnar contra 7211 MB heap; 1530 stripes.

En frío, soltando el page cache de la relación con `posix_fadvise` antes de
cada medición — o sea, midiendo I/O de verdad:

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **2,2 s** | 30,5 s | 76,5 s |
| `sum(monto)` | **8,1 s** | 31,0 s | 77,7 s |
| `WHERE id BETWEEN 100 AND 200` | **0,06 s** | 30,4 s | 76,8 s |

Bloques leídos, que es la medida que no depende del hardware:

| consulta | F3 | F2 | heap |
|---|---|---|---|
| `count(*)` | **173** | 1.466.119 | 3.387.927 |
| `sum(monto)` | **69.424** | 1.466.176 | 3.397.876 |
| `WHERE id BETWEEN 100 AND 200` | **68** | 1.027.320 | 2.378.527 |

En caliente, con todo en cache, F3 sigue ganando: `count(*)` 2,0 s contra
4,8 s de heap y 35,4 s de F2.

O sea: **35× más rápido que heap en `count(*)`, 9,6× en `sum`, 1233× con un
predicado selectivo.** El objetivo de order-of-magnitude de la fase está
cumplido — con pruning, sin vectorizar todavía.

**Lo que apareció al medir a escala:** el planning pasó de 0,3 ms (heap) a
10,9 ms, porque el costeo recorría los segmentos de los 1530 stripes (7650
filas de catálogo). Se arregló muestreando 32 stripes y extrapolando por
filas — los stripes son parejos porque los corta el mismo umbral. Quedó en
2,1 ms; el resto es listar los stripes, que hace falta igual.

**Agregación empujada al scan** (100M filas, caliente, config por defecto)

| consulta | F3 | sin pushdown | heap | vs heap |
|---|---|---|---|---|
| `count(*)` | **2,2 ms** | 2235 ms | 2697 ms | **1230×** |
| `min(id), max(id)` | **18 ms** | 3805 ms | 2989 ms | **169×** |
| `sum(monto)` | **1948 ms** | 3914 ms | 4054 ms | **2,1×** |

**Con predicado** — el `WHERE` se evalúa adentro del nodo:

| consulta | F3 | sin pushdown | heap | vs heap |
|---|---|---|---|---|
| `sum(id) WHERE id BETWEEN 100 AND 200` | **14 ms** | 53 ms | 2576 ms | **180×** |
| `count(*) WHERE id > 99000000` | **24 ms** | 90 ms | 2479 ms | **105×** |
| `sum(monto) WHERE region = 'norte'` | **2633 ms** | 3635 ms | 3830 ms | **1,5×** |
| `avg(monto) WHERE id % 2 = 0` | **2150 ms** | 4354 ms | 3681 ms | **1,7×** |

Las dos primeras son descarte de stripes: el trabajo no se acelera, se
elimina. Las dos últimas son el caso sin descarte posible —`region` se
repite en todos los stripes, `id % 2` no es traducible a un zone map— y ahí
lo que se gana es no armar 100M tuplas para tirarlas. `sum(monto) WHERE
region` pasó de 4172 ms (agregación arriba del scan) a 2633 ms.

**Late materialization** (100M filas, `SELECT *` con `WHERE region='norte'`,
que deja pasar el 25% y **no descarta ningún stripe** porque `region` se
repite en todos):

| | ms |
|---|---|
| con late materialization | **3202** |
| forzando decodificar `nota` siempre | 4576 |
| el mismo scan por el Table AM | 6692 |
| heap | 3440 |

La contraprueba mete `nota IS NOT NULL` en el predicado: no filtra nada,
solo obliga a leer la columna para las 100M filas en vez de para las 25M que
pasan. **30% menos**, y es la diferencia entre perder y ganarle a heap en
una consulta que devuelve la fila entera.

Tres mecanismos, y conviene no confundirlos:

1. **`count(*)`, `min` y `max` salen del catálogo.** El `row_count` y el zone
   map de cada stripe alcanzan; no se lee un byte de datos. `min`/`max` se
   reconocen por `pg_aggregate.aggsortop`, la misma marca que usa el planner
   para resolverlos con un índice. Con visibilidad parcial el atajo no
   aplica —el zone map abarca filas que el snapshot no ve— y se recorre.
2. **El resto se lee por columna, no por fila.** De cada stripe se pide el
   arreglo de la columna una vez y se lo recorre en un lazo propio. Lo que
   se saca del medio es armar un `TupleTableSlot` por fila y cruzar el
   airlock por cada valor: medido, 54 ns de los 74 ns por fila. Este es el
   *back channel* que pide R4.
3. **En paralelo el nodo hace de Partial Agg.** Entrega el estado de
   transición —serializado con `aggserialfn` si es `internal`— y el
   `Finalize Agg` de arriba lo combina. Sin esto el planner elegía
   `Gather → Partial Agg → Parallel Custom Scan`, que construye una tupla
   por fila en cada worker: justo lo que este nodo evita.

**Lo que NO cubre y por qué:**

- ~~**Sin `GROUP BY`, `HAVING`, `DISTINCT`, `FILTER` ni ordered-set.**~~
  Desactualizado: el nodo agrupa —por orden físico cuando lo hay, y por tabla
  de hash cuando no— y resuelve `count(DISTINCT)`. Lo que sigue cayendo al
  plan normal son `HAVING`, `FILTER` y los ordered-set. HashJoin vectorizado
  queda pendiente.
- **La agregación empujada no cruza un `Append`.** Cada partición recibe su
  propio Custom Scan —con column pruning, descarte de stripes y late
  materialization— pero un `count(*)` sobre la tabla particionada se resuelve
  con `Append` + `Agg` normal, no con el nodo de agregación. Ese nodo pide
  una única relación base; hacerlo por partición necesita construir un
  `Append` de agregaciones parciales, que es trabajo aparte.
- **Las columnas de largo variable y las de diccionario** no tienen arreglo
  contiguo: ahí se pide valor por valor, aunque sin armar tuplas. Se nota:
  `sum(monto)` (numeric) rinde 2,2× y `sum(id)` (bigint) 3,8×.

**Con `WHERE` el predicado se evalúa adentro del nodo.** Las cláusulas no
pueden ir en `plan.qual` ni en `custom_exprs` — como el nodo tiene
`custom_scan_tlist` (los agregados), `setrefs.c` reescribiría sus `Var`
contra ese tlist y no los encontraría. Viajan en `custom_private`, que
`setrefs.c` no toca, y se evalúan contra un slot con el descriptor de la
relación: `ExecInitExpr()` traduce cualquier `varno` que no sea
`INNER_VAR`/`OUTER_VAR` a `EEOP_SCAN_VAR`, que lee `ecxt_scantuple` por
`varattno`. Los stripes que el zone map descarta ni se miran. Los atajos
por metadatos se apagan: el `row_count` y el zone map describen todas las
filas del stripe, no las que pasan el filtro.

**Late materialization en el scan.** El cursor llena el slot en dos pasos:
primero las columnas que mira el predicado, y las demás recién si la fila lo
pasa. Para eso el predicado sale de `plan.qual` y se evalúa adentro del
nodo — que es también el único lugar donde se puede intercalar entre los dos
pasos. Va en `custom_exprs` y no en `custom_private` porque es lo único que
`setrefs.c` ajusta con el mismo `rtoffset` que `scanrelid`, y donde
`create_customscan_plan()` reemplaza los `Var` del outer por `Param` cuando
el path está parametrizado.

**Dos bugs que no rompían nada, solo apagaban cosas.** Los dos se
encontraron midiendo, no probando:

- El path de la agregación se creaba con `parallel_safe = false`. `add_path()`
  compara costos con 1% de tolerancia y, cuando dos paths empatan dentro de
  esa banda, desempata por `parallel_safe` **antes** que por costo. O sea que
  el nodo se descartaba en silencio cada vez que el plan normal costaba
  parecido — justo el caso de un `WHERE` selectivo, donde la agregación pesa
  poco al lado del scan. `parallel_safe` no significa "corre en paralelo"
  sino "se puede ejecutar dentro de un worker", y este nodo hace lo mismo que
  un Seq Scan no paralelo, que también lo es.
- Los predicados podables viajaban en `custom_private`, que `setrefs.c` no
  toca. Funcionaba mientras el `rtoffset` fuera 0; con más de un nivel de
  subconsulta los `varno` dejaban de coincidir con `scanrelid` y **el
  descarte de stripes se apagaba sin avisar**. Ver ADR-004.

Y una GUC que mentía: `dbengine.enable_zonemap_pruning` estaba definida y no
la leía nadie. Ahora se consulta en `dbe_filterset_build()`, que es el único
punto donde los dos nodos deciden si hay descarte.

**La semántica no se reimplementa.** Se usan las funciones de transición y
finalización de `pg_aggregate` con la misma inicialización que `nodeAgg`,
así que `sum(int4)` desborda igual y `avg(numeric)` redondea igual. Los
agregados con estado `internal` — `sum(bigint)`, `sum(numeric)`, `avg(...)` —
necesitan el contexto de agregación para saber dónde dejarlo; se les pasa un
`AggState` mínimo con `curaggcontext`, que es lo único que
`AggCheckCallContext()` mira. El test compara 30 consultas contra el plan
normal, incluyendo tabla vacía, columna toda nula y transacciones abiertas.

---

#### F4 — Index AM: PGM-Index ✅

- [x] `IndexAmRoutine`: `ambuild`, `aminsert`, `amrescan`, `amgettuple`, `amgetbitmap`, `amcostestimate`
- [x] PGM sobre array ordenado de claves + TIDs, con garantía ε verificada
- [x] Buffer delta para inserts nuevos; `REINDEX` lo absorbe
- [x] **Gate de correctitud:** property-based test — mismos datos con índice PGM y con btree
- [x] **Benchmark:** lookup vs. btree, tamaño del índice en disco

**Resultado** (2M filas, tabla heap para que lo que se compare sea el
índice y no el storage, `bench/f4-pgm.sh`)

| distribución de la clave | tamaño PGM | tamaño btree | 10k sondas PGM | 10k sondas btree |
|---|---|---|---|---|
| secuencial `1..n` | **31 MB** | 43 MB | **476 ms** | 602 ms |
| aleatoria uniforme | **38 MB** | 43 MB | **500 ms** | 1391 ms |
| clusterizada (huecos grandes) | **31 MB** | 43 MB | **501 ms** | 601 ms |

Las sondas son 10.000 lookups puntuales en una sola query, con nested loop
sobre claves elegidas al azar: un lookup suelto lo tapa el andamiaje del
executor y no mide nada. El número del btree con clave aleatoria es el más
ruidoso de la tabla —entre corridas va de 1,4 s a 3,1 s según cómo quede la
cache—; los del PGM se mueven menos de un 5%.

**El modelo es una pista, nunca la autoridad.** Es la decisión de diseño que
sostiene todo lo demás. `pgm_locate()` devuelve una ventana; antes de creerle
se verifica con dos comparaciones exactas si la respuesta puede estar afuera,
y si puede, se hace la binaria sobre el arreglo entero. El resultado es el
correcto ande bien o mal el modelo — lo único que cambia es cuánto tarda. En
las distribuciones medidas el respaldo se usa entre el 0% y el 2,7% de las
veces.

**El modelo va sobre las claves DISTINTAS, no sobre las filas.** Con claves
repetidas un modelo sobre las filas necesita una recta por corrida —ninguna
recta puede mandar mil filas a la misma clave— y termina más grande que los
datos: medido, 500 claves distintas en 500k filas daban 7003 tramos y 168 kB
de modelo, con respaldo en el 96% de las consultas. Sobre las distintas son
**1 tramo y 80 bytes**, con 0% de respaldo. El índice guarda entonces las
claves distintas, dónde arranca la corrida de cada una, y los números de
fila en orden de clave.

**Tres bugs que encontró el test del motor**, todos míos y ninguno del
método: convertir las claves a `double` antes de restarlas (con claves sobre
2^53 el error de la recta se volvía de cientos de posiciones), una ventana
corta por uno en el descenso entre niveles, y el cono que no cerraba tramos
cuando la clave se repetía.

**Lo que NO hace:**

- **Solo claves mapeables a un entero conservando el orden**: enteros,
  `date`, `timestamp`, `timestamptz`, `time`, `oid`. El resto es trabajo del
  btree, y el error lo dice.
- **No ordena.** `amcanorder = false`: las filas están en orden de clave en
  el índice, pero devolverlas ordenadas pide además scan hacia atrás y
  marcas de posición.
- **No impone unicidad** ni indexa `NULL`.
- **Una sola columna**, y no soporta tablas unlogged (`ambuildempty` pediría
  reconstruir el modelo desde el fork init).
- **El build respeta `maintenance_work_mem`.** Las claves van a un
  `tuplesort` que derrama a disco, y el modelo se arma de forma incremental
  —el cono decreciente ya es un algoritmo en línea—, así que ni las claves ni
  el modelo necesitan estar enteros en RAM. Cuesta el doble que el btree
  (1,6–2,1 s contra 0,7–1,0 s a 2M filas) porque hacen falta dos pasadas
  sobre el resultado ordenado: la primera alimenta el modelo y cuenta las
  claves distintas, y recién con eso se sabe cuánto reservar.

  Verificado a escala: **100M filas con `maintenance_work_mem = 64 MB`
  tardan 3 min 12 s** y dan un índice de 1530 MB (16 bytes por fila),
  derramando 2,8 GB a disco sin que la memoria del backend se mueva. La
  primera versión juntaba todo en un arreglo con `repalloc_huge()`: ahí
  mismo son unos 4,4 GB de pico, y en una máquina más chica no es una
  degradación sino el OOM killer.

  El escritor del índice va con buffer por otra razón concreta:
  `dbe_storage_write()` emite un registro de WAL de página completa por
  llamada —está pensado para volcar un stripe entero— y escribir de a 8
  bytes hacía un WAL de 8 kB por clave. Medido: el build de 2M filas pasaba
  de 0,8 s a **174 s**.
- **El delta se recorre linealmente.** Es barato de escribir y caro de leer
  si se deja crecer; `REINDEX` lo absorbe. El costeo lo tiene en cuenta.
- **Las inserciones al índice están serializadas.** Agregar al delta es
  leer-modificar-escribir sobre el contador del último bloque y sobre la
  metapágina; sin un lock, dos backends leen el mismo contador, escriben en
  el mismo slot y una de las dos entradas desaparece — un índice al que le
  falta una fila, sin ningún error. Se toma un lock de página sobre el
  bloque 0. Verificado con seis backends insertando a la vez: las 28.000
  filas se encuentran por el índice.

**Los operadores cruzados no son opcionales.** `WHERE id = 5000` sobre un
`bigint` produce `=(bigint,integer)`, no `=(bigint,bigint)`: el literal es
`integer`. Con una familia por tipo, el planner no reconocía la cláusula y
**el índice no se usaba nunca** — el `EXPLAIN` mostraba un Seq Scan
deshabilitado y ni siquiera llamaba a `amcostestimate`. Los enteros van
todos en una familia con los 30 operadores cruzados, y el argumento de la
cláusula se interpreta con `sk_subtype`, no con el tipo de la columna.

---

#### F5 — Filtros por segmento ✅

- [x] **Medición previa contra el baseline de F2** (`bench/f5-zonemaps.sh`)
- [~] Modelo aprendido por bloque: **descartado con la medición**, ver abajo
- [x] Filtro por bloque que **sí** cierra el hueco medido, con error de un solo lado
- [x] Consultado **antes** de leer el stripe; nunca es autoridad
- [x] Se construye solo donde el canal de feedback lo pide
- [x] **Benchmark A/B contra los zone maps min/max de F2**

**Primero se midió, y la medición decidió el diseño.** La pregunta no era
cuánto podan los min/max sino **cuántos stripes se leen para nada**: los que
el min/max no puede descartar y que igual no tienen ninguna fila que cumpla.
Eso, y nada más, es lo que un filtro por bloque puede sacar.

2M filas, 31 stripes, tres columnas con relaciones distintas con el orden de
inserción (`bench/f5-zonemaps.sh`):

| columna | predicado | descartados | leídos | con alguna fila | **leídos para nada** |
|---|---|---|---|---|---|
| `ord` correlacionada | `= 1000000` | 30 | 1 | 1 | **0** |
| `ord` correlacionada | `BETWEEN` (101 filas) | 30 | 1 | 1 | **0** |
| `grupo` baja cardinalidad | `= 7` (40k filas) | 0 | 31 | 31 | **0** |
| `uid` alta cardinalidad | `=` (1 fila) | 0 | 31 | 1 | **30** |
| `uid` alta cardinalidad | `BETWEEN` (21 filas) | 0 | 31 | 13 | **18** |

Los tres primeros casos no dejan nada que ganar. **El único hueco es el
predicado selectivo sobre una columna de alta cardinalidad no correlacionada
con el orden de inserción** — un `uid = X` en una tabla que crece por tiempo.

**Por qué no un modelo aprendido.** Descartar un stripe en ese caso es
responder "¿está esta clave entre estas 65.536?". Eso es **pertenencia a un
conjunto**, no aproximación de una función suave. Una regresión logística no
puede: no hay frontera lineal —ni de dos capas— que separe 65.536 enteros
arbitrarios del resto del dominio. Una red lo bastante grande para
memorizarlos sería una tabla de hash cara y, peor, sin error de un solo
lado. Donde un modelo sí brilla es aproximando una función monótona, que es
exactamente lo que ya hace el PGM de F4.

**Lo que sí lo cierra: un filtro de Bloom por segmento.** Espacio óptimo
para el problema, y su error es de un solo lado — puede decir "puede estar"
cuando no está, **nunca** "no está" cuando sí. Así la regla de que el filtro
no sea autoridad no se cumple por disciplina sino por construcción: no puede
cambiar un resultado, solo cuánto se lee.

**Resultado** (2M filas, 31 stripes, 50 lookups por igualdad sobre la
columna sin correlación):

| | stripes descartados | buffers | tiempo |
|---|---|---|---|
| con filtro | 1455 de 1550 | 54.894 | **849 ms** |
| solo min/max | 0 | 92.650 | 7192 ms |

**8,5× más rápido.** Y el costo, medido: **+25% de tiempo de escritura** con
filtros en las tres columnas, y **1 byte por fila por columna filtrada** de
espacio (8 bits por fila, ~2% de falsos positivos).

**Por eso el default no los construye en todas las columnas.** Con
`dbengine.bloom_bits_per_row = 0` —el default— el filtro se arma solo para
las columnas que **el canal de feedback de F6 vio con predicados de
igualdad**. Sin historial no se paga nada; después de las primeras consultas,
lo que se escribe lleva filtro solo en esas columnas. Las dos fases encajan:
F6 le dice a F5 dónde vale la pena.

**El hash es el de la familia hash del tipo, no uno propio.** Es lo que hace
que ande con tipos cruzados: `id = 5000` sobre un `bigint` trae un `integer`
del otro lado, y las familias hash de Postgres garantizan que dos valores
iguales de tipos distintos hasheen igual. Hashear con el tipo de la columna
daría otro número y el filtro descartaría stripes que sí tenían la fila — un
resultado incorrecto, no una lentitud. El valor de la consulta se hashea con
la función que el propio operador de igualdad declara para su lado
(`get_op_hash_functions`).

**Lo que no cubre**, y sigue siendo trabajo de F6: los rangos sobre columnas
sin correlación (un filtro de pertenencia no puede contestar `BETWEEN`), y
las columnas donde la igualdad es poco selectiva. Ahí lo que sirve es
reordenar los datos, y una tabla se puede ordenar por una sola cosa: por eso
el filtro y el clustering son complementarios, no alternativas.

---
#### F6 — Clustering adaptativo en compactación

*Mecanismo principal de adaptividad. Opción (c) de R3.*

- [x] Canal de feedback: el hook del planner registra qué columnas se filtran y con qué forma, en memoria compartida (`dbengine.workload_stats()`)
- [x] La decisión sale de esas estadísticas: `dbengine.cluster_advice()` dice por cuál columna conviene reordenar cada tabla
- [x] La reescritura: `relation_copy_for_cluster`, o sea `CLUSTER` y `VACUUM FULL` sobre tablas columnares
- [x] Los TIDs cambian **solo ahí**, en el mismo punto donde Postgres ya reconstruye los índices
- [x] Reconstrucción de zone maps, filtros de Bloom, compresión y PGM dentro de la misma reescritura
- [x] Presupuesto por ciclo: `dbengine.merge_stripes()` fusiona hasta `max_blocks_per_compaction` stripes por corrida
- [~] Presupuesto por ciclo **para el reordenamiento**: no, la reescritura es de la tabla entera (ver abajo)
- [x] **Benchmark** (`bench/f6-clustering.sh`)

*Prerequisito de lectura: Updating a Cracked Database (SIGMOD 2007) — el cracking clásico asume datos estáticos, este caso no lo es.*

**Resultado** (10M filas, 153 stripes, `bench/f6-clustering.sh`). La tabla
tiene dos columnas selectivas: `ts` sigue el orden de inserción y `uid` no
—el caso real de una tabla que crece por tiempo y se consulta por entidad:

| estado | `uid = k`: descartados / leídas / ms | `ts BETWEEN`: descartados / leídas / ms | tamaño |
|---|---|---|---|
| como se insertó | 0 / 9.999.990 / 681 | 152 / 65.435 / 7,4 | 60 MB |
| clusterizada por `uid` | 152 / 65.526 / **7,0** | 0 / 9.999.899 / 757 | 49 MB |
| clusterizada por `ts` | 0 / 9.999.990 / 687 | 152 / 65.435 / **7,3** | 60 MB |
| por `ts` + Bloom en `uid` | 143 / 655.350 / **53** | 152 / 65.435 / 7,6 | 60 MB |

Reordenar por `uid` lleva la consulta de **681 ms a 7,0 ms: 97×**. El
`CLUSTER` tardó 24,2 s para 10M filas.

La tercera fila es el límite del método: **una tabla se ordena por una sola
cosa**, así que favorecer `uid` le saca a `ts` la correlación que tenía —y
la consulta sobre `ts` pasa de 7,4 ms a 757 ms. No es un detalle de
implementación, es la naturaleza del clustering.

La cuarta es la que cierra el círculo con F5: el filtro de Bloom no depende
del orden, así que sobre la columna que el clustering no pudo favorecer
recupera **12,8×** de los 97× (53 ms contra 687 ms) sin tocar el orden. Las
dos fases son complementarias, y el canal de feedback decide cuál va dónde.

**Reordenar no mejora la compresión en general**, que es lo que uno
esperaría y no es cierto. Por columna, ordenando por `sku` (5000 valores
distintos):

| columna | orden de inserción | clusterizada por `sku` |
|---|---|---|
| `sku` baja cardinalidad | 16 MB | **47 kB** |
| `ts` secuencial | **19 MB** | 29 MB |
| `uid` aleatoria | 24 MB | 24 MB |

Colapsa la columna por la que se ordena —340×, porque RLE encuentra los
iguales juntos— y **empeora un 53%** la que venía ordenada, que era
justamente la que mejor comprimía. El neto depende de la tabla: en el caso
de arriba, ordenar por `uid` bajó el total de 60 MB a 49 MB.

**¿Sirve la adaptividad con cualquier workload?** Es la pregunta que decide
si el mecanismo vale la pena, y la respuesta se lee en el consejo antes de
reescribir nada (1M filas, 16 stripes, cinco columnas independientes entre
sí y del orden de inserción):

| workload | consejo | costo sin clusterizar | clusterizado por la columna aconsejada |
|---|---|---|---|
| repetitivo — 40 igualdades sobre `a`, 3 rangos sobre `c` | `a` **97,6%** | 42.999.957 | 5.621.397 — **7,6×** |
| errático — 9 igualdades sobre cada una de las cinco | 20% cada una | 44.999.955 | 36.589.779 — 1,23× |

El costo es la suma de filas que hay que mirar, pesada por cuántas veces se
consulta cada columna. Con un dashboard el consejo es inequívoco y
clusterizar paga 7,6×; con un workload ad-hoc el consejo sale **plano**, y
reordenar apenas mejora un 23% —exactamente la quinta parte de las consultas
que tocan la columna elegida.

Lo importante es que la distribución del puntaje lo dice **antes** de pagar
el ACCESS EXCLUSIVE. Un consejo plano no es una falla del mecanismo: es la
respuesta correcta, y es "no reordenes".

**La reescritura es la de Postgres, no una propia.** Implementar
`relation_copy_for_cluster` hace que `CLUSTER tabla USING un_btree` y
`VACUUM FULL` funcionen sobre tablas columnares, y con eso Postgres pone lo
difícil: el relfilenode nuevo, el swap atómico, la reconstrucción de los
índices y el lock. Del lado del AM solo hay que leer, ordenar con un
`tuplesort` que derrama a disco, y escribir por el camino de inserción de
siempre — que es el que rearma stripes, elige compresión con los datos ya
agrupados, calcula zone maps y construye los filtros de Bloom.

`use_sort` se ignora a propósito: Postgres lo usa para elegir entre ordenar
o recorrer por el índice, una decisión de costo pensada para el heap. Acá
recorrer por el índice sería acceso aleatorio al storage columnar, que es
justo lo que no conviene. El orden de salida es el mismo.

**Fusión de stripes: la parte incremental, y la que cierra el hueco de F1.**

Con autocommit fila a fila cada transacción cierra su stripe: 2000 `INSERT`
sueltos daban **2000 stripes y 16 MB** para lo que en un solo `INSERT` ocupa
216 kB. El problema no es el espacio sino el scan, que abre 2000 stripes.

`dbengine.merge_stripes(tabla)` fusiona stripes adyacentes, y lo que la hace
barata es una propiedad del layout: **un stripe es dueño de un rango
contiguo de números de fila**, así que fusionar conservando el orden
conserva el número de fila de cada fila — y el número de fila es el TID.

```
stripe A: filas 0..999    stripe B: filas 1000..1999
fusionado: filas 0..1999, cada una donde estaba
```

O sea que fusionar **no mueve ningún TID y no toca ningún índice**. Es la
diferencia con reordenar, que los mueve todos y obliga a reconstruirlos. Con
2000 stripes y un btree encima, verificado: cero TID distintos después de
fusionar, y el índice sigue contestando sin reconstruirse.

| | stripes | tamaño |
|---|---|---|
| 2000 `INSERT` en autocommit | 2000 | 16 MB |
| `merge_stripes()` — pasada 1 | 32 | 16 MB |
| `merge_stripes()` — pasada 2 | **1** | 16 MB |
| `VACUUM FULL` | 1 | **40 kB** |

El presupuesto por ciclo es `dbengine.max_blocks_per_compaction`: cada
corrida fusiona a lo sumo esa cantidad de stripes, así que cada pasada
divide la cantidad por el presupuesto y converge en log_presupuesto(N).
Medido con 500 stripes y presupuesto 64: **500 → 8 → 1 en dos pasadas**, y
la tercera no encuentra nada que hacer. El criterio de qué fusionar es el **tamaño**, no si el
stripe ya se fusionó antes: usar "ya está congelado" como marca dejaba la
fusión trabada en 32 stripes, sin poder dar la segunda pasada.

Solo se fusionan stripes que ya son visibles para todos, y el resultado se
escribe con `FrozenTransactionId`: visible para cualquier snapshot presente
o futuro, y de paso fuera de depender del clog. El lock es
`ShareUpdateExclusive` — dos fusiones a la vez se pisarían, pero las lecturas
y las escrituras siguen andando, porque los TID no se mueven.

**Lo que la fusión no hace es recuperar espacio.** El archivo lógico es
append-only: el stripe nuevo se agrega al final y los bytes viejos quedan
como hueco. `VACUUM FULL` los reclama. La división de trabajo es
deliberada: fusionar arregla el costo del scan ya mismo y sin lock fuerte;
el espacio se recupera cuando alguien lo pide y puede pagar el lock.

**Lo que no está: el presupuesto por ciclo para el REORDENAMIENTO.** Ese
sigue siendo una reescritura de la tabla entera con ACCESS EXCLUSIVE, igual
que `CLUSTER` sobre un heap. Hacerlo incremental es distinto de fusionar:
reordenar sí mueve filas entre stripes, o sea que mueve TIDs, y eso obliga a
mantener los índices. Necesita un `xmax` a nivel de stripe —para que un
lector con snapshot viejo siga viendo lo que se reemplazó— y una forma de
sacar las entradas viejas de los índices. Queda anotado.

**Tampoco corre solo.** El background worker no tiene conexión a base de
datos, y dársela para que decida por su cuenta cuándo reescribir una tabla
es una decisión de operación, no de la extensión — la misma razón por la que
`cluster_advice()` aconseja en vez de actuar. `merge_stripes()` se agenda
como cualquier otra tarea de mantenimiento.

**El canal de feedback.** Es la primera pieza y la que decide todo lo
demás: la compactación necesita saber **por cuál columna** conviene
reordenar, y la única parte del sistema que ve los predicados es el hook del
planner. Se anota por `(base, relfilenode, columna)` la cuenta de
igualdades y de rangos:

```sql
SELECT attnum, eq_preds, range_preds FROM dbengine.workload_stats();
 attnum | eq_preds | range_preds
--------+----------+-------------
      1 |        2 |           2
      2 |        0 |           1
      3 |        1 |           0
```

Se separan igualdad y rango porque a la compactación le sirven distinto: la
igualdad se beneficia de **agrupar** los valores iguales, el rango de
**ordenar**. Solo se anotan los predicados que se pueden traducir a una
comparación contra el zone map — los demás no dicen nada sobre cómo ordenar,
así que anotarlos sería ruido.

De ahí sale `dbengine.cluster_advice()`, que pesa la igualdad por encima
del rango —agrupar valores iguales aprieta los rangos `[min,max]` de cada
stripe más que ordenar por una columna que se consulta por tramos anchos— y
además busca el btree de una sola columna que haría falta para poder correr
el `CLUSTER`:

```sql
SELECT columna, puntaje, indice, se_puede_clusterizar FROM dbengine.cluster_advice();
 columna | puntaje | indice | se_puede_clusterizar
---------+---------+--------+----------------------
 uid     |       9 | f6_uid | t
 ts      |       1 | f6_ts  | t
```

Es un **consejo**, no una acción: reescribir una tabla toma ACCESS
EXCLUSIVE, y eso no lo decide una estadística sino quien opera la base.

Lo que **no** se guarda son los valores de los rangos. Eso haría falta para
decidir el orden fino dentro de una columna, y todavía no hay quién lo use:
agregar un histograma después es fácil, sacarlo no.

La tabla es de tamaño fijo (1024 entradas) y sin contabilidad exacta: si se
llena, las anotaciones nuevas se descartan; si dos backends anotan a la vez,
se puede perder una cuenta. Es estadística de workload — ninguna de las dos
cosas cambia un resultado, y la alternativa es pagar un lock por
planificación. Requiere `shared_preload_libraries`, porque la memoria
compartida solo se puede pedir desde el postmaster; sin precarga el canal
queda apagado y anotar no hace nada.

---

#### F6b — Partial sideways cracking — **no se dispara**

*La fase decía: "solo si F6 converge demasiado lento para columnas
calientes".*

**F6 converge en un ciclo.** No es una fusión incremental que se acerque de a
poco: la reescritura ordena la tabla entera de una vez, y medido a 10M filas
tarda 24,2 s y deja la consulta caliente en 7,0 ms contra 681 ms. La
condición que disparaba F6b no se cumple.

Vale distinguir dos cosas que "converger lento" puede querer decir, porque
solo una es la que F6b resolvería:

- **Cantidad de ciclos**: uno. El cracking clásico gana justamente acá,
  reorganizando de a poco como efecto secundario de las consultas; contra una
  reescritura de un solo paso no tiene qué mejorar.
- **Duración del lock**: la reescritura toma ACCESS EXCLUSIVE y crece lineal
  con la tabla — 24,2 s a 10M filas son unos 40 minutos a 1000M. Eso sí es un
  problema real, pero la respuesta más barata es el **reordenamiento
  incremental** que quedó anotado como pendiente de F6, no cracker maps.

Queda una cosa que F6b sí atacaría y F6 no puede: **una tabla se ordena por
una sola columna**, y los cracker maps mantienen varios órdenes a la vez.
Pero eso ya tiene respuesta medida y mucho más barata — el filtro de Bloom
de F5 recupera 12,8× sobre la columna que el clustering no favorece, a 1
byte por fila, contra duplicar columnas enteras por cada par
`(head, tail)`.

**Resultado negativo, documentado.** Si alguna vez se mide que el lock de la
reescritura es el cuello real y el reordenamiento incremental no alcanza,
esta fase vuelve a estar sobre la mesa; hoy no.

---

#### F7 — Time travel ✅

- [~] Version store como estructura lateral: **no hace falta**, ver abajo
- [x] Retention policy configurable (`dbengine.retention_window`), respetada por la fusión de stripes
- [x] `dbengine.time_travel('tabla', timestamp)` como set-returning function
- [x] `dbengine.stripe_timeline('tabla')`: hasta dónde se puede viajar y con qué granularidad
- [x] **Test:** el estado en momentos arbitrarios, y qué le hace la fusión
- [x] **Test de recovery:** crash con `-m immediate` en medio de una transacción abierta (`make check-crash`)

**El version store ya estaba.** Es el resultado que define la fase: en un
storage **append-only nada se borra ni se pisa**, así que el estado de la
tabla en un momento T es exactamente el conjunto de stripes cuya transacción
commiteó antes de T. Una estructura lateral que guardara versiones sería una
copia de algo que el storage ya tiene por construcción.

Y el momento del commit tampoco hay que guardarlo: lo lleva Postgres en
`pg_commit_ts`, que se enciende con `track_commit_timestamp`. Guardarlo por
stripe sería duplicar el catálogo del servidor.

```sql
SELECT * FROM dbengine.time_travel('eventos', now() - interval '1 hour')
    AS (id int, val text);
```

**La granularidad es el stripe, y es la correcta**: un stripe es un lote de
una transacción, y una transacción commitea entera. No hay estado
intermedio que mostrar.

**Dónde se paga.** La fusión de stripes coarsa el time travel: el resultado
se queda con el momento del más nuevo, así que viajar a un instante
intermedio dejaría de ver filas que ahí existían. Por eso `merge_stripes()`
no toca nada más nuevo que `dbengine.retention_window` —y por eso el stripe
fusionado **guarda** su `commit_ts`, que es el único caso donde hace falta
persistirlo: al congelarse pierde el xid y con él la posibilidad de
preguntar. La ventana solo aplica si `track_commit_timestamp` está
encendido: sin él no hay time travel que proteger y la fusión no espera
nada.

**Lo que no se puede.** Viajar más atrás de lo que `pg_commit_ts` conserva,
y ver dentro de una transacción. Si un stripe no se puede ubicar en el
tiempo, `time_travel()` **falla** en vez de contestar: un resultado que
mezcla momentos es peor que no contestar.

**No hay background worker de purga**, y no porque falte: no hay nada que
purgar. Sin `UPDATE` ni `DELETE` no se acumulan versiones viejas — la única
"purga" es la fusión de stripes, que ya existe y se agenda como cualquier
tarea de mantenimiento.

**Recuperación.** Los stripes viven en el fork `main` a través del buffer
manager, con WAL de página completa (ADR-001), así que su recuperación es la
de Postgres y no una propia. El test lo comprueba: `pg_ctl stop -m
immediate` con una transacción abierta que ya había escrito, y al reiniciar
las 300 filas commiteadas están, ninguna de la transacción abierta aparece,
cada stripe del catálogo tiene sus datos, el time travel a antes del crash
da lo mismo que daba, y el índice coincide con el scan.

---

#### Cierre — ClickBench ✅

Las fases se validaron cada una contra su propio microbenchmark, que es lo
que pedía la disciplina de fases. Falta el otro control: un workload que no
elegí yo. ClickBench, con su DDL y sus 43 queries sin tocar, sobre las
primeras 10M filas del `hits.tsv` real (el dataset completo son 100M filas y
no entra en el disco de desarrollo, así que **estos números no son un score
de ClickBench**; comparan dbengine contra heap sobre el mismo dato).

Las 43 dan el mismo resultado en las dos tablas —las cuatro que difieren son
empates en el `ORDER BY`, y coinciden al agregarles un desempate
determinista—. Tamaño 2421 MB contra 6835 MB, suma de las 43 queries 115 s
contra 200 s, media geométrica 3,9×, 42 ganadas y 1 perdida.

Lo que agrega sobre los microbenchmarks son tres cosas que ellos no podían
mostrar:

- **La limitación más cara estaba medida.** 37 de las 43 queries tienen
  `GROUP BY`, y sobre cinco de ellas entre el 74% y el 92% del tiempo se iba
  en agrupar, no en leer. De ahí salió el trabajo de la sección siguiente.
- **El peor caso quedó acotado.** `SELECT *` con filtro selectivo (q24) es
  la única que pierde contra heap sin índices, 0,75×, y el cálculo dice que
  diferir la carga de segmentos no lo arregla: pasan 646 filas de 10M
  repartidas en 153 stripes, así que casi ninguno se puede saltear entero.
- **El baseline honesto es heap indexado, no heap pelado.** Con seis índices
  del lado de heap y los PGM del lado columnar, el 1,73× baja a **1,49×**, la
  mediana a 1,94× y el marcador a 37/6. La cola de 10×–28× se desarma casi
  entera; lo que no se mueve son las queries que barren la tabla.

El detalle y el sesgo de cache que hubo que corregir para medirlo bien están
en `bench/clickbench/README.md`.

#### Cierre — orden físico y agregación agrupada ✅

ClickBench dijo dónde estaba el hueco, y esto lo tapa:

- **El scan declara pathkeys** cuando la tabla está físicamente ordenada. La
  condición no es `pg_index.indisclustered` —que sobrevive a los `INSERT`
  posteriores, que son los que rompen el orden— sino una marca propia,
  `dbengine.relation_order`, que escribe `CLUSTER` y borra cualquier
  escritura.
- **La agregación se empuja al scan con `GROUP BY`** sobre la columna de
  clustering: los grupos quedan contiguos y se lleva uno solo abierto, sin
  tabla de hash. Sobre 5M filas y 200k grupos, 1088 ms → 610 ms → **165 ms**.
- **El canal de feedback anota las columnas de agrupación**, que antes no
  miraba porque lo único que ganaba con el orden era el descarte de stripes.

**Y la medición dice que en ClickBench no conviene usarlo.** Donde aplica es
espectacular —q18 de 2376 ms a 4,0 ms, q16 de 2696 a 535— pero reordenar por
`userid` le saca a `EventDate` la correlación que descartaba 142 de 153
stripes, y q37–q43 se degradan entre 2,5× y 9×. Tres ganan mucho, siete
pierden bastante, el total queda en 1,46× contra 1,49× sin clusterizar. La
configuración recomendada es **sin clusterizar**: pagan cuando hay una
columna de agrupación dominante, y ClickBench agrupa por seis distintas.

De paso aparecieron dos bugs que no tenían síntoma:

- **El canal de feedback no sobrevivía a la reescritura.** Se indexa por
  relfilenode y `CLUSTER` escribe bajo uno nuevo, así que al preguntarle si
  construir un filtro de Bloom contestaba que no para todas las columnas: el
  Bloom adaptativo **no se activaba nunca**, y solo andaba forzándolo con
  `dbengine.bloom_bits_per_row`.
- **El consejo de clustering era ciego al `GROUP BY`**, así que recomendaba
  reordenar por columnas de filtro en queries que ya ganaban.

Los dos tienen ahora un test que los habría detectado.

#### Cierre — agregación sin depender del layout ✅

El pushdown agrupado anterior servía solo sobre la columna de clustering, que
es exclusiva. Estas cuatro no piden nada del layout:

| | ganancia | alcance |
|---|---|---|
| agrupación por hash, en serie y en paralelo | 1,08×–1,23× | cualquier columna |
| agrupar por el código del diccionario | **1,39×–1,42×** | texto con diccionario |
| `count(DISTINCT)` con conjunto de hash | **2,27×** texto, 1,1× enteros | sin `GROUP BY` |

La del diccionario se decidió midiendo antes de construir: en un stripe de
65.536 filas de ClickBench hay 12.856 URLs distintas y 2.508 frases de
búsqueda, o sea entre 5× y 26× menos búsquedas de hash. Se cachea el payload
del grupo y no la entrada de la tabla, que se reubica al crecer.

**Stripes más chicos: medido y descartado.** Con 8192 y 16384 filas por
stripe el descarte mejora un 13% en filas leídas y el saldo es 20%–33% más
lento, porque ocho veces más stripes son ocho veces más catálogo y más armado
por stripe. Cierra también la pregunta del Bloom: aun con 1221 stripes, los
predicados de igualdad de ClickBench matchean entre 81.000 y 613.000 filas.

Dos cosas que aparecieron y valen más que las ganancias:

- **Un costo mal calculado gana siempre.** La primera versión con hash era
  2,3× más lenta que el plan normal y el planner la elegía igual, porque
  `dbe_all_minmax()` se confundía con la columna de agrupación en el tlist y
  dejaba el costo en 0,2 contra ~55.000. No compite: gana, incluso contra
  planes el doble de rápidos.
- **El nodo de agregación no aplicaba `attmissingval`.** Sobre una columna
  agregada con `ALTER TABLE ADD COLUMN ... DEFAULT` devolvía `NULL` donde
  había valor. Estaba latente en `min`, `max` y `sum`; no se veía porque esas
  queries no llegaban al nodo hasta que `count(DISTINCT)` las trajo.

---

## Parte 3 — Notas para Claude Code

**Frontera C/C++ (crítico)**
- Ninguna excepción de C++ puede escapar a un frame de C. Ninguna llamada a `ereport`/`elog` puede hacer `longjmp` a través de frames de C++.
- Todo cruce pasa por `bridge.cpp`. Si aparece una llamada a API de Postgres dentro de `src/engine/`, está mal.
- En la capa C, toda memoria va por `MemoryContext` (`palloc`/`pfree`), nunca `malloc`.

**Disciplina de fases**
- `make installcheck` tiene que quedar en verde al terminar cada fase. Si una fase lo rompe, no avanzar.
- Cada fase termina con un benchmark comparado contra la fase anterior y contra heap nativo. Si una fase no muestra ganancia medible, parar y reconsiderar antes de seguir.
- No empezar F5 ni F6b hasta tener los números de F2, F3 y F6. Son las fases experimentales y solo se justifican contra un baseline medido, nunca contra la expectativa de un paper.
- Ninguna reorganización física de datos ocurre durante una query. Si una fase propone mover datos en tiempo de query, tiene que pasar antes por el análisis de R3.

**Correctitud sobre velocidad**
- El gate de F1 (suite de regresión de Postgres contra el AM columnar) no es negociable. Un storage engine que devuelve resultados incorrectos rápido no sirve.
- El fallback del Custom Scan tiene que ser silencioso y correcto: si el motor vectorizado no soporta algo, cae al executor nativo sin que el usuario vea un error.

**Fuera de alcance (decidido, no pendiente)**
- Sintaxis `->` de navegación de relaciones — el parser de Postgres no es extensible.
- Sintaxis `AT TIMESTAMP` / `AT TRANSACTION` — mismo motivo; se reemplaza por `time_travel()`.
- Wire protocol propio — se hereda de Postgres.
- Infraestructura base propia (`Result<T,E>`, arena allocator, clock inyectable, pipeline de threads) — Postgres ya tiene el equivalente funcional.

**Compatibilidad**
- Cualquier cliente estándar (`psql`, psycopg, node-postgres, JDBC, ORMs) tiene que funcionar sin configuración especial en todas las fases.
- La única diferencia visible aceptable es el output de `EXPLAIN`, que va a mostrar el nodo Custom Scan con el plan interno del motor.
