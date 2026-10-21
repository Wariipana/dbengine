-- F7: time travel.
--
-- El plan pedía un "version store como estructura lateral".  En un storage
-- append-only no hace falta: nada se borra ni se pisa, así que el estado de
-- la tabla en un momento T es exactamente el conjunto de stripes que
-- commitearon antes de T.  El momento lo lleva Postgres en pg_commit_ts.
--
-- La granularidad es el stripe, que es la transacción.  Es la correcta: una
-- transacción commitea entera.
CREATE EXTENSION dbengine;

SELECT current_setting('track_commit_timestamp')::bool AS con_commit_ts \gset

CREATE TABLE t (id int, val text) USING dbengine_columnar;

-- Tres transacciones separadas, con un instante anotado entre cada una.
INSERT INTO t VALUES (1, 'primera');
SELECT pg_sleep(0.15);
SELECT clock_timestamp() AS t1 \gset
INSERT INTO t VALUES (2, 'segunda');
SELECT pg_sleep(0.15);
SELECT clock_timestamp() AS t2 \gset
INSERT INTO t VALUES (3, 'tercera');

SELECT count(*) AS stripes FROM dbengine.stripe_timeline('t');

\if :con_commit_ts

-- ===========================================================================
-- El estado en cada momento
-- ===========================================================================
SELECT count(*) AS ubicables_en_el_tiempo
FROM dbengine.stripe_timeline('t') WHERE commit_ts IS NOT NULL;

SELECT * FROM dbengine.time_travel('t', :'t1'::timestamptz)
	AS (id int, val text) ORDER BY id;
SELECT * FROM dbengine.time_travel('t', :'t2'::timestamptz)
	AS (id int, val text) ORDER BY id;
SELECT * FROM dbengine.time_travel('t', now())
	AS (id int, val text) ORDER BY id;

-- Antes de que existiera nada.
SELECT count(*) AS antes_de_todo
FROM dbengine.time_travel('t', '2000-01-01'::timestamptz) AS (id int, val text);

-- El presente por time travel es el presente a secas.
SELECT (SELECT count(*) FROM dbengine.time_travel('t', now()) AS (id int, val text))
	 = (SELECT count(*) FROM t) AS coincide_con_el_presente;

-- ===========================================================================
-- La ventana de retención protege la granularidad
-- ===========================================================================
-- Fusionar coarsa el time travel: el stripe resultante se queda con el
-- momento del más nuevo.  Por eso la fusión no toca nada dentro de la
-- ventana.
SELECT dbengine.merge_stripes('t') AS no_fusiona_lo_reciente;

-- Con la ventana en cero sí fusiona, y ahí se ve el costo: los tres
-- momentos pasan a ser uno.
SET dbengine.retention_window = 0;
SELECT dbengine.merge_stripes('t') > 0 AS fusiono;
SELECT count(*) AS stripes_tras_fusion FROM dbengine.stripe_timeline('t');
-- El stripe fusionado sigue ubicable: hereda el momento del más nuevo.
SELECT count(*) AS sigue_ubicable
FROM dbengine.stripe_timeline('t') WHERE commit_ts IS NOT NULL;
-- Y las filas siguen todas.
SELECT count(*) AS filas, sum(id) AS suma FROM t;
-- El presente sigue siendo exacto; lo que se perdió es el pasado fino.
SELECT count(*) AS presente_tras_fusion
FROM dbengine.time_travel('t', now()) AS (id int, val text);
RESET dbengine.retention_window;

\else
\echo 'sin track_commit_timestamp: el time travel no puede ubicar los stripes'
\set ON_ERROR_STOP off
SELECT count(*) FROM dbengine.time_travel('t', now()) AS (id int, val text);
\set ON_ERROR_STOP on
\endif

-- ===========================================================================
-- Errores claros
-- ===========================================================================
CREATE TABLE h (id int);
\set ON_ERROR_STOP off
-- No es columnar.
SELECT * FROM dbengine.time_travel('h', now()) AS (id int);
-- La lista de columnas no coincide con la tabla.
SELECT * FROM dbengine.time_travel('t', now()) AS (id int);
\set ON_ERROR_STOP on

DROP TABLE t, h;
DROP EXTENSION dbengine CASCADE;
