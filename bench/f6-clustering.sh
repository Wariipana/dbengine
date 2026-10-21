#!/usr/bin/env bash
# F6 — clustering adaptativo y fusión de stripes.
#
# Cuatro cosas, en orden de lo que decide la fase:
#
#   1. cuánto mejora el descarte de bloques respecto al orden de inserción
#   2. qué se PIERDE al reordenar: una tabla se ordena por una sola cosa
#   3. si el mecanismo adaptativo sirve con un workload errático o solo con
#      uno repetitivo — que es la pregunta que decide si la adaptividad vale
#   4. cuántas pasadas tarda la fusión de stripes en converger
#
#   bench/f6-clustering.sh [filas]
set -euo pipefail

ROWS="${1:-10000000}"
PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }
SET="SET dbengine.enable_agg_pushdown=off; SET max_parallel_workers_per_gather=0;
     SET enable_indexscan=off; SET enable_bitmapscan=off; SET enable_seqscan=on;"

Q -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null
Q -c "DROP TABLE IF EXISTS f6;" >/dev/null

# `ts` sigue el orden de inserción; `uid` no; `sku` tampoco, y es de baja
# cardinalidad; `pad` es carga muerta para que la tabla pese.
Q -c "CREATE TABLE f6 (ts bigint, uid bigint, sku int, pad text)
      USING dbengine_columnar;" >/dev/null
Q -c "INSERT INTO f6
      SELECT g, (g::bigint * 7919) % 1000000, ((g::bigint * 104729) % 5000)::int,
             repeat('x', 24)
      FROM generate_series(1, $ROWS) g;" >/dev/null
Q -c "CREATE INDEX f6_uid ON f6 USING btree (uid);" >/dev/null
Q -c "CREATE INDEX f6_ts  ON f6 USING btree (ts);" >/dev/null
Q -c "CREATE INDEX f6_sku ON f6 USING btree (sku);" >/dev/null
Q -c "ANALYZE f6;" >/dev/null

leidas() { $PSQL -d "$DB" -Aqt -c "$SET" \
    -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT count(*) FROM f6 WHERE $1;" |
    grep -oE "Rows Removed by Filter: [0-9]+" | grep -oE "[0-9]+$" || echo 0; }
descarta() { $PSQL -d "$DB" -Aqt -c "$SET" \
    -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
        SELECT count(*) FROM f6 WHERE $1;" |
    grep -oE "Stripes descartados: [0-9]+" | grep -oE "[0-9]+$" || echo 0; }
ms() { for _ in 1 2 3; do
    $PSQL -d "$DB" -Aqt -c "$SET" -c "\timing on" -c "SELECT count(*) FROM f6 WHERE $1;" |
      grep '^Time' | sed 's/Time: //; s/ ms.*//'
  done | sort -n | head -1; }
stripes() { Q -c "SELECT count(*) FROM dbengine.stripe
                  WHERE relfilenode = pg_relation_filenode('f6');"; }
tam() { Q -c "SELECT pg_size_pretty(pg_relation_size('f6'));"; }

UID_K=$(Q -c "SELECT uid FROM f6 LIMIT 1;")
TS_LO=$((ROWS / 2)); TS_HI=$((TS_LO + 100))
P_UID="uid = $UID_K"
P_TS="ts BETWEEN $TS_LO AND $TS_HI"

fila() { printf "  %-24s %-6s %-9s %-8s | %-6s %-9s %-8s | %s\n" \
    "$1" "$(descarta "$P_UID")" "$(leidas "$P_UID")" "$(ms "$P_UID")" \
    "$(descarta "$P_TS")" "$(leidas "$P_TS")" "$(ms "$P_TS")" "$(tam)"; }

echo "filas: $(Q -c 'SELECT count(*) FROM f6;')   stripes: $(stripes)"
echo
echo "                                    uid = k (sin correlación)      | ts BETWEEN (correlacionada)   |"
printf "  %-24s %-6s %-9s %-8s | %-6s %-9s %-8s | %s\n" \
   "estado" "desc." "leídas" "ms" "desc." "leídas" "ms" "tamaño"
fila "como se insertó"

t0=$(date +%s.%N); Q -c "CLUSTER f6 USING f6_uid;" >/dev/null; t1=$(date +%s.%N)
Q -c "ANALYZE f6;" >/dev/null
fila "clusterizada por uid"
printf "  CLUSTER por uid: %s s\n" "$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.1f", b-a}')"

Q -c "CLUSTER f6 USING f6_ts;" >/dev/null; Q -c "ANALYZE f6;" >/dev/null
fila "clusterizada por ts"

Q -c "SET dbengine.bloom_bits_per_row=8; CLUSTER f6 USING f6_ts;" >/dev/null
Q -c "ANALYZE f6;" >/dev/null
fila "ts + Bloom en uid"

echo
echo "=== compresión: ¿agrupar valores parecidos comprime mejor? ==="
Q -c "CLUSTER f6 USING f6_sku;" >/dev/null
$PSQL -d "$DB" -c "
  SELECT a.attname AS columna,
         pg_size_pretty(sum(c.seg_length)) AS bytes_clusterizado_por_sku
  FROM dbengine.stripe_column c
  JOIN pg_attribute a ON a.attrelid = 'f6'::regclass AND a.attnum = c.attnum
  WHERE c.relfilenode = pg_relation_filenode('f6')
  GROUP BY a.attname ORDER BY a.attname;"

echo
echo "=== convergencia de la fusión de stripes ==="
# Autocommit fila a fila: un stripe por transacción.  El presupuesto por
# corrida es dbengine.max_blocks_per_compaction.
Q -c "DROP TABLE IF EXISTS conv;" >/dev/null
Q -c "CREATE TABLE conv (a int, b text) USING dbengine_columnar;" >/dev/null
$PSQL -d "$DB" -q -c "DO \$\$ BEGIN FOR i IN 1..500 LOOP
    INSERT INTO conv VALUES (i, 'v'||i); COMMIT; END LOOP; END \$\$;" >/dev/null
convst() { Q -c "SELECT count(*) FROM dbengine.stripe
                 WHERE relfilenode = pg_relation_filenode('conv');"; }
printf "  presupuesto=%s   al insertar: stripes=%s  tamaño=%s\n" \
  "$(Q -c 'SHOW dbengine.max_blocks_per_compaction;')" "$(convst)" \
  "$(Q -c "SELECT pg_size_pretty(pg_relation_size('conv'));")"
p=0
while : ; do
  p=$((p + 1))
  n=$(Q -c "SELECT dbengine.merge_stripes('conv');")
  printf "    pasada %s: fusionó %-5s -> stripes=%-5s filas=%s\n" \
    "$p" "$n" "$(convst)" "$(Q -c 'SELECT count(*) FROM conv;')"
  [ "$n" = "0" ] && break
  [ "$p" -gt 6 ] && break
done
Q -c "VACUUM FULL conv;" >/dev/null
printf "    tras VACUUM FULL: tamaño=%s  filas=%s\n" \
  "$(Q -c "SELECT pg_size_pretty(pg_relation_size('conv'));")" \
  "$(Q -c 'SELECT count(*) FROM conv;')"
