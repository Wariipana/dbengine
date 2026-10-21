#!/usr/bin/env bash
# Benchmark de F3: column pruning y descarte de stripes por zone maps,
# contra F2 (mismo storage, seq scan del Table AM) y contra heap nativo.
#
# dbengine.enable_vectorized apaga el Custom Scan, así que la comparación
# F3/F2 usa exactamente los mismos datos y el mismo código de storage.
#
#   bench/f3-customscan.sh [filas]
set -euo pipefail

ROWS="${1:-500000}"
PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

Q -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null
Q -c "DROP TABLE IF EXISTS f3_col, f3_heap;" >/dev/null

# staging_threshold bajo a propósito: varios stripes es lo que hace que los
# zone maps tengan algo que descartar.
Q -c "SET dbengine.staging_threshold_rows = 25000;
      CREATE TABLE f3_col (id bigint, ts timestamptz, region text,
                           monto numeric(12,2), nota text)
        USING dbengine_columnar;
      INSERT INTO f3_col
      SELECT g, '2024-01-01'::timestamptz + g*interval '1 minute',
             (ARRAY['norte','sur','este','oeste'])[1+g%4],
             (g%1000)/10.0, 'nota-'||g
      FROM generate_series(1,$ROWS) g;" >/dev/null

Q -c "CREATE TABLE f3_heap (LIKE f3_col);
      INSERT INTO f3_heap SELECT * FROM f3_col;
      ANALYZE f3_col; ANALYZE f3_heap;" >/dev/null

echo "filas: $ROWS   stripes: $(Q -c "SELECT count(*) FROM dbengine.stripe WHERE relfilenode=pg_relation_filenode('f3_col');")"
echo

best() { for _ in 1 2 3; do
    $PSQL -d "$DB" -Aqt -c "$2" -c "\timing on" -c "$1" 2>&1 |
      grep '^Time' | sed 's/Time: //;s/ ms//'
  done | sort -n | head -1; }

printf "%-50s %9s %9s %9s\n" "consulta" "F3" "F2" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT sum(monto) FROM %s" \
         "SELECT count(DISTINCT region) FROM %s" \
         "SELECT count(*) FROM %s WHERE id BETWEEN 100 AND 200" \
         "SELECT sum(id) FROM %s WHERE id > $((ROWS - 10000))" \
         "SELECT count(*) FROM %s WHERE nota = 'nota-42'"; do
  printf "%-50s %9s %9s %9s\n" "$(echo "$q" | cut -c1-50)" \
    "$(best "$(printf "$q" f3_col)"  "SET dbengine.enable_vectorized=on")" \
    "$(best "$(printf "$q" f3_col)"  "SET dbengine.enable_vectorized=off")" \
    "$(best "$(printf "$q" f3_heap)" "SET dbengine.enable_vectorized=off")"
done

echo
echo "=== cuántos stripes se descartan ==="
$PSQL -d "$DB" -Aqt -c "SET max_parallel_workers_per_gather=0;" \
  -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
      SELECT count(*) FROM f3_col WHERE id BETWEEN 100 AND 200;" |
  grep -E "Stripes|Columnas|Buffers"
