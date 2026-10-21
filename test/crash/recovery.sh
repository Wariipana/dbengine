#!/usr/bin/env bash
# F7 — consistencia después de un crash.
#
# El "version store" son los stripes, y viven en el fork main a través del
# buffer manager con WAL de página completa (ADR-001).  O sea que su
# recuperación es la de Postgres, no una propia.  Lo que este test verifica
# es justamente eso: que después de un `pg_ctl stop -m immediate` en medio de
# la carga, al reiniciar no quede ni una fila a medias, ni un stripe cuyo
# metadato exista sin sus datos, ni un time travel que devuelva algo distinto
# de lo que quedó commiteado.
#
#   PGDATA=... PGHOST=... PGPORT=... test/crash/recovery.sh
set -euo pipefail

PGDATA="${PGDATA:?falta PGDATA}"
PSQL="${PSQL:-psql}"
DB="${DB:-crashtest}"
BIN="${PGBIN:-$(dirname "$(command -v pg_ctl)")}"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

fallos=0
chequeo() { # $1 = qué, $2 = obtenido, $3 = esperado
  if [ "$2" = "$3" ]; then printf "  ok   %-46s %s\n" "$1" "$2"
  else printf "  FALLA %-45s obtuvo=%s esperaba=%s\n" "$1" "$2" "$3"; fallos=$((fallos+1)); fi
}

$PSQL -d postgres -q -c "DROP DATABASE IF EXISTS $DB WITH (FORCE);"
$PSQL -d postgres -q -c "CREATE DATABASE $DB;"
Q -c "CREATE EXTENSION dbengine;" >/dev/null
Q -c "CREATE TABLE c (id int, val text) USING dbengine_columnar;" >/dev/null

# Carga commiteada, en transacciones separadas para que haya varios stripes.
$PSQL -d "$DB" -q -c "DO \$\$ BEGIN FOR i IN 1..300 LOOP
    INSERT INTO c VALUES (i, 'v'||i); COMMIT; END LOOP; END \$\$;" >/dev/null

antes_filas=$(Q -c "SELECT count(*) FROM c;")
antes_suma=$(Q -c "SELECT coalesce(sum(id),0) FROM c;")
antes_stripes=$(Q -c "SELECT count(*) FROM dbengine.stripe_timeline('c');")
marca=$(Q -c "SELECT now();")

# Una transacción ABIERTA que escribió pero no commiteó: al reiniciar no
# tiene que aparecer ni una de sus filas.
($PSQL -d "$DB" -q <<SQL &
BEGIN;
INSERT INTO c SELECT g, 'sin commit' FROM generate_series(10000, 10500) g;
SELECT pg_sleep(30);
COMMIT;
SQL
) 2>/dev/null
sleep 3

echo "=== crash (pg_ctl stop -m immediate) ==="
"$BIN/pg_ctl" -D "$PGDATA" -m immediate -w stop >/dev/null 2>&1 || true
"$BIN/pg_ctl" -D "$PGDATA" -w start -l "$PGDATA/../server.log" >/dev/null 2>&1
sleep 2

echo "=== después de recuperar ==="
chequeo "las filas commiteadas están todas"      "$(Q -c 'SELECT count(*) FROM c;')" "$antes_filas"
chequeo "y son las mismas"                       "$(Q -c 'SELECT coalesce(sum(id),0) FROM c;')" "$antes_suma"
chequeo "ninguna fila de la transacción abierta" "$(Q -c "SELECT count(*) FROM c WHERE val = 'sin commit';")" "0"
chequeo "los stripes siguen siendo los mismos"   "$(Q -c "SELECT count(*) FROM dbengine.stripe_timeline('c');")" "$antes_stripes"

# Cada stripe del catálogo tiene que poder leerse: si el metadato hubiera
# sobrevivido sin sus datos, esto fallaría o daría de menos.
chequeo "los datos de cada stripe se leen" \
  "$(Q -c "SELECT sum(row_count) FROM dbengine.stripe_timeline('c');")" "$antes_filas"

# Y el time travel a un momento anterior al crash tiene que dar lo mismo que
# daba antes.
if [ "$(Q -c "SHOW track_commit_timestamp;")" = "on" ]; then
  chequeo "time travel a antes del crash" \
    "$(Q -c "SELECT count(*) FROM dbengine.time_travel('c', '$marca'::timestamptz) AS (id int, val text);")" \
    "$antes_filas"
fi

# El scan columnar y el índice tienen que coincidir después de recuperar.
Q -c "CREATE INDEX c_id ON c USING btree (id);" >/dev/null
chequeo "el índice coincide con el scan" \
  "$(Q -c "SET enable_seqscan=off; SELECT count(*) FROM c WHERE id > 0;")" "$antes_filas"

echo
[ "$fallos" = "0" ] && echo "todo ok" || echo "HAY FALLAS: $fallos"
exit "$fallos"
