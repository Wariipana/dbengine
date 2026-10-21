-- F1 / ADR-002: visibilidad de stripes.
--
-- El buffer de escritura junta filas y las escribe tarde, así que la
-- visibilidad no puede depender del MVCC de la fila de metadatos: se decide
-- con insert_xid / insert_cid.  Estos casos son la evidencia de que la
-- regla es correcta, sobre todo con subtransacciones de por medio.

CREATE EXTENSION dbengine;

CREATE TABLE mv (id int, t text) USING dbengine_columnar;

-- ===========================================================================
-- Misma transacción, comandos distintos
-- ===========================================================================

BEGIN;
INSERT INTO mv VALUES (1, 'a');
SELECT count(*) AS ve_su_propio_insert FROM mv;
INSERT INTO mv VALUES (2, 'b');
SELECT count(*) AS ve_los_dos FROM mv;
COMMIT;
SELECT count(*) AS tras_commit FROM mv;

-- Un INSERT no se ve a sí mismo dentro del mismo comando (regla de Halloween).
BEGIN;
INSERT INTO mv SELECT g, 'x' FROM generate_series(3, 5) g;
SELECT count(*) AS tras_insert_select FROM mv;
COMMIT;

-- ===========================================================================
-- Un stripe abarca varios comandos
--
-- El buffer de escritura junta filas de toda la transacción, no de un solo
-- comando: si no, N INSERT de una fila daban N stripes.  Para que eso sea
-- correcto, el stripe guarda el command id de CADA fila (segmento attnum 0)
-- y la visibilidad se decide fila por fila.
-- ===========================================================================

-- Sin scans intercalados el buffer no se corta: un solo stripe para toda la
-- transacción, con rango de command ids y segmento de cids.
CREATE TABLE mvc (a int) USING dbengine_columnar;

BEGIN;
INSERT INTO mvc VALUES (1);
INSERT INTO mvc VALUES (2);
INSERT INTO mvc VALUES (3);
COMMIT;

SELECT array_agg(a ORDER BY a) AS datos FROM mvc;

SELECT count(*) AS stripes,
	   bool_or(insert_cid < insert_cid_max) AS abarca_varios_comandos
FROM dbengine.stripe WHERE relfilenode = pg_relation_filenode('mvc');

SELECT count(*) FILTER (WHERE attnum = 0) > 0 AS tiene_segmento_de_cids
FROM dbengine.stripe_column WHERE relfilenode = pg_relation_filenode('mvc');

-- Un SELECT en el medio sí cierra el buffer, porque un scan solo ve lo que
-- está en el storage.  Lo que importa es que cada comando vea exactamente
-- lo que insertaron los anteriores.
CREATE TABLE mvi2 (a int) USING dbengine_columnar;
BEGIN;
INSERT INTO mvi2 VALUES (1);
SELECT count(*) AS ve_1 FROM mvi2;
INSERT INTO mvi2 VALUES (2);
SELECT count(*) AS ve_2 FROM mvi2;
INSERT INTO mvi2 VALUES (3);
SELECT array_agg(a ORDER BY a) AS ve_3 FROM mvi2;
COMMIT;
SELECT array_agg(a ORDER BY a) AS tras_commit FROM mvi2;

-- Un INSERT no puede leer lo que él mismo escribe (regla de Halloween):
-- las filas del comando en curso quedan invisibles para su propio scan.
CREATE TABLE mvh (a int) USING dbengine_columnar;
BEGIN;
INSERT INTO mvh VALUES (1), (2);
INSERT INTO mvh SELECT a + 100 FROM mvh;
COMMIT;
SELECT array_agg(a ORDER BY a) AS sin_halloween FROM mvh;

-- Un solo comando no paga el segmento de cids.
CREATE TABLE mvb (a int) USING dbengine_columnar;
INSERT INTO mvb SELECT generate_series(1, 1000);
SELECT count(*) FILTER (WHERE attnum = 0) = 0 AS bulk_sin_segmento_de_cids
FROM dbengine.stripe_column WHERE relfilenode = pg_relation_filenode('mvb');

-- ===========================================================================
-- ROLLBACK
-- ===========================================================================

BEGIN;
INSERT INTO mv VALUES (100, 'rollback');
SELECT count(*) AS antes_del_rollback FROM mv;
ROLLBACK;
SELECT count(*) AS tras_rollback FROM mv;
SELECT count(*) AS filas_100 FROM mv WHERE id = 100;

-- ===========================================================================
-- Subtransacciones
--
-- El caso que importa: la fila 4 queda en el buffer del nivel externo, y el
-- INSERT de la 5 fuerza su flush YA ADENTRO del savepoint.  Si el stripe se
-- registrara sin más, el ROLLBACK TO se llevaría puesta la fila 4.
-- ===========================================================================

CREATE TABLE mvs (id int) USING dbengine_columnar;

BEGIN;
INSERT INTO mvs VALUES (4);
SAVEPOINT sp;
INSERT INTO mvs VALUES (5);
ROLLBACK TO sp;
SELECT array_agg(id ORDER BY id) AS tras_rollback_to FROM mvs;
COMMIT;
SELECT array_agg(id ORDER BY id) AS tras_commit FROM mvs;

-- Savepoints anidados: cada nivel se deshace por separado.
CREATE TABLE mvn (id int) USING dbengine_columnar;

BEGIN;
INSERT INTO mvn VALUES (1);
SAVEPOINT s1;
INSERT INTO mvn VALUES (2);
SAVEPOINT s2;
INSERT INTO mvn VALUES (3);
ROLLBACK TO s2;
SELECT array_agg(id ORDER BY id) AS tras_rollback_s2 FROM mvn;
ROLLBACK TO s1;
SELECT array_agg(id ORDER BY id) AS tras_rollback_s1 FROM mvn;
COMMIT;
SELECT array_agg(id ORDER BY id) AS final FROM mvn;

-- RELEASE SAVEPOINT conserva todo.
CREATE TABLE mvr (id int) USING dbengine_columnar;

BEGIN;
INSERT INTO mvr VALUES (1);
SAVEPOINT s;
INSERT INTO mvr VALUES (2);
RELEASE SAVEPOINT s;
INSERT INTO mvr VALUES (3);
COMMIT;
SELECT array_agg(id ORDER BY id) AS tras_release FROM mvr;

-- Bloque EXCEPTION de PL/pgSQL: también es una subtransacción.
CREATE TABLE mvp (id int) USING dbengine_columnar;

DO $$
BEGIN
	INSERT INTO mvp VALUES (1);
	BEGIN
		INSERT INTO mvp VALUES (2);
		RAISE EXCEPTION 'forzado';
	EXCEPTION WHEN OTHERS THEN
		RAISE NOTICE 'subtransacción abortada';
	END;
	INSERT INTO mvp VALUES (3);
END
$$;
SELECT array_agg(id ORDER BY id) AS tras_bloque_exception FROM mvp;

-- ===========================================================================
-- Aislamiento entre transacciones: un REPEATABLE READ no ve lo que se
-- commitea después de su snapshot.
-- ===========================================================================

CREATE TABLE mvi (id int) USING dbengine_columnar;
INSERT INTO mvi VALUES (1);

BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS ve_una FROM mvi;    -- fija el snapshot
COMMIT;

-- ===========================================================================
-- El TID es estable: el mismo índice devuelve la misma fila
-- ===========================================================================

CREATE TABLE mvt (id int, t text) USING dbengine_columnar;
INSERT INTO mvt SELECT g, 't'||g FROM generate_series(1, 100) g;
CREATE INDEX ON mvt (id);

SET enable_seqscan = off;
SELECT ctid = (SELECT ctid FROM mvt WHERE id = 50) AS ctid_estable
FROM mvt WHERE id = 50;
RESET enable_seqscan;

SELECT count(DISTINCT ctid) AS ctids_unicos, count(*) AS filas FROM mvt;

DROP TABLE mv, mvc, mvi2, mvh, mvb, mvs, mvn, mvr, mvp, mvi, mvt;
DROP EXTENSION dbengine CASCADE;
