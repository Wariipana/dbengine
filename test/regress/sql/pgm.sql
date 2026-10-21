-- F4: Index AM `dbengine_pgm`, el índice aprendido.
--
-- El gate de la fase es una property: con los MISMOS datos, un índice PGM y
-- un btree tienen que devolver exactamente lo mismo para cualquier query.
-- Todo lo demás —tamaño, velocidad— es negociable; esto no.
CREATE EXTENSION dbengine;

SET enable_seqscan = off;
SET enable_bitmapscan = off;			-- fuerza amgettuple; el bitmap se prueba aparte
SET dbengine.enable_vectorized = off;	-- que compita el índice, no el scan

-- ===========================================================================
-- Registro
-- ===========================================================================
SELECT amname, amtype FROM pg_am WHERE amname = 'dbengine_pgm';

-- ===========================================================================
-- Property: PGM contra btree
-- ===========================================================================
CREATE TABLE p (k bigint, v text);
CREATE TABLE b (k bigint, v text);

CREATE FUNCTION cargar(dist text) RETURNS void AS $$
BEGIN
	TRUNCATE p, b;
	IF dist = 'secuencial' THEN
		INSERT INTO p SELECT g, 'v' || g FROM generate_series(1, 20000) g;
	ELSIF dist = 'duplicados' THEN
		INSERT INTO p SELECT (g / 500)::bigint, 'v' || g FROM generate_series(1, 20000) g;
	ELSIF dist = 'negativos' THEN
		INSERT INTO p SELECT (g - 10000)::bigint, 'v' || g FROM generate_series(1, 20000) g;
	ELSIF dist = 'clusters' THEN
		INSERT INTO p SELECT (x * 1000000000::bigint + y), 'v'
			FROM generate_series(1, 20) x, generate_series(1, 1000) y;
	ELSIF dist = 'extremos' THEN
		INSERT INTO p VALUES (-9223372036854775808, 'min'), (0, 'cero'),
							 (9223372036854775807, 'max');
	END IF;
	INSERT INTO b SELECT * FROM p;
END
$$ LANGUAGE plpgsql;

CREATE INDEX ip ON p USING dbengine_pgm (k);
CREATE INDEX ib ON b USING btree (k);

-- Compara el resultado de las dos tablas para un predicado.
CREATE FUNCTION comparar(pred text) RETURNS text AS $$
DECLARE
	a text;
	c text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(x::text, ''|'' ORDER BY x), ''-''))
			 FROM (SELECT k, v FROM p WHERE ' || pred || ') x' INTO a;
	EXECUTE 'SELECT md5(coalesce(string_agg(x::text, ''|'' ORDER BY x), ''-''))
			 FROM (SELECT k, v FROM b WHERE ' || pred || ') x' INTO c;
	IF a = c THEN
		RETURN 'igual';
	END IF;
	RETURN 'DIFIERE en: ' || pred;
END
$$ LANGUAGE plpgsql;

-- Recorre varias distribuciones y varias formas de predicado.  Los bordes
-- —el mínimo, el máximo, uno afuera de cada lado— son los que rompen un
-- índice aprendido si la ventana del modelo está mal.
DO $$
DECLARE
	d text;
	pred text;
	res text;
	kk bigint;
	preds text[];
	malos int := 0;
	total int := 0;
BEGIN
	FOREACH d IN ARRAY ARRAY['secuencial', 'duplicados', 'negativos',
							 'clusters', 'extremos'] LOOP
		PERFORM cargar(d);
		REINDEX INDEX ip;
		REINDEX INDEX ib;
		ANALYZE p;
		ANALYZE b;

		preds := ARRAY[]::text[];
		FOR kk IN SELECT unnest(ARRAY[(SELECT min(x.k) FROM p x),
									  (SELECT max(x.k) FROM p x),
									  (SELECT min(x.k) + 1 FROM p x),
									  (SELECT max(x.k) - 1 FROM p x), 0]) LOOP
			-- El literal se escribe entre comillas: -9223372036854775808 sin
			-- ellas se parsea como negar 9223372036854775808, que no entra.
			preds := preds || ARRAY[
				'k = '  || quote_literal(kk) || '::bigint',
				'k < '  || quote_literal(kk) || '::bigint',
				'k <= ' || quote_literal(kk) || '::bigint',
				'k > '  || quote_literal(kk) || '::bigint',
				'k >= ' || quote_literal(kk) || '::bigint'];
		END LOOP;
		FOR kk IN SELECT x.k FROM p x WHERE x.k < 9223372036854775000
								   ORDER BY random() LIMIT 15 LOOP
			preds := preds || ARRAY[
				'k = ' || quote_literal(kk) || '::bigint',
				'k >= ' || quote_literal(kk) || '::bigint AND k <= ' ||
					quote_literal(kk + 137) || '::bigint'];
		END LOOP;

		FOREACH pred IN ARRAY preds LOOP
			total := total + 1;
			res := comparar(pred);
			IF res <> 'igual' THEN
				malos := malos + 1;
				RAISE NOTICE '% [%]', res, d;
			END IF;
		END LOOP;
	END LOOP;

	RAISE NOTICE 'predicados comparados: %, distintos: %', total, malos;
END
$$;

-- ===========================================================================
-- El buffer delta: lo insertado DESPUÉS del build
-- ===========================================================================
SELECT cargar('secuencial');
REINDEX INDEX ip;
REINDEX INDEX ib;
ANALYZE p;
ANALYZE b;

INSERT INTO p SELECT g, 'nuevo' || g FROM generate_series(30000, 30500) g;
INSERT INTO b SELECT g, 'nuevo' || g FROM generate_series(30000, 30500) g;
INSERT INTO p VALUES (-5, 'negativo'), (10000, 'duplica una del build');
INSERT INTO b VALUES (-5, 'negativo'), (10000, 'duplica una del build');

SELECT comparar('k = 30250') AS igualdad_en_delta,
	   comparar('k BETWEEN 29990 AND 30010') AS rango_que_cruza,
	   comparar('k = 10000') AS clave_en_build_y_en_delta,
	   comparar('k = -5') AS solo_en_delta,
	   comparar('k > 29000') AS rango_grande,
	   comparar('k < 0') AS menores;

-- REINDEX absorbe el delta.
REINDEX INDEX ip;
SELECT comparar('k = 30250') AS despues_de_reindex,
	   comparar('k = -5') AS negativo_despues_de_reindex;

-- El mismo índice por el camino de bitmap (amgetbitmap en vez de amgettuple).
SET enable_bitmapscan = on;
SET enable_indexscan = off;
SELECT comparar('k = 30250') AS bitmap_igualdad,
	   comparar('k BETWEEN 100 AND 200') AS bitmap_rango,
	   comparar('k = -5') AS bitmap_delta;
EXPLAIN (COSTS OFF) SELECT k FROM p WHERE k = 12345;
RESET enable_indexscan;
SET enable_bitmapscan = off;

-- ===========================================================================
-- Planes
-- ===========================================================================
EXPLAIN (COSTS OFF) SELECT k FROM p WHERE k = 12345;
EXPLAIN (COSTS OFF) SELECT k FROM p WHERE k BETWEEN 100 AND 200;
-- El literal es integer y la columna bigint: sin los operadores cruzados en
-- la familia, el planner no reconocería la cláusula.
EXPLAIN (COSTS OFF) SELECT k FROM p WHERE k = 12345::int4;

-- ===========================================================================
-- Lo que el índice NO hace
-- ===========================================================================
CREATE TABLE malo (t text, n numeric);
\set ON_ERROR_STOP off
CREATE INDEX ON malo USING dbengine_pgm (t);		-- tipo no mapeable
CREATE INDEX ON malo USING dbengine_pgm (n);		-- tampoco
CREATE UNIQUE INDEX ON p USING dbengine_pgm (k);	-- no impone unicidad
\set ON_ERROR_STOP on

-- Los NULL no se indexan, así que IS NULL no usa el índice y el resultado
-- sigue siendo correcto por el filtro.
INSERT INTO p VALUES (NULL, 'nulo');
INSERT INTO b VALUES (NULL, 'nulo');
SELECT comparar('k IS NULL') AS is_null;
SELECT count(*) FROM p WHERE k IS NULL;

DROP FUNCTION comparar(text);
DROP FUNCTION cargar(text);
DROP TABLE p, b, malo;
DROP EXTENSION dbengine CASCADE;
