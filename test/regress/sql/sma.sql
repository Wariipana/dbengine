--
-- Agregados precalculados por segmento (SMA, C2).
--
-- sum, avg y count(col) sobre smallint/integer se contestan con la suma
-- exacta y el null_count del catálogo, sin leer el stripe.  Todo se compara
-- contra heap: los casos son los que podrían romper el atajo.
--

-- La ruta de actualización: 0.1.0 no tenía la columna de la suma.
CREATE EXTENSION dbengine VERSION '0.1.0';
ALTER EXTENSION dbengine UPDATE;
SELECT extversion FROM pg_extension WHERE extname = 'dbengine';
SELECT count(*) AS tiene_sum_value FROM pg_attribute
WHERE attrelid = 'dbengine.stripe_column'::regclass AND attname = 'sum_value';

SET max_parallel_workers_per_gather = 0;

CREATE TABLE sm (a int2, b int4, c int8, t text) USING dbengine_columnar;
-- Un stripe por INSERT: con nulos, sin nulos, todo nulo, negativos.
INSERT INTO sm SELECT g % 100, g * 3, g, 'x' FROM generate_series(1, 5000) g;
INSERT INTO sm SELECT CASE WHEN g % 3 = 0 THEN NULL ELSE -g % 50 END,
                      CASE WHEN g % 5 = 0 THEN NULL ELSE 2000000000 END, NULL, NULL
FROM generate_series(1, 4000) g;
INSERT INTO sm SELECT NULL, NULL, NULL, NULL FROM generate_series(1, 300);
INSERT INTO sm VALUES (32767, 2147483647, 9223372036854775807, 'max');
CREATE TABLE sm_heap AS SELECT * FROM sm;

-- La suma se guarda para los enteros y no para el texto.
SELECT a.attname, count(*) AS stripes, count(sc.sum_value) AS con_suma
FROM dbengine.stripe_column sc
JOIN pg_attribute a ON a.attrelid = 'sm'::regclass AND a.attnum = sc.attnum
WHERE sc.relfilenode = pg_relation_filenode('sm')
GROUP BY a.attname ORDER BY a.attname;

-- Sin WHERE: los stripes salen enteros del catálogo.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT sum(a), avg(b), count(t), count(*) FROM sm;

SELECT sum(a), sum(b), avg(a), avg(b), count(a), count(b), count(c), count(t), count(*)
FROM sm;
SELECT sum(a), sum(b), avg(a), avg(b), count(a), count(b), count(c), count(t), count(*)
FROM sm_heap;

-- Mezclado con agregados que no tienen SMA: esos leen el stripe y los
-- demás igual salen del catálogo.
SELECT sum(b), avg(a), sum(c), min(t), max(a) FROM sm;
SELECT sum(b), avg(a), sum(c), min(t), max(a) FROM sm_heap;

-- Con WHERE no aplica: el catálogo describe todas las filas del stripe.
SELECT sum(b), count(a) FROM sm WHERE b > 100;
SELECT sum(b), count(a) FROM sm_heap WHERE b > 100;

-- Filas que este snapshot todavía no ve como de otro comando: dentro de
-- la misma transacción, el stripe nuevo es de visibilidad parcial y se
-- recorre fila por fila.
BEGIN;
INSERT INTO sm VALUES (1, 1, 1, 'y');
SELECT sum(b), count(a), count(*) FROM sm;
ROLLBACK;
SELECT sum(b), count(a), count(*) FROM sm;

-- Argumentos que son una expresión sobre una columna.  col + k y col - k
-- salen del catálogo (sum(col) + k·count(col)); las demás se evalúan fila
-- por fila contra el slot.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT sum(a + 1), sum(b - 5), avg(1 + a), count(a + 7) FROM sm;
SELECT sum(a + 1), sum(b - 5), avg(1 + a), count(a + 7), sum(a::int4 + 100000) FROM sm;
SELECT sum(a + 1), sum(b - 5), avg(1 + a), count(a + 7), sum(a::int4 + 100000) FROM sm_heap;
SELECT sum(a * 2), avg(abs(b)), max(a + 1), min(length(t)) FROM sm;
SELECT sum(a * 2), avg(abs(b)), max(a + 1), min(length(t)) FROM sm_heap;
SELECT t, sum(a + 1), count(b - 1) FROM sm GROUP BY t ORDER BY t;
SELECT t, sum(a + 1), count(b - 1) FROM sm_heap GROUP BY t ORDER BY t;
SELECT sum(a + 1) FROM sm WHERE b > 100;
SELECT sum(a + 1) FROM sm_heap WHERE b > 100;
-- Overflow: b llega a 2147483647, así que b + 1 no entra en integer.  El
-- zone map lo ve y el stripe se recorre fila por fila, que da el error.
SELECT sum(b + 1) FROM sm;
SELECT sum(b + 1) FROM sm_heap;

-- Una tabla sin filas y una con un solo stripe todo nulo.
CREATE TABLE sm_vacia (a int4) USING dbengine_columnar;
SELECT sum(a), avg(a), count(a), count(*) FROM sm_vacia;
INSERT INTO sm_vacia SELECT NULL FROM generate_series(1, 10);
SELECT sum(a), avg(a), count(a), count(*) FROM sm_vacia;

DROP TABLE sm, sm_heap, sm_vacia;
RESET max_parallel_workers_per_gather;
DROP EXTENSION dbengine CASCADE;
