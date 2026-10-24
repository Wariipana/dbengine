-- F6: fusión de stripes.
--
-- Con autocommit fila a fila cada transacción cierra su stripe, así que N
-- filas dan N stripes.  Fusionar stripes adyacentes lo arregla, y lo hace
-- barato una propiedad del layout: un stripe es dueño de un rango contiguo
-- de números de fila, así que fusionar CONSERVANDO EL ORDEN conserva el
-- número de fila de cada fila — y el número de fila es el TID.
--
-- Lo que se prueba, en este orden de importancia: que los TID no se muevan
-- (o sea que los índices sigan valiendo sin reconstruirse), que no se
-- pierda ni se duplique ninguna fila, y que converja.
CREATE EXTENSION dbengine;

SET dbengine.enable_agg_pushdown = off;
SET max_parallel_workers_per_gather = 0;
-- El presupuesto por ciclo es dbengine.max_blocks_per_compaction: con el
-- default de 64, doscientos stripes tardan dos pasadas.
--
-- Y la ventana de retención en cero: fusionar coarsa el time travel de F7,
-- así que por defecto no toca nada más nuevo que un día.  Solo aplica si
-- track_commit_timestamp está encendido — sin él no hay time travel que
-- proteger y la fusión no espera nada.
SET dbengine.retention_window = 0;

CREATE TABLE m (a int, b text) USING dbengine_columnar;
CREATE INDEX m_a ON m USING btree (a);

-- Cada INSERT en su propia transacción: es el caso que deja un stripe por
-- fila.  Un DO con COMMIT adentro es la forma de tenerlos separados sin
-- salir de un script.
DO $$
BEGIN
	FOR i IN 1..200 LOOP
		INSERT INTO m VALUES (i, 'v' || i);
		COMMIT;
	END LOOP;
END
$$;

SELECT count(*) AS stripes_al_insertar FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('m');

-- ===========================================================================
-- Los TID no se mueven
-- ===========================================================================
CREATE TEMP TABLE tids_antes AS
SELECT a, ctid::text AS tid FROM m;

-- La fusión solo toca stripes anteriores al xmin global del cluster: otra
-- sesión con una transacción abierta (en cualquier base) la frena, y el test
-- fallaría sin que haya nada roto.  Se espera a que se vayan las pasajeras
-- (autovacuum, por ejemplo) y, si alguna sigue, el diff lo dice acá en vez
-- de mostrar una fusión que "no hizo nada".
DO $$
BEGIN
	FOR i IN 1..600 LOOP
		PERFORM pg_stat_clear_snapshot();
		EXIT WHEN NOT EXISTS (
			SELECT 1 FROM pg_stat_activity
			WHERE pid <> pg_backend_pid()
			  AND (backend_xmin IS NOT NULL OR backend_xid IS NOT NULL))
		  AND NOT EXISTS (SELECT 1 FROM pg_prepared_xacts)
		  AND NOT EXISTS (SELECT 1 FROM pg_replication_slots
						  WHERE xmin IS NOT NULL OR catalog_xmin IS NOT NULL);
		PERFORM pg_sleep(0.1);
	END LOOP;
END
$$;
SELECT NOT EXISTS (
	SELECT 1 FROM pg_stat_activity
	WHERE pid <> pg_backend_pid()
	  AND (backend_xmin IS NOT NULL OR backend_xid IS NOT NULL))
   AND NOT EXISTS (SELECT 1 FROM pg_prepared_xacts) AS cluster_quieto;

SELECT dbengine.merge_stripes('m') > 0 AS fusiono_algo;

SELECT count(*) AS tids_distintos
FROM m JOIN tids_antes USING (a)
WHERE m.ctid::text <> tids_antes.tid;

-- ===========================================================================
-- Ni una fila de más ni de menos
-- ===========================================================================
SELECT count(*) AS filas, sum(a) AS suma, min(a) AS minimo, max(a) AS maximo
FROM m;

-- ===========================================================================
-- El índice sigue sirviendo, sin reconstruirlo
-- ===========================================================================
SET enable_seqscan = off;
SET dbengine.enable_vectorized = off;
EXPLAIN (COSTS OFF) SELECT b FROM m WHERE a = 137;
SELECT b FROM m WHERE a = 137;
SELECT count(*) AS por_indice FROM m WHERE a BETWEEN 50 AND 100;
-- Todas las filas se encuentran por el índice.
SELECT count(*) AS todas_por_indice FROM m i
WHERE EXISTS (SELECT 1 FROM m j WHERE j.a = i.a);
RESET enable_seqscan;
RESET dbengine.enable_vectorized;

-- ===========================================================================
-- Converge, y después no hace nada
-- ===========================================================================
SELECT dbengine.merge_stripes('m') AS pasada_2;
SELECT dbengine.merge_stripes('m') AS pasada_3;
SELECT dbengine.merge_stripes('m') AS pasada_4;
SELECT count(*) AS stripes_al_final FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('m');
SELECT count(*) AS filas_al_final, sum(a) AS suma_al_final FROM m;

-- ===========================================================================
-- Un stripe grande no se toca, y un hueco corta la corrida
-- ===========================================================================
-- Este INSERT deja un solo stripe grande.
CREATE TABLE g (a int) USING dbengine_columnar;
INSERT INTO g SELECT generate_series(1, 50000);
SELECT dbengine.merge_stripes('g') AS grande_no_se_toca;

-- ===========================================================================
-- Lo que no es columnar da error claro
-- ===========================================================================
CREATE TABLE h (a int);
\set ON_ERROR_STOP off
SELECT dbengine.merge_stripes('h');
\set ON_ERROR_STOP on

-- El espacio se recupera con VACUUM FULL, no con la fusión.
VACUUM FULL m;
SELECT count(*) AS filas_tras_vacuum_full, sum(a) AS suma FROM m;

DROP TABLE m, g, h;
DROP EXTENSION dbengine CASCADE;
