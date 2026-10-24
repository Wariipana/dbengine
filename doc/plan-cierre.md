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

0. [ ] **Apertura perezosa de segmentos**: el header al abrir; FOR y RLE
       se expanden cuando se pide el vector, `get(fila)` va directo al dato
       empacado (FOR en O(1), RLE por búsqueda binaria sobre las corridas),
       el diccionario FSST se descomprime por entrada a demanda. Además,
       desempaquetado especializado por ancho (hoy un `memcpy` de libc por
       valor) y sin rellenar con ceros antes de sobrescribir. Ataca q24 y
       toda query con muchas columnas y filtro selectivo.

### C2.1b — Victorias rápidas

1. [ ] **Agregados precalculados por stripe (SMA).** Guardar suma exacta
       (`numeric`) y conteo de no nulos por columna numérica, igual que hoy
       `row_count` y min/max. `sum`/`avg`/`count(x)` sin filtro salen del
       catálogo; `sum(x + k)` = `sum(x) + k·count(x)` (q30: 90 sumas, 5,1 s).
       Y la regla del **stripe que cumple entero**: si el zone map prueba
       que todas sus filas pasan el `WHERE`, se usan los metadatos sin
       evaluar nada — hoy un `WHERE` apaga todos los atajos. Ataca q30, q3,
       q4, q2, q8. Ref.: Moerkotte, *Small Materialized Aggregates*, VLDB 1998.
2. [ ] **`read_stream` + AIO de PG 18** en `dbe_storage_read`: un segmento
       es un rango contiguo de bloques (`READ_STREAM_SEQUENTIAL`). Medir en
       frío, con `io_method` worker e io_uring. Revisar de paso la
       `BufferAccessStrategy` (hoy ninguna: un scan grande desaloja todo
       `shared_buffers`).
3. [ ] **`count(DISTINCT)` por unión de diccionarios**: en un segmento con
       diccionario, las entradas *son* los valores distintos del stripe; sin
       filtro y con todo visible, alcanza con unir diccionarios (q6). Y
       combinar los conjuntos parciales para correr en paralelo (q5, que
       hoy pierde 0,79×).
4. [ ] **Experimento: clustering multicolumna (Z-order / Hilbert)** en
       `relation_copy_for_cluster`, con las columnas normalizadas por
       cuantiles. Revisa "una tabla se ordena por una sola cosa":
       `bench/f6-clustering.sh` ya tiene el caso (`uid` contra `ts`). Si no
       gana, se documenta como negativo.

### C2.2 — Trabajar sobre los datos comprimidos

5. [ ] **Evaluar una vez por entrada del diccionario**: predicados (`LIKE`,
       `<> ''`, `=`) y expresiones inmutables (`length`, `REGEXP_REPLACE`,
       `extract`) se calculan por entrada y se guardan en un arreglo
       indexado por código. Ataca q21–q23, q28, q29 (20,8 s; 4,7 s son el
       regexp), q11–q15, q37–q43. Solo funciones inmutables y con la
       colación de la columna; se apaga si el diccionario ≈ las filas.
6. [ ] **`LIKE` sobre FSST sin descomprimir** (autómata sobre los códigos
       de símbolo): 2,5×–17× sobre descomprimir y comparar según Pop, Riedl
       y Neumann (TUM). Vortex lo implementa para `prefix%` y `%needle%`,
       que son los de ClickBench. Revisa "FSST es solo tamaño".
7. [ ] **Predicados sobre enteros comprimidos**: comparar contra
       `const − min` en FOR sin reconstruir el valor, y salir con un vector
       de selección; `ExecQual` queda para el residuo.

### C2.3 — Lo grueso

8. [ ] **Top-N con materialización tardía y umbral dinámico** para
       `ORDER BY ... LIMIT k`: calcular solo la clave de orden y el filtro,
       mantener los k mejores, descartar gránulos cuyo min/max no puede
       entrar y leer las demás columnas **solo de las k filas** por TID.
       Ataca q24 (3,3 s, pierde 0,64×), q25–q27 y `SELECT * LIMIT`. Revisa
       el veredicto "estructural" sobre q24. Ref.: DuckDB 1.3 (3–10× con
       LIMIT), Doris y DataFusion (filtros dinámicos al scan).
9. [ ] **Tabla de hash propia** para claves `int2/4/8` y códigos de
       diccionario: direccionamiento abierto, estados de agregado en un
       arreglo plano, sin `TupleHashTable` ni `MinimalTuple`; agregación
       parcial por worker. Ataca las queries de mayor tiempo absoluto (q9,
       q10, q16, q17, q19, q31–q36). Revisa "el hash domina": lo que domina
       es la tabla genérica. Ref.: DuckDB; Xue y Marcus 2025.
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

**Criterio de cierre**: ClickBench a 10M con la media geométrica en frío y
en caliente contra heap, junto a Citus columnar y pg_duckdb **en la misma
máquina**, con las 43 queries verificadas contra heap. Las que sigan
perdiendo, explicadas.

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

C0 ✅ → **C2 (rendimiento)** → C1 (DELETE/UPDATE) → C4, con C3 intercalado
cuando un cambio toque el archivo en cuestión. El foco pasó a ser que el
motor mejore el rendimiento de verdad, así que C2 va primero; C1 después,
porque DELETE obliga a revisar todos los atajos por metadatos (SMA, zone
maps, `count(*)`), y conviene hacerlo una vez con la lista completa de
atajos ya escrita. C4 al final para que los números y el README reflejen el
estado definitivo.
