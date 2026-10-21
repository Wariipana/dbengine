-- F3: Custom Scan — column pruning y descarte de stripes por zone maps.

CREATE EXTENSION dbengine;

SET max_parallel_workers_per_gather = 0;
SET dbengine.staging_threshold_rows = 2000;

CREATE TABLE s (
	id     bigint,
	ts     timestamptz,
	region text,
	monto  numeric(12,2),
	nota   text
) USING dbengine_columnar;

INSERT INTO s
SELECT g,
	   '2024-01-01'::timestamptz + g * interval '1 minute',
	   (ARRAY['norte','sur','este','oeste'])[1 + g % 4],
	   (g % 1000) / 10.0,
	   'nota-' || g
FROM generate_series(1, 20000) g;

RESET dbengine.staging_threshold_rows;
ANALYZE s;

SELECT count(*) AS stripes FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('s');

-- ===========================================================================
-- Column pruning
--
-- Es lo que el Table AM no puede hacer: scan_begin() nunca se entera de qué
-- columnas pide la query.  El Custom Scan sí.
-- ===========================================================================

EXPLAIN (COSTS OFF) SELECT count(*) FROM s;
EXPLAIN (COSTS OFF) SELECT sum(monto) FROM s;
EXPLAIN (COSTS OFF) SELECT region, count(*) FROM s GROUP BY region;

-- ===========================================================================
-- Descarte de stripes por zone maps
-- ===========================================================================

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM s WHERE id BETWEEN 100 AND 200;

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM s WHERE id > 19500;

-- Un predicado que no descarta nada: region se repite en todos los stripes.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM s WHERE region = 'norte';

-- ===========================================================================
-- El pruning no puede cambiar ningún resultado
--
-- Cada consulta se corre con el Custom Scan y con el seq scan del Table AM;
-- descartar un stripe que sí tenía filas daría distinto.
-- ===========================================================================

CREATE FUNCTION igual(q text) RETURNS text AS $$
DECLARE
	con text;
	sin text;
BEGIN
	SET LOCAL dbengine.enable_vectorized = on;
	EXECUTE 'SELECT (' || q || ')::text' INTO con;
	SET LOCAL dbengine.enable_vectorized = off;
	EXECUTE 'SELECT (' || q || ')::text' INTO sin;
	IF con IS NOT DISTINCT FROM sin THEN
		RETURN 'igual: ' || coalesce(con, 'NULL');
	END IF;
	RETURN 'DIFIERE con=' || coalesce(con, 'NULL') || ' sin=' || coalesce(sin, 'NULL');
END
$$ LANGUAGE plpgsql;

SELECT igual('SELECT count(*) FROM s WHERE id BETWEEN 100 AND 200');
SELECT igual('SELECT count(*) FROM s WHERE id > 19500');
SELECT igual('SELECT count(*) FROM s WHERE id < 3');
SELECT igual('SELECT count(*) FROM s WHERE id = 12345');
SELECT igual('SELECT count(*) FROM s WHERE id >= 20000');
SELECT igual('SELECT count(*) FROM s WHERE id <= 0');
SELECT igual('SELECT count(*) FROM s WHERE id > 1000000');
SELECT igual('SELECT sum(id) FROM s WHERE id BETWEEN 5000 AND 6000');
SELECT igual('SELECT count(*) FROM s WHERE ts < ''2024-01-01''::timestamptz');
SELECT igual('SELECT count(*) FROM s WHERE nota = ''nota-777''');
SELECT igual('SELECT count(*) FROM s WHERE region = ''sur''');
SELECT igual('SELECT count(*) FROM s WHERE id IS NULL');
SELECT igual('SELECT count(*) FROM s WHERE id = 100 OR id = 15000');
SELECT igual('SELECT count(*) FROM s WHERE monto > 99.0');
-- Tipos cruzados: la columna es bigint y el literal integer.  Es el caso
-- normal, no un borde, y hay que podar igual.
SELECT igual('SELECT count(*) FROM s WHERE id > 19990::int');
SELECT igual('SELECT max(nota) FROM s WHERE id < 50');

-- ===========================================================================
-- Fallback: lo que el Custom Scan no puede acotar vuelve al seq scan
-- ===========================================================================

-- Referencia a la fila entera: hacen falta todas las columnas.
EXPLAIN (COSTS OFF) SELECT count(s.*) FROM s;

-- Columna de sistema que un storage columnar no tiene.
EXPLAIN (COSTS OFF) SELECT count(*) FROM s WHERE xmin::text <> '';

-- Apagarlo a mano también cae al seq scan.
SET dbengine.enable_vectorized = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM s;
RESET dbengine.enable_vectorized;

-- ===========================================================================
-- Scan paralelo
-- ===========================================================================

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;

SELECT count(*), sum(id) FROM s;
SELECT count(*) FROM s WHERE id BETWEEN 100 AND 200;
SELECT igual('SELECT sum(id) FROM s');

RESET ALL;
SET max_parallel_workers_per_gather = 0;

-- Apagar el descarte por GUC no cambia ningún resultado, solo lo apaga.
SET dbengine.enable_zonemap_pruning = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM s WHERE id BETWEEN 100 AND 200;
SELECT igual('SELECT count(*) FROM s WHERE id BETWEEN 100 AND 200');
RESET dbengine.enable_zonemap_pruning;

-- ---------------------------------------------------------------------------
-- Late materialization
--
-- El predicado se evalúa con SOLO las columnas que mira; las demás se
-- decodifican recién para las filas que lo pasan.  Por eso el predicado no
-- vive en plan.qual sino adentro del nodo — el `Filter:` lo imprime el nodo.
-- ---------------------------------------------------------------------------

SET dbengine.enable_agg_pushdown = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT id, nota FROM s WHERE id BETWEEN 100 AND 200;

-- Sin predicado se llenan todas las columnas igual.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(nota) FROM s;

SELECT igual('SELECT count(nota) FROM s');
SELECT igual('SELECT sum(length(nota)) FROM s WHERE id < 500');
SELECT igual('SELECT max(nota) FROM s WHERE region = ''sur''');
-- La fila entera: todas las columnas se materializan.
SELECT igual('SELECT count(*) FROM (SELECT * FROM s WHERE id BETWEEN 100 AND 200) z');
SELECT igual('SELECT md5(string_agg(t.*::text, ''|'' ORDER BY t.id)) FROM (SELECT * FROM s WHERE id < 50) t');
-- Predicado sobre una columna que la proyección NO pide.
SELECT igual('SELECT sum(id) FROM s WHERE nota = ''nota-42''');
-- Predicado sobre una columna que sí pide.
SELECT igual('SELECT count(*) FROM (SELECT nota FROM s WHERE nota IS NOT NULL) z');
RESET dbengine.enable_agg_pushdown;

-- ===========================================================================
-- Particiones
--
-- Una partición es RELOPT_OTHER_MEMBER_REL, no RELOPT_BASEREL.  El hook
-- las trata igual: el reltarget y el baserestrictinfo del hijo ya vienen
-- traducidos a SUS números de columna.
-- ===========================================================================

CREATE TABLE sp (id bigint, region text, nota text) PARTITION BY RANGE (id);
CREATE TABLE sp1 PARTITION OF sp FOR VALUES FROM (0) TO (10000)
	USING dbengine_columnar;
CREATE TABLE sp2 PARTITION OF sp FOR VALUES FROM (10000) TO (20000)
	USING dbengine_columnar;
CREATE TABLE sp_heap (LIKE sp);
INSERT INTO sp SELECT g, (ARRAY['norte','sur'])[1 + g % 2], 'nota-' || g
	FROM generate_series(0, 19999) g;
INSERT INTO sp_heap SELECT * FROM sp;
ANALYZE sp;

-- Cada partición recibe su propio nodo, con pruning y column pruning.
EXPLAIN (COSTS OFF) SELECT id FROM sp WHERE region = 'norte';
-- Con poda de particiones queda una sola.
EXPLAIN (COSTS OFF) SELECT id, nota FROM sp WHERE id BETWEEN 15000 AND 15010;

CREATE FUNCTION igual_part(q text) RETURNS text AS $$
DECLARE
	col text;
	hea text;
BEGIN
	EXECUTE 'SELECT (' || replace(q, '@', 'sp') || ')::text' INTO col;
	EXECUTE 'SELECT (' || replace(q, '@', 'sp_heap') || ')::text' INTO hea;
	IF col IS NOT DISTINCT FROM hea THEN
		RETURN 'igual: ' || coalesce(col, 'NULL');
	END IF;
	RETURN 'DIFIERE columnar=' || coalesce(col, 'NULL') ||
		   ' heap=' || coalesce(hea, 'NULL');
END
$$ LANGUAGE plpgsql;

SELECT igual_part('SELECT count(*) FROM @');
SELECT igual_part('SELECT count(*) FROM @ WHERE region = ''norte''');
SELECT igual_part('SELECT sum(id) FROM @ WHERE id BETWEEN 15000 AND 15010');
SELECT igual_part('SELECT max(nota) FROM @ WHERE region = ''sur''');
SELECT igual_part('SELECT md5(string_agg(x.*::text, ''|'' ORDER BY x.id))
                   FROM (SELECT * FROM @ WHERE id < 100) x');
-- Predicado que no deja pasar nada en una partición y sí en la otra.
SELECT igual_part('SELECT count(*) FROM @ WHERE id > 19990');

DROP FUNCTION igual_part(text);
DROP TABLE sp, sp_heap;

-- ===========================================================================
-- Agregación empujada al scan
--
-- El nodo Agg pide una fila por vez; para agregar hay que construir una
-- tupla por fila y tirarla.  Este nodo alimenta las funciones de transición
-- del propio catálogo directamente, sin armar tuplas intermedias.
-- ===========================================================================

EXPLAIN (COSTS OFF) SELECT count(*) FROM s;
EXPLAIN (COSTS OFF) SELECT sum(monto), min(id), max(id) FROM s;

-- Los agregados tienen que dar EXACTAMENTE lo mismo que el plan normal:
-- se usan las funciones de transición de pg_aggregate, así que sum(int4)
-- desborda igual y avg(numeric) redondea igual.
SELECT igual('SELECT count(*) FROM s');
SELECT igual('SELECT count(id) FROM s');
SELECT igual('SELECT sum(id) FROM s');
SELECT igual('SELECT sum(monto) FROM s');
SELECT igual('SELECT avg(monto) FROM s');
SELECT igual('SELECT min(id) FROM s');
SELECT igual('SELECT max(nota) FROM s');
SELECT igual('SELECT min(ts) FROM s');
SELECT igual('SELECT count(region) FROM s');

-- Tabla vacía: sum() da NULL y count() da 0, igual que siempre.
CREATE TABLE svacia (a int, b numeric) USING dbengine_columnar;
SELECT igual('SELECT count(*) FROM svacia');
SELECT igual('SELECT sum(a) FROM svacia');
SELECT igual('SELECT avg(b) FROM svacia');
SELECT igual('SELECT min(a) FROM svacia');

-- Todo nulo: los nulos no entran en la transición.
CREATE TABLE snulos (a int, b numeric) USING dbengine_columnar;
INSERT INTO snulos SELECT NULL, NULL FROM generate_series(1, 100);
SELECT igual('SELECT count(*) FROM snulos');
SELECT igual('SELECT count(a) FROM snulos');
SELECT igual('SELECT sum(a) FROM snulos');
SELECT igual('SELECT avg(b) FROM snulos');

-- GROUP BY sí se empuja, pero a esta escala el plan normal cuesta menos y
-- gana; lo que se prueba en test/regress/sql/grouped.sql.
EXPLAIN (COSTS OFF) SELECT region, count(*) FROM s GROUP BY region;
-- count(DISTINCT) sin GROUP BY sí: un conjunto de hash sobre la columna.
EXPLAIN (COSTS OFF) SELECT count(DISTINCT region) FROM s;
-- FILTER sigue cayendo al plan normal.
EXPLAIN (COSTS OFF) SELECT count(*) FILTER (WHERE id < 100) FROM s;
SELECT igual('SELECT count(DISTINCT region) FROM s');
SELECT igual('SELECT count(*) FILTER (WHERE id < 100) FROM s');

-- ---------------------------------------------------------------------------
-- Agregación CON predicado
--
-- El WHERE se evalúa adentro del nodo, contra un slot que trae solo las
-- columnas que el predicado mira.  Los stripes que el zone map descarta no
-- llegan a evaluarse.
-- ---------------------------------------------------------------------------

EXPLAIN (COSTS OFF) SELECT count(*) FROM s WHERE id < 100;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT sum(monto) FROM s WHERE id BETWEEN 100 AND 200;

SELECT igual('SELECT count(*) FROM s WHERE id < 100');
SELECT igual('SELECT sum(id) FROM s WHERE id > 19500');
SELECT igual('SELECT sum(monto) FROM s WHERE region = ''norte''');
SELECT igual('SELECT avg(monto) FROM s WHERE id BETWEEN 100 AND 200');
SELECT igual('SELECT min(ts) FROM s WHERE region <> ''norte''');
SELECT igual('SELECT max(ts) FROM s WHERE region <> ''norte''');
-- Predicado sobre una columna que ningún agregado lee.
SELECT igual('SELECT count(*) FROM s WHERE region = ''sur''');
-- Predicado que no deja pasar ninguna fila.
SELECT igual('SELECT count(*) FROM s WHERE id > 999999');
SELECT igual('SELECT sum(id) FROM s WHERE id > 999999');
-- Predicado sobre nulos, y una expresión que no es `columna OP constante`.
SELECT igual('SELECT count(*) FROM s WHERE nota IS NULL');
SELECT igual('SELECT count(*) FROM s WHERE id % 7 = 0');
SELECT igual('SELECT sum(id) FROM s WHERE region || ''!'' = ''norte!''');

-- El predicado no cambia con la visibilidad: las filas de la propia
-- transacción tienen que contarse igual que en el plan normal.
BEGIN;
INSERT INTO s(id, region, monto, nota, ts) VALUES (30000, 'norte', 1, 'x', now());
SELECT igual('SELECT count(*) FROM s WHERE region = ''norte''');
SELECT igual('SELECT sum(monto) FROM s WHERE id >= 30000');
ROLLBACK;
SELECT igual('SELECT count(*) FROM s WHERE region = ''norte''');

-- ===========================================================================
-- count(*), min y max se responden desde los metadatos
--
-- El row_count y el zone map de cada stripe alcanzan: no se lee un byte de
-- datos.  Por eso mismo la visibilidad tiene que salir de ahí también, y un
-- stripe con visibilidad parcial obliga a caer al recorrido normal — su
-- zone map abarca filas que el snapshot todavía no ve.
-- ===========================================================================

CREATE FUNCTION igual_fila(q text) RETURNS text AS $$
DECLARE
	con text;
	sin text;
BEGIN
	SET LOCAL dbengine.enable_agg_pushdown = on;
	EXECUTE 'SELECT row(x.*)::text FROM (' || q || ') x' INTO con;
	SET LOCAL dbengine.enable_agg_pushdown = off;
	EXECUTE 'SELECT row(x.*)::text FROM (' || q || ') x' INTO sin;
	IF con IS NOT DISTINCT FROM sin THEN
		RETURN 'igual: ' || coalesce(con, 'NULL');
	END IF;
	RETURN 'DIFIERE push=' || coalesce(con, 'NULL') || ' normal=' || coalesce(sin, 'NULL');
END
$$ LANGUAGE plpgsql;

EXPLAIN (COSTS OFF) SELECT min(id), max(id) FROM s;

SELECT igual_fila('SELECT min(id), max(id), count(*) FROM s');
SELECT igual_fila('SELECT min(ts), max(ts) FROM s');
SELECT igual_fila('SELECT min(region), max(region) FROM s');
SELECT igual_fila('SELECT min(monto), max(monto) FROM s');

CREATE TABLE smm (a int, t text, n numeric) USING dbengine_columnar;
SELECT igual_fila('SELECT min(a), max(a) FROM smm') AS tabla_vacia;
INSERT INTO smm SELECT NULL, NULL, NULL FROM generate_series(1, 50);
SELECT igual_fila('SELECT min(a), max(t), min(n) FROM smm') AS todo_nulo;
INSERT INTO smm SELECT g, 'v' || g, g / 7.0 FROM generate_series(1, 500) g;
SELECT igual_fila('SELECT min(a), max(a), min(t), max(t), min(n), max(n) FROM smm')
	AS con_nulos_y_valores;

-- Adentro de la transacción el stripe queda parcialmente visible y el
-- atajo no aplica; el resultado tiene que ser el mismo igual.
BEGIN;
INSERT INTO smm VALUES (-99, 'aaa', -1);
SELECT igual_fila('SELECT min(a), max(t) FROM smm') AS dentro_de_txn;
ROLLBACK;
SELECT igual_fila('SELECT min(a), max(t) FROM smm') AS tras_rollback;

-- Valores demasiado largos para guardar un zone map: sin atajo, mismo
-- resultado.
CREATE TABLE slargo (t text) USING dbengine_columnar;
INSERT INTO slargo SELECT repeat('z', 300) || g FROM generate_series(1, 100) g;
SELECT igual_fila('SELECT length(min(t)), length(max(t)) FROM slargo') AS sin_zonemap;

DROP TABLE smm, slargo;

-- ===========================================================================
-- Agregación parcial en paralelo
--
-- Con workers disponibles el nodo hace de Partial Agg: entrega el estado de
-- transición (serializado si es `internal`) y el Finalize Agg de arriba lo
-- combina.  Es la misma forma que arma el planner, pero sin construir una
-- tupla por fila en cada worker.
-- ===========================================================================

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;

CREATE TABLE spar (a bigint, n numeric, t text) USING dbengine_columnar;
SET dbengine.staging_threshold_rows = 2000;
INSERT INTO spar SELECT g, g / 3.0, 'v' || g FROM generate_series(1, 20000) g;
INSERT INTO spar SELECT NULL, NULL, NULL FROM generate_series(1, 100) g;
RESET dbengine.staging_threshold_rows;
ANALYZE spar;

EXPLAIN (COSTS OFF) SELECT sum(a) FROM spar;

-- Los estados parciales se combinan sin perder nada: sum(bigint) y
-- avg(numeric) llevan estado `internal` y tienen que viajar serializados.
SELECT igual_fila('SELECT sum(a), count(a), count(*) FROM spar');
SELECT igual_fila('SELECT sum(n), avg(n) FROM spar');
SELECT igual_fila('SELECT avg(a), min(a), max(a) FROM spar');
SELECT igual_fila('SELECT min(t), max(t), count(t) FROM spar');

DROP TABLE spar;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

-- count(*) se responde desde los metadatos, así que la visibilidad tiene
-- que salir de ahí también.
CREATE TABLE svis (a int) USING dbengine_columnar;
INSERT INTO svis SELECT generate_series(1, 50);
BEGIN;
INSERT INTO svis VALUES (99);
SELECT igual('SELECT count(*) FROM svis');
INSERT INTO svis VALUES (98);
SELECT igual('SELECT count(*) FROM svis');
SAVEPOINT sp;
INSERT INTO svis VALUES (97);
ROLLBACK TO sp;
SELECT igual('SELECT count(*) FROM svis');
COMMIT;
SELECT igual('SELECT count(*) FROM svis');

DROP TABLE svacia, snulos, svis;
RESET ALL;

DROP FUNCTION igual(text);
DROP FUNCTION igual_fila(text);
DROP TABLE s;
DROP EXTENSION dbengine CASCADE;
