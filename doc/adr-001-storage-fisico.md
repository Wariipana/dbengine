# ADR-001 — Dónde viven físicamente los bloques columnares

**Estado:** aceptado, F1
**Contexto:** F1 del plan dice *"Storage: un archivo por columna, null bitmap
por columna"*.

## El problema

"Un archivo por columna" no se puede tomar literalmente en una extensión de
Postgres. El storage de una relación se direcciona por `RelFileLocator` +
*fork*, y los forks son un conjunto cerrado (`main`, `fsm`, `vm`, `init`):
no es extensible. Escribir archivos propios por fuera del storage manager
rompe, todo junto:

- `DROP TABLE` y `TRUNCATE` (nadie borra esos archivos),
- recovery ante crash (no hay WAL),
- tablespaces (`ALTER TABLE ... SET TABLESPACE`),
- replicación física,
- `pg_total_relation_size` y todas las vistas de tamaño.

Cualquiera de esos por sí solo hace fracasar el gate de F1, que es correr la
suite de regresión estándar de Postgres contra el AM.

## Decisión

Los segmentos por columna son **lógicos, no físicos**. Se implementa un
**archivo lógico direccionable por bytes sobre el fork `main`**, a través
del buffer manager:

- Bloque 0: página de metadatos del storage (offset libre, próximo stripe
  id, próximo row number).
- Bloques 1..N: bytes crudos en el área utilizable de páginas inicializadas
  con `PageInit`, así siguen valiendo checksums, `PageIsNew` y el WAL.
- Un *stripe* escribe sus segmentos de columna contiguos:
  `[seg col1][seg col2]...[seg colN]`.
- El catálogo `dbengine.stripe_column` guarda `(offset, longitud)` de cada
  segmento.

**El objetivo del plan se cumple igual:** el column pruning lee solo los
rangos de bytes de las columnas pedidas, que es la ganancia real de "un
archivo por columna". Lo que se conserva a cambio es la integración con
todo el resto de Postgres.

## La clave de los metadatos es el relfilenode, no el OID

El catálogo interno (`dbengine.stripe`, `dbengine.stripe_column`) se indexa
por **relfilenode**, no por OID de `pg_class`.

El storage vive en el relfilenode. Una reescritura —`ALTER TABLE ... ALTER
COLUMN ... TYPE`, `TRUNCATE`, `VACUUM FULL`— crea un relfilenode nuevo y
después intercambia, dejando el OID igual. Con la clave puesta en el OID,
los stripes viejos quedaban asociados a un storage que ya no existía: la
tabla se leía corrupta (*"segmento más corto que su propio header"*), no
vacía. Lo encontró el gate de F1 con `ALTER TABLE ... TYPE`.

Consecuencias de la decisión:

- `relation_set_new_filelocator` limpia los metadatos del relfilenode nuevo
  (por si el número se reusa) **y** del viejo (su storage va a desaparecer).
- `relation_copy_data` (`SET TABLESPACE`) copia las filas de metadatos al
  relfilenode nuevo: van con el storage.
- Para introspección, la clave se obtiene con
  `pg_relation_filenode('tabla'::regclass)`.

## Consecuencias

- El WAL se hace con `log_newpage_buffer()`: los bloques de datos se
  escriben una sola vez y no se modifican, así que es exactamente la
  primitiva que corresponde.
- La reserva de espacio se serializa con el lock exclusivo del bloque 0.
  Alcanza para F1 (una escritura = un stripe); si aparece contención, se
  revisa.
- Un `ABORT` después de reservar espacio lo deja filtrado hasta el próximo
  VACUUM. Correcto, no óptimo.

## Alternativa descartada

Guardar los segmentos como `bytea` en una tabla de catálogo daba WAL, MVCC
y limpieza gratis, y hubiera sido más corto. Se descartó porque F2
(compresión propia por bloque) y F3 (lectura directa de bloques sin pasar
por la interfaz de tuplas) necesitan control del layout físico: adoptarlo
en F1 era garantizar que hubiera que tirarlo en F2.
