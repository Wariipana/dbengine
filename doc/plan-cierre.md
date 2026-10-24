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

## C0 — Base y riesgos de correctitud (≈ 3–5 días)

Antes de agregar nada, cerrar lo que hoy puede devolver datos incorrectos o
perder trabajo.

- [ ] **Trabajo en curso**: correr `installcheck` completo (regress +
      isolation) sobre los cambios sin commitear (`row_reservation`,
      `scancols`, `columnar_ddl`) y commitearlos.
- [ ] **pg_dump / restore — probable bug.** `stripe`, `stripe_column` y
      `relation_order` están marcadas con `pg_extension_config_dump`, pero
      pg_dump ya vuelca los datos de la tabla columnar con `COPY` (que pasa
      por el AM) y el restore los reescribe en stripes nuevos. Las filas del
      catálogo restauradas quedan indexadas por los relfilenode **del
      cluster de origen**: en el mejor caso son basura; si un relfilenode
      coincide con uno nuevo, el scan listaría stripes ajenos. Escribir el
      test (dump + restore a una base nueva + comparar resultados) y, si se
      confirma, sacar los `config_dump`.
- [ ] **Remote + CI corriendo** (GitHub). Hoy el workflow nunca se ejecutó.
- [ ] **LICENSE** del proyecto (PostgreSQL License o MIT; FSST ya es MIT).
- [ ] `doc/recuperacion/` fuera del repo: tiene rutas y datos de la máquina
      local, no documentación del proyecto.
- [ ] **Tests de merge/timetravel deterministas**: detectar que el xmin
      global está retenido y fallar con un mensaje que lo diga, en vez de
      `fusiono_algo = f`.
- [ ] **CI más estricto**:
  - job con PostgreSQL compilado con `--enable-cassert` (ya encontró
    corrupción silenciosa una vez; tiene que correr siempre);
  - ASan/UBSan sobre `check-engine`;
  - `check-crash` en CI;
  - el **gate de F1 automatizado**: la suite de Postgres con el AM como
    default, comparada contra una lista de fallos esperados — un fallo
    nuevo rompe el build.

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

## C2 — Rendimiento (≈ 3–6 semanas, iterativo)

Primero **perfilar** (`perf` sobre ClickBench en caliente) y atacar lo que
el perfil diga; el orden de abajo es la hipótesis a confirmar. Máquina de
referencia: aarch64, 4 núcleos — las optimizaciones SIMD tienen que ser
NEON o auto-vectorizables, no AVX.

1. [ ] **Decodificación por lotes a través del airlock.** Hoy el scan pide
       valor por valor (`dbe_segreader_get`). Reemplazar por
       `decode_range(row, n, datums, nulls)` de 1024 filas, con
       bit-unpacking desenrollado por ancho de bit (estilo FastPFor) que
       el compilador vectorice. `bitpack.hpp` dice explícitamente que se
       dejó sin optimizar hasta medirlo.
2. [ ] **Predicados evaluados sobre datos comprimidos.** Para `Var op
       Const` sobre tipos de ancho fijo y `=`/`LIKE` sobre texto: comparar
       contra `const - min` en FOR, y en diccionario resolver el predicado
       **una vez por entrada** (un `LIKE '%google%'` sobre 800k URLs
       distintas en vez de 10M filas) y filtrar por código. Sale un vector
       de selección; `ExecQual` queda solo para el residuo.
3. [ ] **Late materialization a nivel segmento** (q24, `SELECT *` con
       filtro selectivo): abrir los segmentos de las columnas de salida
       solo cuando hay filas que pasaron, y leerlos con acceso aleatorio
       (FOR y diccionario ya lo permiten; RLE necesita un índice de
       corridas).
4. [ ] **I/O con `read_stream`** (PG 17+), que en PG 18 usa AIO: los
       segmentos son rangos contiguos de bloques, el caso ideal. Debería
       notarse en frío, que es donde dbengine ya gana más.
5. [ ] **Agregación de alta cardinalidad** (q16, q5): tabla hash
       especializada para claves enteras (open addressing, sin `Datum`), y
       `count(DISTINCT)` sobre un hash set de `int64`.
6. [ ] **Catálogo compacto + caché.** Una fila por stripe con los zone
       maps de todas las columnas empaquetados, y un caché por backend
       invalidado por relcache. Ataca el overhead fijo de las queries chicas
       (q20: 2,4 ms contra 1,1 ms de heap) y el crecimiento a 272k filas
       de `stripe_column`.
7. [ ] (Opcional) **zstd/lz4 como candidata más** en la selección "gana la
       más chica", para texto de alta entropía donde hoy queda crudo.

**Criterio de cierre**: ClickBench a 10M (y si entra en disco, 100M) con
la media geométrica en frío y en caliente contra heap, publicada junto a
Citus columnar y pg_duckdb **en la misma máquina**, con las 43 queries
verificadas contra heap. Las que sigan perdiendo, explicadas como hoy.

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

C0 → C1 (DELETE/UPDATE) → C2 → C4, con C3 intercalado cuando un cambio de
C1/C2 toque el archivo en cuestión. C4 al final para que los números y el
README reflejen el estado definitivo.
