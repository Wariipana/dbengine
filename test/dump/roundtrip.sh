#!/usr/bin/env bash
# pg_dump de un cluster y restore en otro.
#
# pg_dump vuelca los datos de una tabla columnar con COPY, que pasa por el
# AM, y el restore los vuelve a escribir en stripes nuevos.  El catálogo
# interno (dbengine.stripe, stripe_column, ...) NO tiene que viajar en el
# dump: está indexado por relfilenode, que es del cluster de origen.  Cuando
# viajaba, el restore en un cluster recién creado —donde los relfilenode
# arrancan en el mismo rango— chocaba con la clave primaria de `stripe` y la
# tabla quedaba ilegible ("lectura fuera del storage").
#
# Por eso el test levanta DOS clusters nuevos: es el caso que lo reproduce
# siempre.  No necesita un cluster corriendo, solo los binarios.
#
#   PGBIN=/usr/lib/postgresql/18/bin test/dump/roundtrip.sh
set -euo pipefail

BIN="${PGBIN:-$(dirname "$(command -v pg_ctl)")}"
PORT_SRC="${PORT_SRC:-55431}"
PORT_DST="${PORT_DST:-55432}"

# El socket va en un directorio corto: el límite de la ruta es 107 bytes.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dbedump.XXXX")"

limpiar() {
  for c in src dst; do
    "$BIN/pg_ctl" -D "$WORK/$c" -m fast -w stop >/dev/null 2>&1 || true
  done
  rm -rf "$WORK"
}
trap limpiar EXIT

for c in src dst; do
  "$BIN/initdb" -D "$WORK/$c" -A trust -U postgres >/dev/null
  cat >> "$WORK/$c/postgresql.conf" <<EOF
shared_preload_libraries = 'dbengine'
track_commit_timestamp = on
unix_socket_directories = '$WORK'
listen_addresses = ''
EOF
done
"$BIN/pg_ctl" -D "$WORK/src" -o "-p $PORT_SRC" -l "$WORK/src.log" -w start >/dev/null
"$BIN/pg_ctl" -D "$WORK/dst" -o "-p $PORT_DST" -l "$WORK/dst.log" -w start >/dev/null

SRC() { "$BIN/psql" -h "$WORK" -p "$PORT_SRC" -U postgres -d postgres -X -Aqt "$@"; }
DST() { "$BIN/psql" -h "$WORK" -p "$PORT_DST" -U postgres -d postgres -X -Aqt "$@"; }

fallos=0
chequeo() { # $1 = qué, $2 = obtenido, $3 = esperado
  if [ "$2" = "$3" ]; then printf "  ok   %-46s %s\n" "$1" "$2"
  else printf "  FALLA %-45s obtuvo=%s esperaba=%s\n" "$1" "$2" "$3"; fallos=$((fallos+1)); fi
}

# Varias formas de tabla: varios stripes, NULLs, valores grandes, índice,
# CLUSTER (relation_order) y particionada.
SRC -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
CREATE EXTENSION dbengine;
CREATE TABLE t (id int, v text, n numeric) USING dbengine_columnar;
INSERT INTO t SELECT g, CASE WHEN g % 7 = 0 THEN NULL ELSE 'v' || g END, g / 3.0
  FROM generate_series(1, 150000) g;
INSERT INTO t VALUES (-1, repeat('x', 20000), NULL);
CREATE INDEX t_id ON t (id);

CREATE TABLE c (k int, payload text) USING dbengine_columnar;
INSERT INTO c SELECT (g * 7919) % 50000, 'p' || g FROM generate_series(1, 50000) g;
CREATE INDEX c_k ON c (k);
CLUSTER c USING c_k;

CREATE TABLE p (d date, x int) PARTITION BY RANGE (d);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM ('2026-01-01') TO ('2026-07-01')
  USING dbengine_columnar;
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM ('2026-07-01') TO ('2027-01-01')
  USING dbengine_columnar;
INSERT INTO p SELECT date '2026-01-01' + (g % 365), g FROM generate_series(1, 30000) g;
SQL

# Huella de cada tabla: independiente del orden físico.
huella() {
  "$1" -c "SELECT md5(string_agg(r::text, '|' ORDER BY r::text)) FROM $2 r;"
}
declare -A antes
for tabla in t c p; do antes[$tabla]="$(huella SRC "$tabla")"; done

"$BIN/pg_dump" -h "$WORK" -p "$PORT_SRC" -U postgres -d postgres -Fc -f "$WORK/dump.pgc"
restore_log="$WORK/restore.log"
if "$BIN/pg_restore" -h "$WORK" -p "$PORT_DST" -U postgres -d postgres \
     --exit-on-error "$WORK/dump.pgc" >"$restore_log" 2>&1; then
  chequeo "pg_restore termina sin errores" "ok" "ok"
else
  chequeo "pg_restore termina sin errores" "$(head -c 200 "$restore_log")" "ok"
fi

echo "=== después de restaurar ==="
for tabla in t c p; do
  chequeo "$tabla: mismos datos" "$(huella DST "$tabla" 2>&1)" "${antes[$tabla]}"
done

# Ninguna fila del catálogo puede referirse a un relfilenode que no sea de
# una tabla de este cluster.
chequeo "sin stripes huérfanos" "$(DST -c "
  SELECT count(*) FROM dbengine.stripe s
  WHERE NOT EXISTS (SELECT 1 FROM pg_class r
                    WHERE pg_relation_filenode(r.oid) = s.relfilenode);")" "0"
chequeo "sin segmentos huérfanos" "$(DST -c "
  SELECT count(*) FROM dbengine.stripe_column s
  WHERE NOT EXISTS (SELECT 1 FROM dbengine.stripe x
                    WHERE x.relfilenode = s.relfilenode
                      AND x.stripe_id = s.stripe_id);")" "0"

# El índice restaurado tiene que coincidir con el scan.
chequeo "el índice coincide con el scan" \
  "$(DST -c "SET enable_seqscan = off; SET dbengine.enable_vectorized = off;
             SELECT count(*) FROM t WHERE id > 1000;")" "149000"

# Y la tabla restaurada tiene que seguir aceptando escrituras.
DST -c "INSERT INTO t VALUES (200000, 'después', 1);" >/dev/null
chequeo "se puede escribir después del restore" \
  "$(DST -c "SELECT count(*) FROM t;")" "150002"

if [ "$fallos" -ne 0 ]; then
  echo "check-dump: $fallos chequeo(s) fallaron"
  exit 1
fi
echo "check-dump: OK"
