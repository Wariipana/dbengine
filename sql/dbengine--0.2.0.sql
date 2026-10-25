-- dbengine 0.2.0
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dbengine" to load this file. \quit

-- ===========================================================================
-- Funciones de identificación y diagnóstico
-- ===========================================================================

CREATE FUNCTION version()
RETURNS text
AS 'MODULE_PATHNAME', 'dbengine_version'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION version() IS
	'Versión del motor interno (capa C++).';

CREATE FUNCTION preloaded()
RETURNS boolean
AS 'MODULE_PATHNAME', 'dbengine_preloaded'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION preloaded() IS
	'True si la biblioteca se cargó vía shared_preload_libraries. Los hooks '
	'del planner y el background worker solo existen en ese caso.';

CREATE FUNCTION bridge_selftest(
	OUT test_name text,
	OUT passed    boolean,
	OUT detail    text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'dbengine_bridge_selftest'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION bridge_selftest() IS
	'Verifica las dos invariantes de la frontera C/C++: ninguna excepción de '
	'C++ escapa a un frame de C, y ningún longjmp de ereport atraviesa un '
	'frame de C++ sin correr sus destructores.';

CREATE FUNCTION raise_from_engine(kind integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'dbengine_raise_from_engine'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION raise_from_engine(integer) IS
	'Propaga a SQL un error nacido en el motor C++, para verificar la '
	'conversión DbeStatus -> ereport.';

-- ===========================================================================
-- Catálogo interno del storage columnar
--
-- USING heap explícito, no por omisión: si el usuario tiene
-- default_table_access_method = 'dbengine_columnar', estas tablas saldrían
-- columnares.  Eso es circular — escribir un stripe insertaría en una tabla
-- columnar, que para escribirse necesitaría otro stripe — y además rompe
-- CREATE EXTENSION, porque el access method todavía no existe en este punto
-- del script.
--
-- Estas tablas son heap normales: su MVCC, su WAL y su limpieza vienen de
-- Postgres.  La visibilidad de un stripe NO se delega en el MVCC de su fila
-- acá (ver doc/adr-002-visibilidad-de-stripes.md): se decide con las
-- columnas insert_xid / insert_cid.
-- ===========================================================================

CREATE TABLE stripe (
	relfilenode      oid    NOT NULL,
	stripe_id        bigint NOT NULL,
	first_row_number bigint NOT NULL,
	row_count        bigint NOT NULL,
	data_offset      bigint NOT NULL,
	data_length      bigint NOT NULL,
	insert_xid       xid8    NOT NULL,
	insert_cid       integer NOT NULL,	-- command id mínimo del stripe
	insert_cid_max   integer NOT NULL,	-- command id máximo
	commit_ts        timestamptz,		-- ver F7; NULL = preguntarle a pg_commit_ts
	PRIMARY KEY (relfilenode, stripe_id)
) USING heap;

COMMENT ON TABLE stripe IS
	'Un stripe es un lote de filas de UNA transacción, que puede abarcar '
	'varios comandos. Si insert_cid < insert_cid_max, el stripe guarda '
	'además un segmento con el command id de cada fila (attnum 0) y la '
	'visibilidad se decide fila por fila. Se indexa por '
	'relfilenode y no por OID de pg_class: el storage vive en el '
	'relfilenode, y una reescritura (ALTER TABLE ... TYPE, TRUNCATE) lo '
	'cambia sin cambiar el OID. insert_xid/insert_cid definen su '
	'visibilidad; ver doc/adr-002-visibilidad-de-stripes.md. commit_ts se '
	'llena solo al fusionar stripes, que es cuando el xid original se '
	'pierde: en un stripe normal queda NULL y el momento del commit se le '
	'pregunta a pg_commit_ts. Es lo que hace posible el time travel (F7).';

CREATE TABLE stripe_column (
	relfilenode oid     NOT NULL,
	stripe_id  bigint   NOT NULL,
	attnum     smallint NOT NULL,
	seg_offset bigint   NOT NULL,	-- relativo a stripe.data_offset
	seg_length bigint   NOT NULL,
	null_count bigint   NOT NULL,
	encoding   smallint NOT NULL,	-- 0 crudo, 1 RLE, 2 FOR, 3 diccionario
	min_value  bytea,				-- zone map; NULL = sin zone map
	max_value  bytea,
	bloom      bytea,				-- filtro de Bloom del segmento; NULL = sin filtro
	granules   bytea,				-- min/max por gránulo; NULL = sin zone map fino
	sum_value  numeric,				-- suma exacta de los no nulos (SMA); NULL = no se calcula
	PRIMARY KEY (relfilenode, stripe_id, attnum)
) USING heap;

COMMENT ON TABLE stripe_column IS
	'Ubicación del segmento de cada columna dentro del stripe. Es lo que '
	'permite el column pruning: leer solo los rangos de bytes pedidos. '
	'attnum 0 es el segmento de command ids, que solo existe cuando el '
	'stripe abarca más de un comando. min_value/max_value son el zone map '
	'del segmento, en la representación binaria del tipo; quedan en NULL '
	'si el tipo no tiene orden btree, si todas las filas son nulas, o si '
	'los valores son demasiado grandes para que guardarlos valga la pena. '
	'Para tipos de largo variable se guarda el varlena completo, con su '
	'header, para poder reconstruir el Datum sin copiar. bloom es un filtro '
	'de Bloom sobre los valores del segmento: descarta stripes con '
	'predicados de igualdad sobre columnas que el min/max no puede acotar. '
	'Solo se construye para las columnas que el canal de feedback vio con '
	'predicados de igualdad, y su error es de un solo lado. granules es el '
	'min/max de cada tramo de 8192 filas dentro del segmento: sirve para '
	'saltear filas en un stripe que el min/max del stripe ya dejó pasar. '
	'Solo se construye para tipos de ancho fijo de hasta 8 bytes, porque '
	'guardar min/max de largo variable por gránulo multiplicaría el '
	'catálogo por la cantidad de gránulos. sum_value es la suma exacta de '
	'los valores no nulos del segmento, para contestar sum/avg/count sin '
	'leer datos; solo para smallint, integer y bigint.';

CREATE TABLE relation_order (
	relfilenode oid NOT NULL,
	indexrelid  oid NOT NULL,
	PRIMARY KEY (relfilenode)
) USING heap;

COMMENT ON TABLE relation_order IS
	'Marca que las filas de un relfilenode están físicamente ordenadas '
	'según indexrelid. La escribe CLUSTER después de volcar los stripes '
	'ordenados, y la borra CUALQUIER escritura posterior: pg_index.'
	'indisclustered dice con qué índice se clusterizó la última vez, no '
	'que los datos sigan en ese orden, y un INSERT después de CLUSTER lo '
	'rompe. Sin esta marca el planner no puede confiar en el orden. La '
	'fusión de stripes NO la borra porque respeta el orden de las filas. '
	'Se indexa por relfilenode y no por OID: una reescritura cambia el '
	'storage, y con él el orden.';

-- Estas tablas NO se marcan con pg_extension_config_dump, a propósito.
-- pg_dump vuelca los datos de una tabla columnar con COPY, que pasa por el
-- AM, y el restore los reescribe en stripes nuevos con su propio catálogo.
-- Las filas de acá están indexadas por relfilenode, que es del cluster de
-- origen: si viajaran en el dump, en un cluster nuevo chocarían con las que
-- escribe el restore y la tabla quedaría ilegible (test/dump/roundtrip.sh).
-- pg_upgrade no pasa por acá: copia los archivos y preserva los relfilenode.

CREATE TABLE row_reservation (
	relfilenode oid    NOT NULL,
	first_row   bigint NOT NULL,
	row_count   bigint NOT NULL,
	PRIMARY KEY (relfilenode, first_row)
) USING heap;

COMMENT ON TABLE row_reservation IS
	'Números de fila reservados por un buffer de escritura que todavía no '
	'llegó al storage. Solo se escribe en tablas con índices únicos o de '
	'exclusión: el índice recibe el TID apenas se inserta la fila, pero la '
	'fila vive en la memoria del backend hasta el flush, así que otra '
	'sesión que chequea unicidad no la encontraba y dejaba pasar el '
	'duplicado. Con esta fila, el xmin en curso le dice a quién esperar. '
	'Se borra en el pre-commit de la transacción dueña; una reserva cuyo '
	'dueño ya terminó no significa nada.';

-- ===========================================================================
-- Access method
-- ===========================================================================

CREATE FUNCTION columnar_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'dbengine_columnar_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD dbengine_columnar
	TYPE TABLE
	HANDLER columnar_handler;

COMMENT ON ACCESS METHOD dbengine_columnar IS
	'Storage columnar de dbengine.';

-- ---------------------------------------------------------------------------
-- Canal de feedback del workload (F6)
--
-- Qué columnas se filtran y con qué forma.  La compactación lo va a usar
-- para decidir por cuál columna reordenar; por ahora se puede mirar.
-- ---------------------------------------------------------------------------

CREATE FUNCTION workload_stats(
	OUT dbid oid, OUT relfilenode bigint, OUT attnum smallint,
	OUT eq_preds bigint, OUT range_preds bigint, OUT group_refs bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'dbengine_workload_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION workload_stats() IS
	'Predicados observados por el planner, por columna. Requiere shared_preload_libraries.';

CREATE FUNCTION workload_reset() RETURNS void
AS 'MODULE_PATHNAME', 'dbengine_workload_reset'
LANGUAGE C STRICT;

-- Por cuál columna convendría reordenar cada tabla columnar, según lo que
-- el planner vio.  Es un consejo, no una acción: reescribir una tabla toma
-- ACCESS EXCLUSIVE y eso no lo decide una estadística.
CREATE FUNCTION cluster_advice(
	OUT tabla regclass, OUT attnum smallint, OUT columna text,
	OUT puntaje bigint, OUT indice regclass, OUT se_puede_clusterizar boolean,
	OUT es_orden_actual boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'dbengine_cluster_advice'
LANGUAGE C STRICT;

-- Fusiona stripes chicos adyacentes.  Conserva el orden, así que conserva
-- los números de fila: no mueve ningún TID y no toca ningún índice.
CREATE FUNCTION merge_stripes(regclass) RETURNS bigint
AS 'MODULE_PATHNAME', 'dbengine_merge_stripes'
LANGUAGE C STRICT;

COMMENT ON FUNCTION merge_stripes(regclass) IS
	'Fusiona stripes chicos adyacentes en uno, conservando el orden y por lo '
	'tanto los TID: no toca los índices. Arregla el caso de autocommit fila '
	'a fila, donde cada transacción deja un stripe. El espacio de los '
	'stripes viejos se recupera con VACUUM FULL. Devuelve cuántos se '
	'fusionaron.';

COMMENT ON FUNCTION cluster_advice() IS
	'Por cuál columna conviene reordenar cada tabla columnar, según los '
	'predicados que vio el planner. La igualdad pesa más que el rango: '
	'agrupar valores iguales aprieta los zone maps más que ordenar por una '
	'columna que se consulta por tramos anchos. Hace falta un btree de una '
	'sola columna sobre ella para poder correr CLUSTER. es_orden_actual '
	'marca la columna por la que la tabla YA está ordenada: reordenar es '
	'una redistribución, no una mejora, y lo que se gana en una columna se '
	'pierde en la que servía el orden anterior. Medido sobre ClickBench, '
	'reordenar por UserID da 455x en un GROUP BY con LIMIT y al mismo '
	'tiempo hunde a 0,17x una query que filtraba por rango de fecha.';

-- ---------------------------------------------------------------------------
-- Time travel (F7)
--
-- En un storage append-only el version store ya está: nada se borra ni se
-- pisa, así que el estado en un momento T son los stripes que commitearon
-- antes de T.  El momento lo lleva pg_commit_ts, que hay que encender con
-- track_commit_timestamp.
-- ---------------------------------------------------------------------------

CREATE FUNCTION time_travel(tabla regclass, momento timestamptz)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'dbengine_time_travel'
LANGUAGE C STRICT;

COMMENT ON FUNCTION time_travel(regclass, timestamptz) IS
	'Las filas que la tabla tenía en ese momento. Devuelve setof record, '
	'así que hay que darle la lista de columnas: SELECT * FROM '
	'dbengine.time_travel(''t'', ...) AS (a int, b text). Requiere '
	'track_commit_timestamp. La granularidad es el stripe, que es la '
	'transacción.';

CREATE FUNCTION stripe_timeline(
	tabla regclass,
	OUT stripe_id bigint, OUT first_row_number bigint, OUT row_count bigint,
	OUT insert_xid bigint, OUT commit_ts timestamptz)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'dbengine_stripe_timeline'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stripe_timeline(regclass) IS
	'Qué stripes tiene la tabla y de cuándo es cada uno. Hace explicable el '
	'time travel: hasta dónde se puede viajar y con qué granularidad. '
	'commit_ts en NULL significa que no se puede ubicar en el tiempo.';

-- ---------------------------------------------------------------------------
-- Index AM: PGM-Index (F4)
--
-- Índice aprendido: en vez de un árbol de páginas guarda las claves
-- ordenadas más un modelo de rectas por tramos que predice la posición con
-- error acotado.  Solo sirve para claves mapeables a un entero conservando
-- el orden; el resto es trabajo del btree.
-- ---------------------------------------------------------------------------

CREATE FUNCTION pgm_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'dbengine_pgm_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD dbengine_pgm
	TYPE INDEX
	HANDLER pgm_handler;

COMMENT ON ACCESS METHOD dbengine_pgm IS
	'Índice aprendido (PGM) de dbengine, para claves enteras.';

-- Las estrategias son las del btree: 1 <, 2 <=, 3 =, 4 >=, 5 >.  No hay
-- funciones de soporte: la conversión de la clave a int64 la decide el AM
-- por el tipo del argumento de la cláusula.
--
-- Los enteros van todos en UNA familia, con los operadores cruzados.  No es
-- un detalle: en `WHERE id = 5000` con `id bigint`, el literal es integer y
-- el operador que sale es =(bigint,integer).  Sin los cruzados el planner no
-- reconoce la cláusula y el índice no se usa nunca — que es exactamente por
-- qué integer_ops del btree los trae.
CREATE OPERATOR FAMILY integer_pgm_ops USING dbengine_pgm;

CREATE OPERATOR CLASS int2_pgm_ops DEFAULT FOR TYPE int2 USING dbengine_pgm
	FAMILY integer_pgm_ops AS OPERATOR 1 <(int2,int2), OPERATOR 2 <=(int2,int2), OPERATOR 3 =(int2,int2), OPERATOR 4 >=(int2,int2), OPERATOR 5 >(int2,int2);
CREATE OPERATOR CLASS int4_pgm_ops DEFAULT FOR TYPE int4 USING dbengine_pgm
	FAMILY integer_pgm_ops AS OPERATOR 1 <(int4,int4), OPERATOR 2 <=(int4,int4), OPERATOR 3 =(int4,int4), OPERATOR 4 >=(int4,int4), OPERATOR 5 >(int4,int4);
CREATE OPERATOR CLASS int8_pgm_ops DEFAULT FOR TYPE int8 USING dbengine_pgm
	FAMILY integer_pgm_ops AS OPERATOR 1 <(int8,int8), OPERATOR 2 <=(int8,int8), OPERATOR 3 =(int8,int8), OPERATOR 4 >=(int8,int8), OPERATOR 5 >(int8,int8);

ALTER OPERATOR FAMILY integer_pgm_ops USING dbengine_pgm ADD
	OPERATOR 1 <(int2,int4),
	OPERATOR 2 <=(int2,int4),
	OPERATOR 3 =(int2,int4),
	OPERATOR 4 >=(int2,int4),
	OPERATOR 5 >(int2,int4),
	OPERATOR 1 <(int2,int8),
	OPERATOR 2 <=(int2,int8),
	OPERATOR 3 =(int2,int8),
	OPERATOR 4 >=(int2,int8),
	OPERATOR 5 >(int2,int8),
	OPERATOR 1 <(int4,int2),
	OPERATOR 2 <=(int4,int2),
	OPERATOR 3 =(int4,int2),
	OPERATOR 4 >=(int4,int2),
	OPERATOR 5 >(int4,int2),
	OPERATOR 1 <(int4,int8),
	OPERATOR 2 <=(int4,int8),
	OPERATOR 3 =(int4,int8),
	OPERATOR 4 >=(int4,int8),
	OPERATOR 5 >(int4,int8),
	OPERATOR 1 <(int8,int2),
	OPERATOR 2 <=(int8,int2),
	OPERATOR 3 =(int8,int2),
	OPERATOR 4 >=(int8,int2),
	OPERATOR 5 >(int8,int2),
	OPERATOR 1 <(int8,int4),
	OPERATOR 2 <=(int8,int4),
	OPERATOR 3 =(int8,int4),
	OPERATOR 4 >=(int8,int4),
	OPERATOR 5 >(int8,int4);

-- Los demás tipos van cada uno en su familia.  date y timestamp comparan
-- entre sí en el btree, pero acá no pueden: uno cuenta días y el otro
-- microsegundos, así que un solo mapeo a int64 no conserva el orden de los
-- dos a la vez.  Una comparación cruzada de esas cae al plan sin índice.
CREATE OPERATOR CLASS oid_pgm_ops DEFAULT FOR TYPE oid USING dbengine_pgm AS OPERATOR 1 <(oid,oid), OPERATOR 2 <=(oid,oid), OPERATOR 3 =(oid,oid), OPERATOR 4 >=(oid,oid), OPERATOR 5 >(oid,oid);
CREATE OPERATOR CLASS date_pgm_ops DEFAULT FOR TYPE date USING dbengine_pgm AS OPERATOR 1 <(date,date), OPERATOR 2 <=(date,date), OPERATOR 3 =(date,date), OPERATOR 4 >=(date,date), OPERATOR 5 >(date,date);
CREATE OPERATOR CLASS timestamp_pgm_ops DEFAULT FOR TYPE timestamp USING dbengine_pgm AS OPERATOR 1 <(timestamp,timestamp), OPERATOR 2 <=(timestamp,timestamp), OPERATOR 3 =(timestamp,timestamp), OPERATOR 4 >=(timestamp,timestamp), OPERATOR 5 >(timestamp,timestamp);
CREATE OPERATOR CLASS timestamptz_pgm_ops DEFAULT FOR TYPE timestamptz USING dbengine_pgm AS OPERATOR 1 <(timestamptz,timestamptz), OPERATOR 2 <=(timestamptz,timestamptz), OPERATOR 3 =(timestamptz,timestamptz), OPERATOR 4 >=(timestamptz,timestamptz), OPERATOR 5 >(timestamptz,timestamptz);
CREATE OPERATOR CLASS time_pgm_ops DEFAULT FOR TYPE time USING dbengine_pgm AS OPERATOR 1 <(time,time), OPERATOR 2 <=(time,time), OPERATOR 3 =(time,time), OPERATOR 4 >=(time,time), OPERATOR 5 >(time,time);
