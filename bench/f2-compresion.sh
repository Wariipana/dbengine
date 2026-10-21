#!/usr/bin/env bash
# Benchmark de F2: ratio de compresión y tiempo de scan contra F1 (crudo) y
# contra heap nativo.  El plan pide medir cada fase contra la anterior; la
# GUC dbengine.enable_compression permite hacerlo con el mismo código.
#
#   bench/f2-compresion.sh [filas]
set -euo pipefail

ROWS="${1:-500000}"
PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

GEN="SELECT g::bigint,
  '2024-01-01'::timestamptz + (g%86400)*interval '1 second',
  (ARRAY['norte','sur','este','oeste','centro'])[1+g%5],
  (ARRAY['activo','pausado','cerrado'])[1+g%3],
  ((g%100000)/100.0)::numeric(12,2), (g%500)::int, (g%2=0),
  'nota-'||(g%1000)
FROM generate_series(1,$ROWS) g"

Q -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null
Q -c "DROP TABLE IF EXISTS bench_f2, bench_f1, bench_heap;" >/dev/null
Q -c "CREATE TABLE bench_heap (id bigint, ts timestamptz, region text,
        estado text, monto numeric(12,2), cantidad int, activo boolean,
        nota text);" >/dev/null
Q -c "CREATE TABLE bench_f2 (LIKE bench_heap) USING dbengine_columnar;" >/dev/null
Q -c "CREATE TABLE bench_f1 (LIKE bench_heap) USING dbengine_columnar;" >/dev/null

echo "cargando $ROWS filas..."
Q -c "SET dbengine.enable_compression=on;  INSERT INTO bench_f2 $GEN;" >/dev/null
Q -c "SET dbengine.enable_compression=off; INSERT INTO bench_f1 $GEN;" >/dev/null
Q -c "INSERT INTO bench_heap $GEN;" >/dev/null

echo
echo "=== TAMAÑO ==="
Q -c "SELECT 'columnar F2 (comprimido): ' || pg_size_pretty(pg_relation_size('bench_f2'))
UNION ALL SELECT 'columnar F1 (crudo):      ' || pg_size_pretty(pg_relation_size('bench_f1'))
UNION ALL SELECT 'heap nativo:              ' || pg_size_pretty(pg_relation_size('bench_heap'))
UNION ALL SELECT 'ratio vs F1:              ' || round(pg_relation_size('bench_f1')::numeric/pg_relation_size('bench_f2'),2)||'x'
UNION ALL SELECT 'ratio vs heap:            ' || round(pg_relation_size('bench_heap')::numeric/pg_relation_size('bench_f2'),2)||'x';"

echo
echo "=== CODIFICACIÓN ELEGIDA POR COLUMNA ==="
Q -c "SELECT a.attname || ': ' ||
        CASE sc.encoding WHEN 0 THEN 'crudo' WHEN 1 THEN 'RLE'
                         WHEN 2 THEN 'FOR' WHEN 3 THEN 'diccionario' END
      FROM dbengine.stripe_column sc
      JOIN pg_attribute a ON a.attrelid='bench_f2'::regclass AND a.attnum=sc.attnum
      WHERE sc.relfilenode=pg_relation_filenode('bench_f2') ORDER BY sc.attnum;"

echo
echo "=== BLOQUES LEÍDOS (reiniciar el servidor antes para vaciar buffers) ==="
for t in bench_f2 bench_f1 bench_heap; do
  r=$(Q -c "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
            SELECT count(*) FROM $t;" | grep -oE "read=[0-9]+" | head -1)
  printf "%-12s %s\n" "$t" "${r:-todo en cache}"
done

echo
echo "=== TIEMPO DE SCAN (mejor de 3, sin paralelismo) ==="
best() { for _ in 1 2 3; do
    $PSQL -d "$DB" -Aqt -c "SET max_parallel_workers_per_gather=0;" \
        -c "\timing on" -c "$1" 2>&1 | grep '^Time' | sed 's/Time: //;s/ ms//'
  done | sort -n | head -1; }

printf "%-44s %9s %9s %9s\n" "consulta" "F2" "F1" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT sum(cantidad) FROM %s" \
         "SELECT count(*) FROM %s WHERE nota='nota-42'" \
         "SELECT count(DISTINCT region) FROM %s"; do
  printf "%-44s %9s %9s %9s\n" "$(echo "$q" | cut -c1-44)" \
    "$(best "$(printf "$q" bench_f2)")" \
    "$(best "$(printf "$q" bench_f1)")" \
    "$(best "$(printf "$q" bench_heap)")"
done
