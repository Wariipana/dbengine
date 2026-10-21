# ADR-002 — MVCC a nivel de stripe y el buffer de escritura

**Estado:** aceptado, F1

## El problema

Postgres llama a `tuple_insert()` **una vez por fila**. Escribir un stripe
columnar por fila sería catastrófico: cada stripe ocupa como mínimo una
página por columna. Hace falta el staging buffer de R8.

Pero un buffer en memoria choca con la visibilidad. Si las filas se juntan
en memoria y el flush a stripe ocurre después, un `SELECT` posterior
**dentro de la misma transacción** no las vería. Y no se puede flushear al
empezar el scan: la fila de metadatos del stripe quedaría insertada por el
*comando actual*, con `cmin == curcid`, así que el snapshot del propio scan
no la vería. Es la misma regla que evita el problema de Halloween.

## Decisión

**La visibilidad del stripe no se delega en el MVCC de su fila de
metadatos.** `dbengine.stripe` guarda explícitamente quién lo insertó:

```
insert_xid  xid8    -- transacción que insertó las filas
insert_cid  integer -- command id dentro de esa transacción
```

El scan lee `dbengine.stripe` con `SnapshotSelf` y aplica **su propia**
regla de visibilidad sobre esas dos columnas:

```
visible(stripe, snap) :=
      insert_xid == mi_xid_actual  ?  insert_cid < snap->curcid
    : XidInMVCCSnapshot(insert_xid, snap) ? false
    : TransactionIdDidCommit(insert_xid)
```

Como los stripes son **append-only** (sin UPDATE ni DELETE en esta fase),
no hay `xmax`: esa regla es la definición completa de visibilidad, y es
exactamente la mitad `xmin` de `HeapTupleSatisfiesMVCC`.

`SnapshotSelf` alcanza como filtro grueso y no rompe nada:

| Quién escribió el stripe | SnapshotSelf | Chequeo explícito | Resultado |
|---|---|---|---|
| Transacción abortada | oculto | — | invisible ✓ |
| Otra transacción en curso | oculto | — | invisible ✓ |
| Otra que commiteó después de mi snapshot | visible | `XidInMVCCSnapshot` → oculto | invisible ✓ |
| Otra que commiteó antes de mi snapshot | visible | commiteada → ok | visible ✓ |
| La mía, comando anterior | visible | `insert_cid < curcid` | visible ✓ |
| La mía, comando actual | visible | `insert_cid == curcid` | invisible ✓ |

## Consecuencia: cuándo se flushea el buffer

El flush graba el stripe registrando el `(xid, cid)` **del momento en que
llegaron las filas**, no el del flush. Eso desacopla *cuándo* se escribe de
*cuándo se ve*, que era el nudo del problema. Se flushea:

- al superar `dbengine.staging_threshold_rows`,
- en `scan_begin` de esa misma relación,
- en `finish_bulk_insert` (COPY, CTAS),
- al terminar la transacción (`RegisterXactCallback`).

## Alcance

Vale mientras el storage columnar sea append-only. Cuando F7 traiga el
version store, o si aparecen UPDATE/DELETE sobre stripes ya compactados,
hace falta además un `delete_xid` por fila y un bitmap de borrados por
stripe. La regla de arriba pasa a ser la mitad `xmin` de la definición
completa, no la definición entera.

---

## Apéndice — atribución de subtransacción (bug encontrado en F1)

Flushear tarde tiene una trampa que la regla de visibilidad **no** cubre.
La fila de metadatos del stripe se inserta en la (sub)transacción actual,
que no es necesariamente la que insertó las filas:

```sql
BEGIN;
INSERT INTO t VALUES (4);   -- queda en el buffer, dueño: top-level
SAVEPOINT sp;
INSERT INTO t VALUES (5);   -- cambia el xid -> flushea la 4 ACÁ ADENTRO
ROLLBACK TO sp;             -- y se lleva la fila de metadatos de la 4
```

Resultado antes del arreglo: la fila 4 desaparecía. **Pérdida de datos
silenciosa.** El problema no es que la fila de metadatos se vea, es que
deja de existir, y eso la regla de `(insert_xid, insert_cid)` no lo puede
arreglar.

Tampoco alcanza con flushear antes: `SUBXACT_EVENT_START_SUB` ya corre
adentro de la subtransacción nueva, así que la fila de metadatos caería
igual en el nivel equivocado. Y `SUBXACT_EVENT_ABORT_SUB` no sirve para
reescribirla: adentro de un abort no se puede escribir en catálogos.

**Solución — retención.** Si el flush cae en una subtransacción distinta de
la dueña de las filas, el buffer no se libera: se retiene con los builders
vivos.

| Evento | Acción |
|---|---|
| `ABORT_SUB(S)`, retenido con `flush_subxid == S` | marcar `needs_reflush` |
| `ABORT_SUB(S)`, retenido con `owner_subxid == S` | las filas también murieron: liberar |
| `COMMIT_SUB(S → P)` | `flush_subxid = P`; si queda igual a `owner_subxid`, liberar |
| commit de la transacción externa | ya no hay quién se lleve la fila: liberar todo |

La reescritura ocurre **perezosamente**, en el siguiente punto de entrada
normal (`dbe_staging_reflush_pending`), nunca adentro del callback de
abort. Los bytes del stripe perdido quedan como basura en el storage hasta
que haya VACUUM propio.

Está cubierto por `test/regress/sql/columnar_mvcc.sql`: `ROLLBACK TO`,
savepoints anidados, `RELEASE SAVEPOINT` y bloques `EXCEPTION` de PL/pgSQL
(que también abren una subtransacción).

---

## Apéndice 2 — el contrato de `SnapshotDirty`

`tableam.h` no lo documenta, pero el executor lo da por sentado.

`InitDirtySnapshot()` **solo fija `snapshot_type`**. Los campos `xmin`,
`xmax` y `speculativeToken` quedan como basura del stack, y es el AM quien
tiene que escribirlos **en cada fila que devuelve** — `heap` lo hace en la
primera línea de `HeapTupleSatisfiesDirty()`. Significan: *"el xid que el
caller tiene que esperar antes de reintentar"*, o `Invalid` si la fila está
estable.

No cumplirlo cuelga el backend. `check_exclusion_or_unique_constraint()`
hace:

```c
xwait = TransactionIdIsValid(DirtySnapshot.xmin) ? DirtySnapshot.xmin
                                                 : DirtySnapshot.xmax;
if (TransactionIdIsValid(xwait) && ...) {
    XactLockTableWait(xwait, ...);
    goto retry;              /* ← nada cambió: gira para siempre */
}
```

Con `xmin` basura, el `XactLockTableWait` sobre un xid inexistente vuelve al
instante y el `goto retry` no tiene fin. `InitDirtySnapshot` está **antes**
del `retry:`, así que la basura tampoco se limpia entre vueltas.

Lo implementa `dbe_note_dirty_snapshot()` en `columnar_read.c`, llamado
desde `dbe_cursor_fill_slot()` para que valga en todos los caminos de
lectura. Como el storage es append-only, `xmax` siempre queda en `Invalid`:
lo único que se puede estar esperando es al que insertó.

**Lo encontró la suite de regresión de Postgres**, con
`INSERT ... ON CONFLICT DO NOTHING` sobre una tabla con índice único. Es
exactamente el tipo de cosa que el gate de F1 existe para atrapar: ningún
test escrito a mano lo hubiera pensado.

---

## Apéndice 3 — command id por fila

**Medición que lo motivó.** Con un `insert_cid` por stripe, el buffer tenía
que cerrarse en cada cambio de comando. Resultado: **un stripe por
(transacción, comando)**.

| caso | stripes | tamaño | scan |
|---|---|---|---|
| 5000 `INSERT` de 1 fila, una transacción | 5000 | 39 MB | 35 ms |
| los mismos datos en un `INSERT ... SELECT` | 1 | 176 kB | 1,0 ms |
| heap equivalente | — | 288 kB | 0,8 ms |

138× en espacio y 44× en scan. Y el costo del scan no era enumerar el
catálogo (6 ms de los 35): era el trabajo *por stripe* — cargar segmentos,
abrir readers, resetear contextos, ~7 µs cada uno. O sea que empaquetar
stripes chicos no servía: había que reducir su **cantidad**.

**Decisión.** El stripe guarda el command id de **cada fila**, en un
segmento con `attnum = 0` (los attnum reales arrancan en 1). Con eso el
buffer junta filas de toda la transacción sin importar cuántos comandos
abarque.

`dbengine.stripe` guarda `insert_cid` (mínimo) e `insert_cid_max`, y la
visibilidad pasa a tener tres respuestas:

| condición | resultado |
|---|---|
| `insert_cid_max < curcid` | todas las filas visibles |
| `insert_cid >= curcid` | ninguna visible |
| si no | **parcial**: se filtra por el cid de cada fila |

El caso parcial es el único que lee el segmento de cids. Y el segmento
**solo se escribe si `insert_cid < insert_cid_max`**: un `COPY` o un
`INSERT ... SELECT` tienen un solo comando y no pagan nada.

**Resultado:** 39 MB → **216 kB**, 5000 stripes → **1**, scan 35 ms →
**1,9 ms**. Ahora ocupa menos que el heap equivalente.

**Lo que sigue faltando.** Un `INSERT` por transacción (autocommit fila a
fila) sigue dando un stripe por fila: 2000 filas = 16 MB. Un buffer en
memoria no lo puede arreglar — muere en el commit. Eso necesita fusionar
stripes en segundo plano, que a su vez necesita el command id **y el xid**
por fila, más números de fila densos para no mover TIDs. Por eso el flush
ya devuelve la cola de números reservados que no usó.

## Apéndice 4 — no todos los snapshots tienen los mismos campos llenos

`dbe_stripe_visible()` empezó con un `switch` sobre `snapshot_type` que
atendía `SNAPSHOT_ANY`, `SNAPSHOT_SELF` y `SNAPSHOT_DIRTY`, y mandaba todo
lo demás — vía `default:` — a la regla MVCC. Eso es un error: **un
`SnapshotData` no es una struct uniforme cuyos campos siempre valgan algo.**

`InitNonVacuumableSnapshot()` llena el tipo y el `GlobalVisState`, y nada
más. `xmin`, `xmax`, `xip` y `xcnt` quedan como estaban en la variable
—típicamente basura de stack— porque para ese tipo de snapshot no
significan nada. Pasarle esa struct a `XidInMVCCSnapshot()` es leer un
arreglo cuyo puntero nunca se escribió.

El síntoma era un segfault en *planning*, no en ejecución, y solo cuando la
tabla tenía estadísticas: `ineq_histogram_selectivity()` llama a
`get_actual_variable_range()`, que hace un index scan de verdad sobre la
tabla —con `SnapshotNonVacuumable`— para encontrar el mínimo o el máximo
real en vez de confiar en el histograma. O sea que aparecía en un `EXPLAIN`,
en una consulta que ni siquiera se ejecutaba, y solo después de un `ANALYZE`.

La regla que queda, y que vale para cualquier AM:

- **Enumerar todos los tipos, sin `default:`.** Así un tipo nuevo de
  snapshot rompe la compilación en vez de tomar silenciosamente un camino
  que no le corresponde.
- `SNAPSHOT_NON_VACUUMABLE` pregunta "¿alguien podría necesitar todavía esta
  fila?". Los stripes son append-only y no tienen `xmax`: ninguno es
  recuperable mientras la transacción que lo escribió no haya abortado.
- `SNAPSHOT_TOAST` y `SNAPSHOT_HISTORIC_MVCC` no llegan a una tabla
  columnar —el TOAST de nuestros catálogos es heap y la decodificación
  lógica no soporta este AM— pero se contestan igual con lo único que se
  puede afirmar sin mirar campos que quizás no estén llenos.

Hay un segundo caso del mismo espíritu. Aun con un snapshot MVCC bien
formado, `XidInMVCCSnapshot()` consulta `pg_subtrans` cuando el snapshot
desbordó de subtransacciones. Para un xid más viejo que `TransactionXmin`
esa información ya puede no existir: en una build con assertions salta el
`Assert` de `subtrans.c`, y sin ellas se lee una página de SLRU que no está.
heap no lo sufre porque para entonces la tupla ya tiene el hint bit de xmin
commiteado y ni pregunta. Acá no hay hint bits, así que el atajo hay que
escribirlo a mano: si `TransactionIdPrecedes(xid, TransactionXmin)`, el
estado del clog es la respuesta final y no hace falta preguntar nada más.
