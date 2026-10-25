--
-- Filas repetidas descartadas en el scan (rowdedup.h).
--
-- Debajo de un GROUP BY cuyos agregados ignoran duplicados, el scan
-- descarta las filas iguales dentro de cada stripe.  Todo se compara contra
-- heap: el descarte no puede cambiar ningún resultado.
--
CREATE EXTENSION dbengine;

CREATE TABLE dd (g int4, u int8, t text, f float8, n numeric, x int4)
	USING dbengine_columnar;
-- Tres stripes.  Los mismos valores se repiten dentro de cada uno y también
-- entre stripes, con nulos en todas las columnas.
INSERT INTO dd SELECT i % 7, (i / 3) % 50, 'v' || (i % 11), (i % 5) * 0.5,
	(i % 4)::numeric / 3, i FROM generate_series(1, 3000) i;
INSERT INTO dd SELECT CASE WHEN i % 9 = 0 THEN NULL ELSE i % 7 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (i / 3) % 50 END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE 'v' || (i % 11) END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE (i % 5) * 0.5 END,
	NULL, i FROM generate_series(1, 3000) i;
-- Texto sin diccionario: valores todos distintos.
INSERT INTO dd SELECT i % 3, i % 2, md5(i::text) || md5((i * 7)::text), -0.0,
	1, i FROM generate_series(1, 2000) i;
ANALYZE dd;
CREATE TABLE dd_heap AS SELECT * FROM dd;

SET max_parallel_workers_per_gather = 0;

-- El plan: Sort y GroupAggregate de siempre, con el descarte en el scan.
EXPLAIN (COSTS OFF) SELECT g, count(DISTINCT u) FROM dd GROUP BY g;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT g, count(DISTINCT u) FROM dd GROUP BY g;

SELECT g, count(DISTINCT u) FROM dd GROUP BY g ORDER BY g;
SELECT g, count(DISTINCT u) FROM dd_heap GROUP BY g ORDER BY g;

-- min y max sin DISTINCT, otros agregados con DISTINCT, varias columnas.
SELECT g, min(t), max(u), count(DISTINCT t), sum(DISTINCT u), max(f)
FROM dd GROUP BY g ORDER BY g;
SELECT g, min(t), max(u), count(DISTINCT t), sum(DISTINCT u), max(f)
FROM dd_heap GROUP BY g ORDER BY g;

-- Agrupando por texto, con WHERE y HAVING.
SELECT t, count(DISTINCT g) FROM dd WHERE x % 2 = 0 GROUP BY t
HAVING count(DISTINCT u) > 3 ORDER BY t;
SELECT t, count(DISTINCT g) FROM dd_heap WHERE x % 2 = 0 GROUP BY t
HAVING count(DISTINCT u) > 3 ORDER BY t;

-- Una columna que no es por valor ni tiene diccionario (numeric): esos
-- stripes pasan enteros, y el resultado es el mismo.
SELECT g, count(DISTINCT n) FROM dd GROUP BY g ORDER BY g;
SELECT g, count(DISTINCT n) FROM dd_heap GROUP BY g ORDER BY g;

-- -0 y 0 son iguales para float8 pero distintos en binario: suben los dos y
-- el DISTINCT de arriba los junta.
SELECT g, count(DISTINCT f) FROM dd WHERE x <= 2000 GROUP BY g ORDER BY g;
SELECT g, count(DISTINCT f) FROM dd_heap WHERE x <= 2000 GROUP BY g ORDER BY g;

-- Un count(*) cuenta repeticiones: con él no hay descarte.
EXPLAIN (COSTS OFF) SELECT g, count(DISTINCT u), count(*) FROM dd GROUP BY g;
-- Ni con una función volátil, que vería menos filas.
EXPLAIN (COSTS OFF)
SELECT (g + random() * 0)::int AS r, count(DISTINCT u) FROM dd GROUP BY 1;

-- Rescan: el scan del lado de adentro de un nested loop se recorre de nuevo
-- por cada fila de afuera y el descarte arranca de cero cada vez.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT k, (SELECT count(*) FROM (SELECT g, count(DISTINCT u) FROM dd
		   WHERE g IS NOT NULL AND g <= k GROUP BY g) s)
FROM generate_series(0, 3) k;
SELECT k, (SELECT count(*) FROM (SELECT g, count(DISTINCT u) FROM dd_heap
		   WHERE g IS NOT NULL AND g <= k GROUP BY g) s)
FROM generate_series(0, 3) k;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- En paralelo: cada worker descarta en sus stripes y ordena, y un Gather
-- Merge junta.
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT g, count(DISTINCT u) FROM dd GROUP BY g;
SELECT g, count(DISTINCT u), min(t) FROM dd GROUP BY g ORDER BY g;
SELECT g, count(DISTINCT u), min(t) FROM dd_heap GROUP BY g ORDER BY g;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

-- Apagado, el plan normal.
SET dbengine.enable_scan_dedup = off;
EXPLAIN (COSTS OFF) SELECT g, count(DISTINCT u) FROM dd GROUP BY g;
RESET dbengine.enable_scan_dedup;

DROP TABLE dd, dd_heap;
RESET max_parallel_workers_per_gather;
DROP EXTENSION dbengine CASCADE;
