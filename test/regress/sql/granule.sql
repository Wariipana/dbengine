-- Descarte anidado: gránulos dentro del stripe.
--
-- El stripe es la unidad de ESCRITURA —65.536 filas, elegido para que el
-- segmento comprima y el I/O sea secuencial— y achicarlo para descartar más
-- fino sale caro: medido, con stripes de 8.192 el scan completo cae a 0,41x.
-- Así que la unidad de DESCARTE se separa de la de escritura, que es lo que
-- hacen ORC, Parquet y ClickHouse.
--
-- Lo que se prueba: que descarte, que NO cambie ningún resultado, y que
-- siga siendo correcto en los casos donde el min/max por gránulo se puede
-- leer mal — nulos, columnas sin orden físico, tipos que no aplican.
CREATE EXTENSION dbengine;

SET max_parallel_workers_per_gather = 0;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

-- 200.000 filas = 3 stripes llenos (8 gránulos cada uno) + 1 corto.
CREATE TABLE t (k int, ruido int, txt text) USING dbengine_columnar;
INSERT INTO t SELECT g, g % 97, 'v' || (g % 13)
	FROM generate_series(1, 200000) g;
CREATE TABLE th AS SELECT * FROM t;
ANALYZE t;

-- ===========================================================================
-- El bloque se escribe, y solo donde sirve
-- ===========================================================================

-- El stripe corto tiene un solo gránulo: guardar su min/max sería repetir
-- el del stripe.  `txt` es de largo variable: no lleva bloque nunca.
SELECT count(*) FILTER (WHERE granules IS NOT NULL) AS con_bloque,
	   count(*) AS filas
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('t');

-- 8 gránulos x 4 bytes: 8 de header + 8 de flags + 32 de min + 32 de max.
SELECT DISTINCT octet_length(granules) AS bytes
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('t') AND granules IS NOT NULL;

-- ===========================================================================
-- Descarta, y el resultado no cambia
-- ===========================================================================

-- `k` sigue el orden de inserción, así que cada gránulo cubre un rango
-- disjunto y un BETWEEN angosto cae en uno solo.
EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off)
	SELECT count(*) FROM t WHERE k BETWEEN 70000 AND 70100;

SET dbengine.enable_granule_pruning = off;
EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off)
	SELECT count(*) FROM t WHERE k BETWEEN 70000 AND 70100;
RESET dbengine.enable_granule_pruning;

-- Contra heap, que es lo único que decide si está bien.
SELECT (SELECT count(*) FROM t WHERE k BETWEEN 70000 AND 70100)
	 = (SELECT count(*) FROM th WHERE k BETWEEN 70000 AND 70100) AS count_igual;
SELECT (SELECT sum(ruido) FROM t WHERE k BETWEEN 70000 AND 70100)
	 = (SELECT sum(ruido) FROM th WHERE k BETWEEN 70000 AND 70100) AS suma_igual;
SELECT (SELECT md5(string_agg(txt, ',' ORDER BY k)) FROM t WHERE k < 30000)
	 = (SELECT md5(string_agg(txt, ',' ORDER BY k)) FROM th WHERE k < 30000)
	AS texto_igual;

-- Igualdad, no solo rango.
SELECT (SELECT count(*) FROM t WHERE k = 123456)
	 = (SELECT count(*) FROM th WHERE k = 123456) AS eq_igual;

-- Sobre una columna SIN orden físico no hay nada que descartar, y tiene que
-- devolver lo mismo igual.
SELECT (SELECT count(*) FROM t WHERE ruido = 42)
	 = (SELECT count(*) FROM th WHERE ruido = 42) AS sin_orden_igual;

-- ===========================================================================
-- Nulos
--
-- Un gránulo entero nulo no se descarta aunque ningún operador de
-- comparación sea verdadero para NULL: descartar de más devolvería filas de
-- menos, y el conservadurismo cuesta un gránulo leído.
-- ===========================================================================
CREATE TABLE n (k int) USING dbengine_columnar;
-- Los primeros 8192 valores no nulos, los siguientes 8192 todos nulos, etc.
INSERT INTO n SELECT CASE WHEN (g / 8192) % 2 = 1 THEN NULL ELSE g END
	FROM generate_series(1, 100000) g;
CREATE TABLE nh AS SELECT * FROM n;
ANALYZE n;

SELECT (SELECT count(*) FROM n WHERE k BETWEEN 1000 AND 2000)
	 = (SELECT count(*) FROM nh WHERE k BETWEEN 1000 AND 2000) AS nulos_rango_igual;
SELECT (SELECT count(*) FROM n WHERE k IS NULL)
	 = (SELECT count(*) FROM nh WHERE k IS NULL) AS nulos_isnull_igual;
SELECT (SELECT count(*) FROM n) = (SELECT count(*) FROM nh) AS total_igual;

-- ===========================================================================
-- Tipos que no llevan bloque
--
-- Solo los de ancho fijo por valor de 1, 2, 4 u 8 bytes.  El resto no falla:
-- simplemente no descarta.
-- ===========================================================================
CREATE TABLE v (s text, b bytea, num numeric) USING dbengine_columnar;
INSERT INTO v SELECT 'x' || g, ('\x' || lpad(to_hex(g), 8, '0'))::bytea, g
	FROM generate_series(1, 100000) g;
CREATE TABLE vh AS SELECT * FROM v;
ANALYZE v;

SELECT count(*) FILTER (WHERE granules IS NOT NULL) AS con_bloque
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('v');

SELECT (SELECT count(*) FROM v WHERE s = 'x50000')
	 = (SELECT count(*) FROM vh WHERE s = 'x50000') AS texto_igual;
SELECT (SELECT count(*) FROM v WHERE num BETWEEN 1000 AND 2000)
	 = (SELECT count(*) FROM vh WHERE num BETWEEN 1000 AND 2000) AS numeric_igual;

DROP TABLE t, th, n, nh, v, vh;
DROP EXTENSION dbengine;
