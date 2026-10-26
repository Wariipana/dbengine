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

-- Expresiones de la salida: el Custom Scan proyecta por su cuenta y calcula
-- cada una una vez por entrada del diccionario.
EXPLAIN (COSTS OFF) SELECT upper(s), id FROM dq WHERE id > 0;
SELECT dq_check($$SELECT upper(s), length(s), id FROM dq$$);
SELECT dq_check($$SELECT regexp_replace(s, '^(.)', '[\1]') AS k, t, id FROM dq WHERE id % 3 = 0$$);
SELECT dq_check($$SELECT coalesce(s, 'nulo') || '!' AS k, s IS NULL AS n FROM dq$$);
SELECT dq_check($$SELECT nullif(s, 'ana') AS k, s || t AS dos FROM dq WHERE s LIKE '%a%'$$);
SELECT dq_check($$SELECT k, count(*) FROM (SELECT upper(s) AS k FROM dq) x GROUP BY k$$);
-- Resultado concreto: el mismo valor en distintos stripes, con códigos distintos.
SELECT upper(s) AS k, count(*) FROM dq GROUP BY 1 ORDER BY 1;
-- Correlacionada: la expresión depende de un Param que cambia por fila de
-- afuera, y la tabla se tiene que vaciar en cada rescan.
SELECT dq_check($$SELECT p, (SELECT string_agg(DISTINCT s || p, ',') FROM dq WHERE id < 20)
                  FROM (VALUES ('1'), ('2')) v(p)$$);

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

-- LIKE comparado sobre los bytes (dbe_like_compile): cada patrón como
-- constante, contra heap.  Los que llevan `_` o escape no califican y se
-- evalúan con textlike(); tienen que dar lo mismo igual.  El primer stripe
-- tiene diccionario y el segundo no (todos los valores distintos), que es
-- el otro camino de la evaluación.
CREATE TABLE lk (s text, v varchar(40)) USING dbengine_columnar;
INSERT INTO lk SELECT x, x FROM (SELECT (ARRAY['', 'a', 'ab', 'aba', 'abba',
	'ababa', 'b', 'bab', 'xaby', 'año', 'ñaño', 'a%b', 'a_b', 'a\b', '日本語',
	'𝄞ab𝄞', NULL])[1 + g % 17] AS x FROM generate_series(1, 3400) g) z;
INSERT INTO lk SELECT 'ab' || g || CASE WHEN g % 2 = 0 THEN 'ba' ELSE 'ñ' END,
	'q' || g FROM generate_series(1, 2000) g;
CREATE TABLE lk_heap AS SELECT * FROM lk;
SELECT DISTINCT sc.encoding IN (3, 4) AS con_diccionario
FROM dbengine.stripe_column sc
WHERE sc.relfilenode = pg_relation_filenode('lk') AND sc.attnum = 1
ORDER BY 1;

CREATE FUNCTION lk_check(OUT pat text, OUT op text, OUT igual bool)
RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	c bigint;
	h bigint;
BEGIN
	FOREACH pat IN ARRAY ARRAY['', '%', '%%', 'a', 'a%', '%a', '%a%', 'ab%',
		'%ba', 'a%a', 'ab%ba', 'aba%aba', '%b%b%', '%ab%ba%', 'a%%b', '%ñ%',
		'%𝄞', '日%語', '%1%9%', 'ab1%', '_b%', '%a_b%', 'a\%b', 'a\_b', 'a\\b',
		'%\\%']
	LOOP
		FOREACH op IN ARRAY ARRAY['s LIKE', 's NOT LIKE', 'v LIKE',
			's COLLATE "C" LIKE']
		LOOP
			EXECUTE format('SELECT count(*) FROM lk WHERE %s %L', op, pat) INTO c;
			EXECUTE format('SELECT count(*) FROM lk_heap WHERE %s %L', op, pat) INTO h;
			igual := (c = h);
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;
SELECT count(*) AS casos, bool_and(igual) AS todos_iguales FROM lk_check();
SELECT * FROM lk_check() WHERE NOT igual;

-- ESCAPE explícito: el planner lo pliega a un patrón con `\`.
SELECT (SELECT count(*) FROM lk WHERE s LIKE 'a#%b' ESCAPE '#')
	 = (SELECT count(*) FROM lk_heap WHERE s LIKE 'a#%b' ESCAPE '#') AS escape_igual;
-- Con el pushdown de agregados y con el Custom Scan proyectando filas.
SELECT (SELECT count(*) FROM lk WHERE s LIKE '%ab%' AND s NOT LIKE '%ba')
	 = (SELECT count(*) FROM lk_heap WHERE s LIKE '%ab%' AND s NOT LIKE '%ba')
	AS dos_like_igual;
SELECT (SELECT string_agg(s, ',' ORDER BY s) FROM lk WHERE s LIKE 'ab1%9%')
	 = (SELECT string_agg(s, ',' ORDER BY s) FROM lk_heap WHERE s LIKE 'ab1%9%')
	AS filas_igual;

DROP FUNCTION lk_check();
DROP TABLE lk, lk_heap;

-- `%aguja%` sobre un diccionario FSST se busca en los códigos comprimidos
-- (SegmentReader::dict_contains).  Agujas que cruzan símbolos, que se
-- solapan consigo mismas (`gogoogle` tiene que encontrar `google` después
-- de un falso comienzo), multibyte, de un byte y más largas que lo que
-- acepta el autómata.  La última empieza con el byte de cabecera varlena de
-- las filas de ese largo: la cabecera no es parte del texto y no puede
-- calzar.
CREATE TABLE fz (s text) USING dbengine_columnar;
INSERT INTO fz SELECT 'http://www.example' || (g % 400) || '.com/path/' ||
	CASE g % 7 WHEN 0 THEN 'google' WHEN 1 THEN 'goog' WHEN 2 THEN 'gogoogle'
			   WHEN 3 THEN 'añoñgoogle日本' WHEN 4 THEN NULL ELSE 'xgooglx' END
	|| '/index.html?q=' || (g % 400)
	FROM generate_series(1, 4000) g;
CREATE TABLE fz_heap AS SELECT * FROM fz;
SELECT DISTINCT sc.encoding = 4 AS fsst
FROM dbengine.stripe_column sc
WHERE sc.relfilenode = pg_relation_filenode('fz');

CREATE FUNCTION fz_check(OUT pat text, OUT igual bool)
RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	c bigint;
	h bigint;
	hdr text;
BEGIN
	SELECT chr((octet_length(s) + 1) * 2 + 1) INTO hdr
	FROM fz_heap WHERE octet_length(s) < 62 LIMIT 1;
	FOREACH pat IN ARRAY ARRAY['%google%', '%goog%', '%gogoogle%', '%oogl%',
		'%ñgoogle日%', '%日本/%', '%h%', '%q=1%', '%example12.com/path/g%',
		'%.com/path/xgooglx/index.html?q=3%', '%zz%', '%' || repeat('x', 300) || '%',
		'%' || hdr || 'http%', '%' || hdr || '%']
	LOOP
		EXECUTE format('SELECT count(*) FROM fz WHERE s LIKE %L', pat) INTO c;
		EXECUTE format('SELECT count(*) FROM fz_heap WHERE s LIKE %L', pat) INTO h;
		igual := (c = h);
		RETURN NEXT;
		EXECUTE format('SELECT count(*) FROM fz WHERE s NOT LIKE %L', pat) INTO c;
		EXECUTE format('SELECT count(*) FROM fz_heap WHERE s NOT LIKE %L', pat) INTO h;
		igual := (c = h);
		RETURN NEXT;
	END LOOP;
END $$;
SELECT count(*) AS casos, bool_and(igual) AS todos_iguales FROM fz_check();
SELECT * FROM fz_check() WHERE NOT igual;
SELECT count(*) AS con_google FROM fz WHERE s LIKE '%google%';

DROP FUNCTION fz_check();
DROP TABLE fz, fz_heap;

DROP TABLE dq, dq_heap;
DROP FUNCTION dq_check(text);
DROP EXTENSION dbengine CASCADE;
