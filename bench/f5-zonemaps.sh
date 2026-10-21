#!/usr/bin/env bash
# F5 — ¿cuánto lugar dejan los zone maps min/max?
#
# La pregunta que decide la fase no es cuánto podan los min/max sino cuántos
# stripes se leen PARA NADA: los que el min/max no puede descartar y que
# igual no tienen ninguna fila que cumpla.  Ese es exactamente el desperdicio
# que un filtro más fino por bloque podría sacar, y nada más.
#
#   bench/f5-zonemaps.sh [filas]
set -euo pipefail

ROWS="${1:-2000000}"
PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

Q -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null
Q -c "DROP TABLE IF EXISTS z;" >/dev/null

# Tres relaciones distintas con el orden de inserción:
#   ord    correlacionada        -> el min/max poda casi todo
#   grupo  baja cardinalidad     -> no poda nada, pero las filas están de
#                                   verdad en cada stripe
#   uid    alta cardinalidad,
#          sin correlación       -> no poda nada Y casi ningún stripe sirve
Q -c "CREATE TABLE z (ord bigint, grupo int, uid bigint, pad text)
      USING dbengine_columnar;" >/dev/null
# `z` va SIN filtros: es el baseline, lo que dejan los min/max solos.
Q -c "SET dbengine.bloom_bits_per_row = 0;
      INSERT INTO z SELECT g, g % 50, (random() * 10000000)::bigint,
                           repeat('x', 16)
      FROM generate_series(1, $ROWS) g;" >/dev/null
Q -c "ANALYZE z;" >/dev/null

$PSQL -d "$DB" -q <<'SQL'
SET dbengine.enable_agg_pushdown = off;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION analizar(pred text) RETURNS text AS $$
DECLARE
	total int; utiles int; descartados int := 0; plan text; filas bigint;
BEGIN
	SELECT count(*) INTO total FROM dbengine.stripe
	 WHERE relfilenode = pg_relation_filenode('z');
	EXECUTE 'SELECT count(*) FROM z WHERE ' || pred INTO filas;
	FOR plan IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
						 SELECT count(*) FROM z WHERE ' || pred LOOP
		IF plan LIKE '%Stripes descartados%' THEN
			descartados := substring(plan from '[0-9]+$')::int;
		END IF;
	END LOOP;
	EXECUTE 'SELECT count(DISTINCT s.stripe_id) FROM z, dbengine.stripe s
			 WHERE ' || pred || '
			   AND s.relfilenode = pg_relation_filenode(''z'')
			   AND ((z.ctid::text::point)[0]::bigint * 2047
					+ (z.ctid::text::point)[1]::bigint - 1)
				   BETWEEN s.first_row_number
					   AND s.first_row_number + s.row_count - 1' INTO utiles;
	RETURN format('filas=%-7s descartados=%-3s leídos=%-3s útiles=%-3s para-nada=%s',
				  filas, descartados, total - descartados, utiles,
				  total - descartados - utiles);
END $$ LANGUAGE plpgsql;

\echo 'ord — correlacionada con el orden de inserción'
SELECT analizar('ord = 1000000');
SELECT analizar('ord BETWEEN 1000000 AND 1000100');
\echo 'grupo — baja cardinalidad, presente en todos los stripes'
SELECT analizar('grupo = 7');
\echo 'uid — alta cardinalidad, sin correlación'
SELECT analizar('uid = ' || (SELECT uid FROM z LIMIT 1));
SELECT analizar('uid BETWEEN 500000 AND 500100');
SQL

# --- A/B: la MISMA tabla con filtros ---------------------------------------
# El filtro se fuerza acá; el default lo construye solo donde el canal de
# feedback vio predicados de igualdad.
Q -c "DROP TABLE IF EXISTS zb;" >/dev/null
Q -c "SET dbengine.bloom_bits_per_row = 8;
      CREATE TABLE zb (ord bigint, grupo int, uid bigint, pad text)
        USING dbengine_columnar;
      INSERT INTO zb SELECT * FROM z;" >/dev/null
Q -c "ANALYZE zb;" >/dev/null

# --- A/B: con y sin el filtro de Bloom -------------------------------------
$PSQL -d "$DB" -q <<'SQL'
SET dbengine.enable_agg_pushdown = off;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION medir_ab(pruning bool) RETURNS text AS $$
DECLARE
	k bigint; t0 timestamptz; ms bigint; plan text;
	buffers bigint := 0; descartados bigint := 0; n int := 0;
BEGIN
	t0 := clock_timestamp();
	FOR k IN SELECT uid FROM zb ORDER BY random() LIMIT 50 LOOP
		IF pruning THEN SET LOCAL dbengine.enable_zonemap_pruning = on;
		ELSE SET LOCAL dbengine.enable_zonemap_pruning = off; END IF;
		FOR plan IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF,
									  SUMMARY OFF)
							 SELECT count(*) FROM zb WHERE uid = ' || k LOOP
			IF plan LIKE '%Stripes descartados%' THEN
				descartados := descartados + substring(plan from '[0-9]+$')::int;
			END IF;
			IF plan LIKE '%shared hit=%' THEN
				buffers := buffers +
					coalesce(substring(plan from 'hit=([0-9]+)')::bigint, 0);
			END IF;
		END LOOP;
		n := n + 1;
	END LOOP;
	ms := (extract(epoch from clock_timestamp() - t0) * 1000)::bigint;
	RETURN format('consultas=%s  stripes descartados=%s  buffers=%s  %s ms',
				  n, descartados, buffers, ms);
END $$ LANGUAGE plpgsql;

\echo ''
\echo '=== A/B sobre la columna sin correlación (50 lookups por igualdad) ==='
SELECT '  con filtro:   ' || medir_ab(true);
SELECT '  solo min/max: ' || medir_ab(false);
SQL
