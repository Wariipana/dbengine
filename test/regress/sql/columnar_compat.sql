--
-- Semántica de PostgreSQL que el AM columnar tiene que respetar.
--
-- Todo lo de este archivo lo encontró el gate de F1 (tools/f1-gate.sh): la
-- suite de PostgreSQL daba resultados distintos sin ningún error de por
-- medio.  Cada sección compara contra lo que devuelve heap.
--

CREATE EXTENSION dbengine;
SET default_table_access_method = dbengine_columnar;

-- ------------------------------------------------------------------------
-- Varlena con header de 4 bytes, desalineados dentro del segmento.
--
-- Los valores van pegados sin padding.  range_deserialize() alinea los
-- límites contra la dirección absoluta, así que un rango que caía en una
-- dirección impar se leía corrido: [2,12) salía como [2,4456960).  Solo la
-- primera fila, en el offset 0, salía bien.  Los valores que llegan de una
-- función (no de una tabla) traen header de 4 bytes.
-- ------------------------------------------------------------------------
CREATE TABLE al (r4 int4range, r8 int8range, a8 int8[], n numeric);
INSERT INTO al SELECT int4range(g, g + 10), int8range(g, g * 1000),
                      ARRAY[g, g * 2]::int8[], g / 7.0
FROM generate_series(1, 6) g;
-- Y un valor que no entra en un header corto.
INSERT INTO al VALUES (NULL, NULL, array_fill(7::int8, ARRAY[40]), NULL);

SELECT r4, r8, a8[1:2] AS a8, round(n, 3) AS n FROM al ORDER BY lower(r4);
SELECT count(*) AS "igual_a_[2,12)" FROM al WHERE r4 = int4range(2, 12);
SELECT count(*) AS contiene_25 FROM al WHERE r8 @> 25::int8;
SELECT sum(a8[2]) AS suma_a8 FROM al;
SELECT array_length(a8, 1) AS largo FROM al WHERE r4 IS NULL;

-- ------------------------------------------------------------------------
-- Agregados polimórficos dentro de DbengineVectorAgg.
--
-- La función de transición no recibía fn_expr y no podía preguntar el tipo
-- de su argumento: "range_agg must be called with a range".
-- ------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT range_agg(r4) FROM al;
SELECT range_agg(r4), range_intersect_agg(r4 * int4range(1, 100)) FROM al;
SELECT range_agg(r4) FROM al WHERE lower(r4) > 3;

-- ------------------------------------------------------------------------
-- Herencia: el Custom Scan sobre el padre leía solo el storage del padre.
-- ------------------------------------------------------------------------
CREATE TABLE padre (a int);
CREATE TABLE hija (b int) INHERITS (padre);
INSERT INTO padre VALUES (1);
INSERT INTO hija VALUES (2, 20);

SELECT a FROM padre ORDER BY a;
SELECT count(*), sum(a) FROM padre;
SELECT a FROM ONLY padre;

-- ------------------------------------------------------------------------
-- tableoid: el Custom Scan no lo ponía en el slot y salía 0.
-- ------------------------------------------------------------------------
CREATE TABLE pp (a int) PARTITION BY LIST (a);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES IN (1);
CREATE TABLE pp2 PARTITION OF pp FOR VALUES IN (2);
INSERT INTO pp VALUES (1), (2), (2);

SELECT tableoid::regclass AS particion, count(*) FROM pp GROUP BY 1 ORDER BY 1;
SELECT tableoid::regclass AS particion, a FROM pp1;
SELECT tableoid::regclass AS tabla, a FROM padre ORDER BY a;

-- ------------------------------------------------------------------------
-- TABLESAMPLE: el Custom Scan se lo quedaba y devolvía la tabla entera.  Lo
-- correcto, mientras no esté implementado, es el error documentado.
-- ------------------------------------------------------------------------
CREATE TABLE ts (id int);
INSERT INTO ts SELECT generate_series(1, 100);
\set VERBOSITY terse
SELECT count(*) FROM ts TABLESAMPLE SYSTEM (0);
SELECT count(*) FROM ts TABLESAMPLE BERNOULLI (50) REPEATABLE (0);
\set VERBOSITY default

-- ------------------------------------------------------------------------
-- ON COMMIT DELETE ROWS.
--
-- El truncate corre en el pre-commit, en el mismo comando en que el flush
-- del buffer inserta el stripe.  El borrado del catálogo no veía esa fila,
-- el storage quedaba en cero y la transacción siguiente fallaba con
-- "lectura fuera del storage".
-- ------------------------------------------------------------------------
CREATE TEMP TABLE oc (f1 int, t text) ON COMMIT DELETE ROWS;
INSERT INTO oc VALUES (1, repeat('x', 5000)), (2, 'y');
CREATE INDEX oc_i ON oc (f1);
SELECT count(*) AS vacia FROM oc;

BEGIN;
INSERT INTO oc VALUES (3, 'z');
SELECT count(*) AS dentro FROM oc;
COMMIT;
SELECT count(*) AS vacia_otra_vez FROM oc;

SELECT count(*) AS stripes_huerfanos FROM dbengine.stripe s
WHERE NOT EXISTS (SELECT 1 FROM pg_class c
                  WHERE pg_relation_filenode(c.oid) = s.relfilenode);

DROP TABLE al, padre, hija, pp, ts, oc;
DROP EXTENSION dbengine CASCADE;
