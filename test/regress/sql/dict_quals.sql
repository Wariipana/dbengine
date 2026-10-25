--
-- Predicados evaluados una vez por entrada del diccionario (C2).
--
-- Cada consulta se corre con dbengine.enable_dict_quals prendido y
-- apagado: tiene que dar exactamente lo mismo.  Los casos son los que
-- podrían romper el atajo: NULL (no tiene código), cláusulas que miran dos
-- columnas, funciones volátiles, Params que cambian entre rescans y varios
-- stripes con diccionarios distintos.
--

CREATE EXTENSION dbengine;

CREATE TABLE dq (id int, s text, t text) USING dbengine_columnar;

-- Tres stripes (una transacción cada uno): el mismo código significa un
-- valor distinto en cada diccionario.
INSERT INTO dq SELECT g, (ARRAY['ana','beto','carla','dario', NULL])[1 + g % 5],
                      'x' || (g % 3)
FROM generate_series(1, 3000) g;
INSERT INTO dq SELECT g, (ARRAY['zoe','yago','ana', NULL])[1 + g % 4], 'x1'
FROM generate_series(3001, 6000) g;
INSERT INTO dq SELECT g, (ARRAY['carla','ximena'])[1 + g % 2], 'x' || (g % 2)
FROM generate_series(6001, 9000) g;

-- La columna s tiene que estar con diccionario para que el test pruebe algo.
SELECT DISTINCT sc.encoding IN (3, 4) AS s_con_diccionario
FROM dbengine.stripe_column sc
JOIN dbengine.stripe st USING (relfilenode, stripe_id)
WHERE sc.relfilenode = pg_relation_filenode('dq') AND sc.attnum = 2;

CREATE FUNCTION dq_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	con text;
	sin text;
BEGIN
	SET LOCAL dbengine.enable_dict_quals = on;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO con;
	SET LOCAL dbengine.enable_dict_quals = off;
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (' || q || ') x' INTO sin;
	RETURN CASE WHEN con IS NOT DISTINCT FROM sin THEN 'igual' ELSE 'DISTINTO' END;
END $$;

-- Custom Scan.
SELECT dq_check($$SELECT id, s FROM dq WHERE s LIKE '%a%'$$);
SELECT dq_check($$SELECT id FROM dq WHERE s = 'carla' AND id % 7 = 0$$);
SELECT dq_check($$SELECT id FROM dq WHERE s IS NULL$$);
SELECT dq_check($$SELECT id FROM dq WHERE coalesce(s, 'nulo') = 'nulo'$$);
SELECT dq_check($$SELECT id FROM dq WHERE s IS DISTINCT FROM 'ana'$$);
SELECT dq_check($$SELECT id FROM dq WHERE s = t || 'x'$$);	-- dos columnas
SELECT dq_check($$SELECT id FROM dq WHERE length(s) > 3 AND t <> 'x0'$$);
SELECT dq_check($$SELECT id FROM dq WHERE s ~ '^[a-c]' OR s IS NULL$$);
-- s en una cláusula de diccionario y en otra que no lo es: ahí s no puede
-- quedar perezosa, porque la segunda la necesita cargada.
SELECT dq_check($$SELECT id, s FROM dq WHERE s LIKE '%a%' AND s || t <> 'anax1'$$);
SELECT dq_check($$SELECT t, count(*) FROM dq WHERE s LIKE '%a%' AND s || t <> 'anax1' GROUP BY t$$);

-- Pushdown de agregados.
SELECT dq_check($$SELECT count(*) FROM dq WHERE s LIKE '%a%'$$);
SELECT dq_check($$SELECT s, count(*) FROM dq WHERE s <> 'zoe' GROUP BY s$$);
SELECT dq_check($$SELECT t, min(s), count(*) FROM dq WHERE s NOT LIKE 'c%' GROUP BY t$$);

-- Volátil: no califica y se evalúa por fila.  Con setseed fijo, las dos
-- corridas ven la misma secuencia.
SELECT dq_check($$SELECT count(*) FROM (SELECT setseed(0.5)) z, dq
                  WHERE s = 'ana' AND random() < 2$$);

-- Params que cambian entre rescans: el patrón viene de afuera, fila por
-- fila.  Si la tabla de resultados no se vaciara en el rescan, el segundo
-- patrón usaría lo que calculó el primero.
SELECT dq_check($$SELECT p, (SELECT count(*) FROM dq WHERE s LIKE p)
                  FROM (VALUES ('a%'), ('%a'), ('c%'), ('%')) v(p)$$);
SELECT p, (SELECT count(*) FROM dq WHERE s LIKE p) AS n
FROM (VALUES ('a%'), ('%a'), ('c%'), ('%')) v(p) ORDER BY p;
-- El EXPLAIN de ese plan fallaba con "invalid attnum 2 for relation v":
-- los quals del pushdown no pasan por setrefs.c y dentro de una subconsulta
-- apuntaban a otra entrada del range table.
EXPLAIN (COSTS OFF)
SELECT p, (SELECT count(*) FROM dq WHERE s LIKE p) AS n
FROM (VALUES ('a%'), ('%a'), ('c%'), ('%')) v(p) ORDER BY p;

-- Y una sentencia preparada con plan genérico, dos valores distintos.
SET plan_cache_mode = force_generic_plan;
PREPARE dqp(text) AS SELECT count(*) FROM dq WHERE s = $1;
EXECUTE dqp('ana');
EXECUTE dqp('carla');
EXECUTE dqp('ximena');
DEALLOCATE dqp;
RESET plan_cache_mode;

-- Resultados concretos, contra heap.
CREATE TABLE dq_heap AS SELECT * FROM dq;
SELECT (SELECT count(*) FROM dq WHERE s LIKE '%a%') AS columnar,
       (SELECT count(*) FROM dq_heap WHERE s LIKE '%a%') AS heap;
SELECT s, count(*) FROM dq WHERE s <> 'zoe' GROUP BY s ORDER BY s;

DROP TABLE dq, dq_heap;
DROP FUNCTION dq_check(text);
DROP EXTENSION dbengine CASCADE;
