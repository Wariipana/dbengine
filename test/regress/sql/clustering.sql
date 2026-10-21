-- F6: clustering adaptativo.
--
-- La decisión de por cuál columna reordenar sale del canal de feedback —lo
-- que el planner vio— y la reescritura la hace CLUSTER, que en un storage
-- columnar además rearma stripes, zone maps, filtros y compresión.
--
-- Lo que se prueba: que el consejo refleje el workload, que reordenar
-- mejore el descarte de stripes, y que **no cambie ninguna fila**.
CREATE EXTENSION dbengine;

SET dbengine.enable_agg_pushdown = off;
SET max_parallel_workers_per_gather = 0;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
-- Varios stripes: con uno solo no hay nada que reordenar.
SET dbengine.staging_threshold_rows = 5000;

CREATE TABLE t (ts bigint, uid bigint, val int) USING dbengine_columnar;
-- `ts` sigue el orden de inserción; `uid` no.
INSERT INTO t SELECT g, (g::bigint * 7919) % 20000, g % 100
	FROM generate_series(1, 40000) g;
CREATE INDEX t_uid ON t USING btree (uid);
CREATE INDEX t_ts ON t USING btree (ts);
ANALYZE t;

SELECT count(*) AS stripes FROM dbengine.stripe
WHERE relfilenode = pg_relation_filenode('t');

-- ===========================================================================
-- El consejo sale del workload
-- ===========================================================================
SELECT dbengine.workload_reset();
SELECT dbengine.preloaded() AS precargado \gset

SELECT count(*) FROM t WHERE uid = 7919;
SELECT count(*) FROM t WHERE uid = 15838;
SELECT count(*) FROM t WHERE ts > 20000;

\if :precargado
-- La igualdad pesa más que el rango, así que gana uid.
SELECT columna, puntaje, se_puede_clusterizar
FROM dbengine.cluster_advice()
WHERE tabla = 't'::regclass
ORDER BY puntaje DESC;
\else
\echo 'sin shared_preload_libraries: el canal de feedback está apagado'
\endif

-- ===========================================================================
-- Reordenar mejora el descarte, y no cambia ninguna fila
-- ===========================================================================
-- Huella de la tabla entera: tiene que ser la misma antes y después.
SELECT md5(string_agg(x::text, '|' ORDER BY x)) AS huella_antes
FROM (SELECT ts, uid, val FROM t) x \gset

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM t WHERE uid = 7919;

CLUSTER t USING t_uid;
ANALYZE t;

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM t WHERE uid = 7919;

SELECT md5(string_agg(x::text, '|' ORDER BY x)) = :'huella_antes' AS filas_intactas
FROM (SELECT ts, uid, val FROM t) x;

-- Después de CLUSTER el consejo tiene que decir por cuál columna está
-- ordenada la tabla AHORA.  Sin ese dato solo muestra lo que se gana y
-- nunca lo que se pierde: medido sobre ClickBench, reordenar por UserID da
-- 455x en un GROUP BY con LIMIT y al mismo tiempo hunde a 0,17x la query
-- que filtraba por rango de fecha, que venía gratis con el orden anterior.
\if :precargado
SELECT columna, es_orden_actual
FROM dbengine.cluster_advice()
WHERE tabla = 't'::regclass
ORDER BY columna;
\endif

-- Una tabla se ordena por UNA cosa: reordenar por uid le sacó a ts la
-- correlación que tenía.  Es la razón por la que el filtro de Bloom de F5
-- no es redundante con el clustering.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM t WHERE ts BETWEEN 20000 AND 20100;

-- ===========================================================================
-- Los índices siguen sirviendo después de la reescritura
-- ===========================================================================
-- CLUSTER los reconstruye: los TID cambiaron todos.
RESET enable_indexscan;
RESET enable_bitmapscan;
SET enable_seqscan = off;
SELECT count(*) AS por_indice FROM t WHERE uid = 7919;
SELECT count(*) AS por_indice_ts FROM t WHERE ts = 12345;
RESET enable_seqscan;

-- El índice PGM no puede definir el orden: no ordena, así que declara
-- amclusterable = false y Postgres lo rechaza antes de llegar al AM.
CREATE INDEX t_pgm ON t USING dbengine_pgm (uid);
\set ON_ERROR_STOP off
CLUSTER t USING t_pgm;
\set ON_ERROR_STOP on
DROP INDEX t_pgm;

-- ===========================================================================
-- VACUUM FULL: la misma reescritura, sin ordenar
-- ===========================================================================
VACUUM FULL t;
SELECT md5(string_agg(x::text, '|' ORDER BY x)) = :'huella_antes' AS intactas_tras_vacuum_full
FROM (SELECT ts, uid, val FROM t) x;

DROP TABLE t;
DROP EXTENSION dbengine CASCADE;
