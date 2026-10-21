-- F1: Table AM columnar — superficie funcional.

CREATE EXTENSION dbengine;

-- ===========================================================================
-- Creación y tipos
-- ===========================================================================

CREATE TABLE c_tipos (
	i2  smallint,
	i4  integer,
	i8  bigint,
	f4  real,
	f8  double precision,
	n   numeric,
	b   boolean,
	t   text,
	vc  varchar(20),
	d   date,
	ts  timestamp,
	by  bytea,
	u   uuid
) USING dbengine_columnar;

SELECT amname FROM pg_am a JOIN pg_class c ON c.relam = a.oid
WHERE c.relname = 'c_tipos';

INSERT INTO c_tipos VALUES
	(1, 100, 10000, 1.5, 2.5, 3.75, true, 'hola', 'mundo',
	 '2024-01-15', '2024-01-15 10:30:00', '\xdeadbeef',
	 '11111111-2222-3333-4444-555555555555'),
	(-1, -100, -10000, -1.5, -2.5, -3.75, false, '', 'x',
	 '1999-12-31', '1999-12-31 23:59:59', '\x00', 
	 '00000000-0000-0000-0000-000000000000'),
	(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	 NULL, NULL, NULL, NULL);

SELECT * FROM c_tipos ORDER BY i4 NULLS LAST;

-- El round-trip tiene que ser exacto para todos los tipos.
SELECT count(*) AS filas,
	   count(i2) AS no_nulos_i2,
	   sum(i8) AS suma_i8,
	   sum(n) AS suma_n,
	   min(d) AS min_d,
	   max(t) AS max_t
FROM c_tipos;

-- ===========================================================================
-- Volumen, NULLs y valores largos
-- ===========================================================================

CREATE TABLE c_datos (id int, txt text, val numeric, opt text)
	USING dbengine_columnar;

INSERT INTO c_datos
SELECT g,
	   repeat('x', (g % 50) + 1),
	   g * 1.25,
	   CASE WHEN g % 3 = 0 THEN NULL ELSE 'v' || g END
FROM generate_series(1, 20000) g;

SELECT count(*), count(opt) AS no_nulos, sum(id), sum(val),
	   sum(length(txt)) AS largo_total
FROM c_datos;

-- Un valor que no entra en una página: el archivo lógico lo reparte solo.
INSERT INTO c_datos VALUES (999999, repeat('y', 100000), 0, NULL);
SELECT id, length(txt) AS largo FROM c_datos WHERE id = 999999;

-- ===========================================================================
-- Stripes: un INSERT ... SELECT es un solo stripe
-- ===========================================================================

CREATE TABLE c_stripes (id int) USING dbengine_columnar;
INSERT INTO c_stripes SELECT generate_series(1, 1000);
SELECT count(*) AS stripes FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('c_stripes'::regclass);

-- Un segmento por columna y por stripe.
SELECT count(*) AS segmentos FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('c_stripes'::regclass);

-- Muchos INSERT de una fila en UNA transacción caen en un solo stripe.
-- Antes daban uno por comando: 5000 INSERT ocupaban 39 MB contra los 288 kB
-- del heap equivalente.
CREATE TABLE c_muchos (a int, b text) USING dbengine_columnar;
BEGIN;
DO $$
BEGIN
	FOR i IN 1..500 LOOP
		INSERT INTO c_muchos VALUES (i, 'v' || i);
	END LOOP;
END
$$;
COMMIT;
SELECT count(*) AS filas, sum(a) AS suma FROM c_muchos;
SELECT count(*) AS stripes FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('c_muchos');
SELECT pg_relation_size('c_muchos') < 100000 AS ocupa_menos_de_100kb;

-- El umbral parte el buffer en varios stripes.
SET dbengine.staging_threshold_rows = 100;
CREATE TABLE c_multi (id int) USING dbengine_columnar;
INSERT INTO c_multi SELECT generate_series(1, 1000);
SELECT count(*) AS filas, sum(id) AS suma FROM c_multi;
SELECT count(*) >= 10 AS al_menos_10_stripes FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('c_multi'::regclass);
RESET dbengine.staging_threshold_rows;

-- ===========================================================================
-- Estadísticas y estimaciones (R6)
-- ===========================================================================

ANALYZE c_datos;

-- La cuenta de filas sale del catálogo interno, así que es exacta.
SELECT reltuples::bigint = (SELECT count(*) FROM c_datos) AS reltuples_exacto
FROM pg_class WHERE oid = 'c_datos'::regclass;

SELECT count(*) AS columnas_con_stats FROM pg_stats WHERE tablename = 'c_datos';

SELECT relpages > 0 AS tiene_paginas FROM pg_class WHERE oid = 'c_datos'::regclass;
SELECT pg_relation_size('c_datos') > 0 AS tiene_tamano;

-- ===========================================================================
-- Índices
-- ===========================================================================

CREATE INDEX c_datos_id_idx ON c_datos (id);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT txt FROM c_datos WHERE id = 777;
SELECT id, val FROM c_datos WHERE id = 777;
SELECT count(*) AS por_rango FROM c_datos WHERE id BETWEEN 100 AND 200;
RESET enable_seqscan;

-- Índice GIN.
--
-- GIN empaqueta los TIDs de su posting list suponiendo que el offset entra
-- en 11 bits.  Con offsets de hasta 2048 —MaxOffsetNumber— se pasaba por
-- uno y el CREATE INDEX abortaba.  Ningún test propio lo había tocado; lo
-- encontró la suite de regresión de Postgres.
CREATE TABLE c_gin (j jsonb) USING dbengine_columnar;
INSERT INTO c_gin SELECT ('{"k":' || g || '}')::jsonb
FROM generate_series(1, 5000) g;
CREATE INDEX c_gin_idx ON c_gin USING gin (j);

SET enable_seqscan = off;
SELECT count(*) AS por_gin FROM c_gin WHERE j @> '{"k":42}';
SELECT count(*) AS rango_gin FROM c_gin WHERE j ? 'k';
RESET enable_seqscan;

-- Scan paralelo por el Table AM, sin Custom Scan.
--
-- table_beginscan_parallel() verifica con un Assert que el phs_locator del
-- descriptor compartido coincida con la relación; ese campo lo tiene que
-- llenar el AM, no el core.
SET dbengine.enable_vectorized = off;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT count(*), count(DISTINCT j) FROM c_gin;
RESET dbengine.enable_vectorized;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- Construcción de índices en paralelo.
--
-- Cada worker recibe el MISMO TableScanDesc compartido, que es el que
-- reparte los stripes.  Ignorarlo y abrir un cursor propio hacía que cada
-- worker recorriera la tabla entera: entradas duplicadas en el índice, y
-- TIDs desordenados que rompían el build de GIN.  Un btree lo hubiera
-- aceptado en silencio.
CREATE TABLE c_par (a int, t text) USING dbengine_columnar;
SET dbengine.staging_threshold_rows = 3000;
INSERT INTO c_par SELECT g, 'v' || g FROM generate_series(1, 30000) g;
RESET dbengine.staging_threshold_rows;

SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
CREATE INDEX c_par_a ON c_par (a);
CREATE INDEX c_par_gin ON c_par USING gin (to_tsvector('simple', t));
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;

SET enable_seqscan = off;
SELECT count(*) AS por_btree FROM c_par WHERE a > 0;
SELECT count(*) AS por_gin FROM c_par
WHERE to_tsvector('simple', t) @@ to_tsquery('simple', 'v12345');
RESET enable_seqscan;
SELECT count(*) AS por_seqscan FROM c_par;

-- Índice parcial y expresión.
CREATE INDEX c_datos_parcial ON c_datos (val) WHERE opt IS NOT NULL;
SELECT count(*) AS con_opt FROM c_datos WHERE opt IS NOT NULL;

-- Índice creado ANTES de insertar: el mantenimiento incremental depende de
-- que tuple_insert() deje el TID definitivo en el slot, no uno provisorio.
CREATE TABLE c_idx_antes (id int, v text) USING dbengine_columnar;
CREATE INDEX ON c_idx_antes (id);
INSERT INTO c_idx_antes SELECT g, 'v' || g FROM generate_series(1, 2000) g;

SET enable_seqscan = off;
SELECT id, v FROM c_idx_antes WHERE id = 1500;
SELECT count(*) AS por_indice FROM c_idx_antes WHERE id BETWEEN 1 AND 2000;
RESET enable_seqscan;
SELECT count(*) AS por_seqscan FROM c_idx_antes;
SELECT count(DISTINCT ctid) AS ctids_unicos FROM c_idx_antes;

-- Índice único: detecta duplicados y ON CONFLICT DO NOTHING los saltea.
--
-- Este caso colgaba el backend.  SnapshotDirty exige que el AM escriba
-- snapshot->xmin/xmax en cada fila que devuelve (InitDirtySnapshot solo fija
-- el tipo, el resto es basura del stack); sin eso
-- check_exclusion_or_unique_constraint reintenta para siempre.
CREATE TABLE c_unico (id int PRIMARY KEY, v text) USING dbengine_columnar;
INSERT INTO c_unico VALUES (1, 'a');
INSERT INTO c_unico VALUES (1, 'b') ON CONFLICT DO NOTHING;
SELECT count(*) AS tras_do_nothing FROM c_unico;

\set ON_ERROR_STOP off
INSERT INTO c_unico VALUES (1, 'c');
\set ON_ERROR_STOP on
SELECT id, v FROM c_unico ORDER BY id;

-- ===========================================================================
-- COPY y CREATE TABLE AS
-- ===========================================================================

CREATE TABLE c_copy (LIKE c_stripes) USING dbengine_columnar;
COPY c_copy FROM PROGRAM 'seq 1 500';
SELECT count(*), sum(id) FROM c_copy;

CREATE TABLE c_ctas USING dbengine_columnar AS
	SELECT id, txt FROM c_datos WHERE id <= 10;
SELECT count(*) FROM c_ctas;

-- ===========================================================================
-- TRUNCATE deja la tabla vacía y reutilizable
-- ===========================================================================

TRUNCATE c_copy;
SELECT count(*) AS tras_truncate FROM c_copy;
SELECT count(*) AS metadatos_tras_truncate FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('c_copy'::regclass);
INSERT INTO c_copy VALUES (42);
SELECT * FROM c_copy;

-- ===========================================================================
-- DDL que toca el storage
--
-- Los metadatos se indexan por relfilenode, no por OID de pg_class: una
-- reescritura cambia el relfilenode dejando el OID igual, y con la clave
-- vieja los stripes quedaban huérfanos y la tabla se leía corrupta.
-- ===========================================================================

CREATE TABLE c_ddl (a int, b int) USING dbengine_columnar;
INSERT INTO c_ddl SELECT g, g * 2 FROM generate_series(1, 500) g;
SELECT count(*) AS antes, sum(b) AS suma_antes FROM c_ddl;

-- Reescritura completa.
ALTER TABLE c_ddl ALTER COLUMN b TYPE float8;
SELECT count(*) AS tras_alter_type, sum(b) AS suma_despues FROM c_ddl;

-- ADD COLUMN con default no reescribe la tabla desde PG 11: guarda el valor
-- en pg_attribute.attmissingval y el AM tiene que completarlo al
-- reconstruir las filas viejas.
ALTER TABLE c_ddl ADD COLUMN c text DEFAULT 'x';
ALTER TABLE c_ddl ADD COLUMN d int;
SELECT count(*) AS filas, count(DISTINCT c) AS valores_de_c, min(c) AS c,
	   count(d) AS d_no_nulos
FROM c_ddl;

-- Filas nuevas conviven con las viejas en stripes distintos.
INSERT INTO c_ddl VALUES (999, 1.5, 'y', 7);
SELECT string_agg(DISTINCT c, ',' ORDER BY c) AS valores_de_c FROM c_ddl;

ALTER TABLE c_ddl DROP COLUMN c;
SELECT count(*) AS tras_drop_column, sum(a) AS suma_a FROM c_ddl;

-- Reescritura con USING y cambio de tipo incompatible.
--
-- ATRewriteTable() arma el slot con tab->oldDesc — el descriptor de ANTES
-- del ALTER — mientras RelationGetDescr() ya devuelve el nuevo.  El AM
-- tiene que interpretar los valores con el descriptor DEL SLOT; usando el
-- de la relación, un segmento de text se leía como si fuera integer.
CREATE TABLE c_using (f text) USING dbengine_columnar;
INSERT INTO c_using VALUES ('1'), ('2'), ('300');
ALTER TABLE c_using ALTER COLUMN f TYPE integer USING f::integer;
SELECT f FROM c_using ORDER BY f;

-- Columna de identidad y varias acciones en un solo ALTER.
CREATE TABLE c_ident (a int GENERATED BY DEFAULT AS IDENTITY, b text)
	USING dbengine_columnar;
INSERT INTO c_ident (b) VALUES ('x'), ('y');
ALTER TABLE c_ident ALTER COLUMN a SET DATA TYPE bigint;
SELECT a, b FROM c_ident ORDER BY a;

CREATE TABLE c_multiacc (col1 int, col2 int) USING dbengine_columnar;
INSERT INTO c_multiacc VALUES (1, 2), (3, 4);
ALTER TABLE c_multiacc
	ALTER COLUMN col1 SET DATA TYPE float8,
	ALTER COLUMN col2 SET DEFAULT 0;
SELECT col1, col2 FROM c_multiacc ORDER BY col1;

-- ===========================================================================
-- VACUUM se acepta y no rompe nada
-- ===========================================================================

VACUUM c_datos;
VACUUM ANALYZE c_datos;
SELECT count(*) AS tras_vacuum FROM c_datos;

-- ===========================================================================
-- Lo que no está soportado tiene que fallar claro, no dar mal resultado
-- ===========================================================================

\set ON_ERROR_STOP off
UPDATE c_datos SET val = 0 WHERE id = 1;
DELETE FROM c_datos WHERE id = 1;
SELECT * FROM c_datos WHERE id = 1 FOR UPDATE;
INSERT INTO c_datos VALUES (1, 'x', 1, NULL)
	ON CONFLICT DO NOTHING;
SELECT * FROM c_datos TABLESAMPLE SYSTEM (10);
\set ON_ERROR_STOP on

-- Después de todos esos errores la tabla sigue intacta.
SELECT count(*) AS intacta FROM c_datos;

-- ===========================================================================
-- VACUUM FULL y CLUSTER: reescriben la tabla entera
--
-- No son "no soportado" desde F6: la reescritura es el mecanismo del
-- clustering adaptativo, y de paso compacta el espacio y rearma zone maps,
-- filtros y compresión con los datos ya agrupados.
-- ===========================================================================

SELECT count(*) AS antes_de_vacuum_full FROM c_datos;
VACUUM FULL c_datos;
SELECT count(*) AS tras_vacuum_full FROM c_datos;

-- Y con un índice btree, ordenando por él.
CREATE INDEX c_datos_id ON c_datos USING btree (id);
CLUSTER c_datos USING c_datos_id;
SELECT count(*) AS tras_cluster FROM c_datos;
-- Las filas no cambiaron, solo su orden en el storage.
SELECT id, val FROM c_datos ORDER BY id LIMIT 3;
DROP INDEX c_datos_id;

-- ===========================================================================
-- DROP limpia los metadatos
--
-- El Table AM no tiene callback de DROP: PostgreSQL borra el archivo por
-- smgr, que es genérico, y nunca avisa al AM.  La limpieza va por
-- object_access_hook.
--
-- Esta sección antes solo borraba las tablas y no miraba el catálogo, así
-- que pasaba igual con las filas quedando huérfanas.  Ahora se cuenta.
-- ===========================================================================

-- Hay algo que limpiar antes de empezar.
SELECT count(*) > 0 AS hay_metadatos
FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('c_stripes');

DROP TABLE c_multi;
DROP TABLE c_muchos;
DROP TABLE c_stripes CASCADE;
DROP TABLE c_copy;
DROP TABLE c_ctas;
DROP TABLE c_tipos;
DROP TABLE c_datos;
DROP TABLE c_idx_antes;
DROP TABLE c_gin;
DROP TABLE c_par;
DROP TABLE c_ddl;
DROP TABLE c_using;
DROP TABLE c_ident;
DROP TABLE c_multiacc;
DROP TABLE c_unico;

-- Ninguna fila apunta a un relfilenode que ya no existe.
SELECT count(*) AS stripes_huerfanos
FROM dbengine.stripe s
WHERE NOT EXISTS (SELECT 1 FROM pg_class c
                  WHERE c.relfilenode = s.relfilenode);
SELECT count(*) AS segmentos_huerfanos
FROM dbengine.stripe_column sc
WHERE NOT EXISTS (SELECT 1 FROM pg_class c
                  WHERE c.relfilenode = sc.relfilenode);

DROP EXTENSION dbengine CASCADE;
