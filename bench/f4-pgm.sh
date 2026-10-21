#!/usr/bin/env bash
# Benchmark de F4: índice PGM contra btree, sobre los MISMOS datos.
#
# Se miden las tres cosas que definen si un índice aprendido vale la pena:
# cuánto ocupa, cuánto tarda un lookup puntual y cuánto un rango.
#
#   bench/f4-pgm.sh [filas]
set -euo pipefail

ROWS="${1:-5000000}"
PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

Q -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null
Q -c "DROP TABLE IF EXISTS f4;" >/dev/null

# Tabla heap: lo que se compara es el ÍNDICE, no el storage.
Q -c "CREATE TABLE f4 (sec bigint, ale bigint, clu bigint, pad text);" >/dev/null
Q -c "INSERT INTO f4
      SELECT g,
             (random() * 1000000000)::bigint,
             ((g / 1000) * 1000000000::bigint + g % 1000),
             repeat('x', 20)
      FROM generate_series(1, $ROWS) g;" >/dev/null
Q -c "ANALYZE f4;" >/dev/null

tiempo() { # $1 = query
    $PSQL -d "$DB" -Aqt -c "\timing on" -c "$1" 2>&1 |
        grep '^Time' | sed 's/Time: //; s/ ms.*//'
}
mejor() { for _ in 1 2 3 4 5; do tiempo "$1"; done | sort -n | head -1; }

echo "filas: $(Q -c 'SELECT count(*) FROM f4;')"
echo

for col in sec ale clu; do
  case $col in
    sec) desc="secuencial (1..n)" ;;
    ale) desc="aleatoria uniforme" ;;
    clu) desc="clusterizada (huecos grandes)" ;;
  esac

  Q -c "DROP INDEX IF EXISTS f4_pgm, f4_bt;" >/dev/null
  t0=$(date +%s.%N)
  Q -c "CREATE INDEX f4_pgm ON f4 USING dbengine_pgm ($col);" >/dev/null
  t1=$(date +%s.%N)
  Q -c "CREATE INDEX f4_bt ON f4 USING btree ($col);" >/dev/null
  t2=$(date +%s.%N)

  spgm=$(Q -c "SELECT pg_relation_size('f4_pgm');")
  sbt=$(Q -c "SELECT pg_relation_size('f4_bt');")

  # Punto medio de los datos, para que el lookup exista.
  k=$(Q -c "SELECT $col FROM f4 ORDER BY $col OFFSET $((ROWS/2)) LIMIT 1;")
  hi=$(Q -c "SELECT $col FROM f4 ORDER BY $col OFFSET $((ROWS/2 + ROWS/1000)) LIMIT 1;")

  # Un lookup suelto lo tapa el overhead del executor.  Se miden 10.000
  # sondas en una sola query, con nested loop, para que lo que se vea sea el
  # índice y no el andamiaje.
  probe="SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_hashjoin=off;
         SET enable_mergejoin=off; SET enable_material=off;"
  sondas="SELECT count(*) FROM (SELECT $col AS k FROM f4 ORDER BY random() LIMIT 10000) s
          JOIN f4 ON f4.$col = s.k;"

  Q -c "DROP INDEX f4_bt;" >/dev/null
  pgm_eq=$(mejor "$probe $sondas")
  Q -c "CREATE INDEX f4_bt ON f4 USING btree ($col);" >/dev/null
  Q -c "DROP INDEX f4_pgm;" >/dev/null
  bt_eq=$(mejor "$probe $sondas")
  Q -c "CREATE INDEX f4_pgm ON f4 USING dbengine_pgm ($col);" >/dev/null

  echo "=== $col — $desc ==="
  printf "  %-22s PGM %10s   btree %10s   (%.2fx)\n" "tamaño" \
     "$(Q -c "SELECT pg_size_pretty($spgm::bigint);")" \
     "$(Q -c "SELECT pg_size_pretty($sbt::bigint);")" \
     "$(Q -c "SELECT round($sbt::numeric / GREATEST($spgm,1), 2);")"
  printf "  %-22s PGM %10.3f   btree %10.3f\n" "build (s)" \
     "$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b-a}')" "$(awk -v a=$t1 -v b=$t2 'BEGIN{printf "%.2f", b-a}')"
  printf "  %-22s PGM %10s   btree %10s\n" "10k sondas (ms)" "$pgm_eq" "$bt_eq"
  echo
done

echo "=== cuánto ocupa el MODELO (lo que reemplaza al árbol) ==="
Q -c "DROP INDEX IF EXISTS f4_pgm, f4_bt;" >/dev/null
Q -c "CREATE INDEX f4_pgm ON f4 USING dbengine_pgm (sec);" >/dev/null
Q -c "SELECT '  índice completo: ' || pg_size_pretty(pg_relation_size('f4_pgm'));"
