#!/usr/bin/env bash
# Gate de F1: la suite de regresión de PostgreSQL contra el AM columnar.
#
# Un storage append-only no puede pasarla entera, así que el gate no mira
# cuántos tests pasan sino dos cosas:
#
#   1. El servidor no se cae: ni un `terminated by signal` ni un `TRAP` en el
#      log.  Con un PostgreSQL compilado con --enable-cassert esto es lo que
#      más bugs encontró.
#   2. Los tests que fallan con columnar y NO fallan con heap son exactamente
#      los de test/f1-gate/expected-failures.txt.  Uno nuevo es una
#      regresión; uno que deja de fallar es una mejora, y el script pide
#      sacarlo de la lista para que no se pierda.
#
# Corre la suite dos veces sobre un cluster temporal propio: con heap (la
# línea base, que absorbe las diferencias de build entre el servidor y el
# árbol de fuentes: ICU, lz4, ...) y con default_table_access_method =
# 'dbengine_columnar'.
#
#   PGSRC=~/src/postgresql-18.6 PGBIN=/usr/lib/postgresql/18/bin tools/f1-gate.sh
#
# PGSRC es un árbol configurado con src/test/regress compilado (`make -C
# src/test/regress`): de ahí salen pg_regress, regress.so y los tests.
# GATE_UPDATE=1 reescribe la lista de fallos esperados con lo obtenido.
set -euo pipefail

SRC="${PGSRC:?falta PGSRC (árbol de fuentes de PostgreSQL configurado)}"
BIN="${PGBIN:-$(dirname "$(command -v pg_ctl)")}"
PORT="${GATE_PORT:-55441}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LISTA="$ROOT/test/f1-gate/expected-failures.txt"
REG="$SRC/src/test/regress"

# `vacuum` no termina con columnar: sus DELETE fallan y los INSERT ... SELECT
# que los siguen duplican la tabla en cada vuelta, hasta mil millones de
# filas.  Se saca del schedule hasta que exista DELETE.
SALTEAR="${GATE_SKIP:-vacuum}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dbegate.XXXX")"
limpiar() {
  "$BIN/pg_ctl" -D "$WORK/data" -m immediate -w stop >/dev/null 2>&1 || true
  [ -n "${GATE_KEEP:-}" ] && echo "resultados en $WORK" || rm -rf "$WORK"
}
trap limpiar EXIT

"$BIN/initdb" -D "$WORK/data" -A trust -U postgres >/dev/null
cat >> "$WORK/data/postgresql.conf" <<EOF
shared_preload_libraries = 'dbengine'
track_commit_timestamp = on
unix_socket_directories = '$WORK'
listen_addresses = ''
max_connections = 50
# Sin transacciones preparadas: prepared_xacts tiene una salida esperada
# para ese caso.  Con columnar, uno de sus pasos falla y deja preparada una
# transacción con un lock tomado, y el DROP siguiente espera para siempre.
max_prepared_transactions = 0
EOF
"$BIN/pg_ctl" -D "$WORK/data" -o "-p $PORT" -l "$WORK/server.log" -w start >/dev/null

# Schedule sin los tests salteados.
schedule="$WORK/schedule"
python3 - "$REG/parallel_schedule" "$schedule" $SALTEAR <<'PY'
import sys
src, dst, skip = sys.argv[1], sys.argv[2], set(sys.argv[3:])
out = []
for line in open(src):
    if line.startswith("test:"):
        tests = [t for t in line.split()[1:] if t not in skip]
        if not tests:
            continue
        line = "test: " + " ".join(tests) + "\n"
    out.append(line)
open(dst, "w").writelines(out)
PY

# Corre la suite y deja en $2 la lista ordenada de tests que fallaron.
correr() { # $1 = nombre, $2 = archivo de fallos, $3 = AM
  local out="$WORK/$1"
  mkdir -p "$out"
  "$BIN/psql" -h "$WORK" -p "$PORT" -U postgres -d postgres -X -q <<SQL
DROP DATABASE IF EXISTS regression;
CREATE DATABASE regression TEMPLATE template0;
\c regression
CREATE EXTENSION dbengine;
ALTER DATABASE regression SET default_table_access_method = '$3';
-- Red de seguridad: un test que deja un lock tomado tiene que fallar, no
-- colgar el gate.
ALTER DATABASE regression SET lock_timeout = '120s';
SQL
  (cd "$REG" && timeout "${GATE_TIMEOUT:-3600}" ./pg_regress --use-existing --dbname=regression \
      --host="$WORK" --port="$PORT" --user=postgres \
      --bindir="$BIN" --dlpath="$REG" --inputdir="$REG" \
      --outputdir="$out" --schedule="$schedule" >"$out/salida.txt" 2>&1) || true
  grep -E '^not ok' "$out/salida.txt" | awk '{print $5}' | sort -u > "$2" || true
  printf "  %-8s %3d tests fallan\n" "$1" "$(wc -l < "$2")"
}

echo "=== suite de PostgreSQL $("$BIN/postgres" --version | awk '{print $3}') ==="
correr heap     "$WORK/fallan-heap"     heap
correr columnar "$WORK/fallan-columnar" dbengine_columnar

fallos=0

# 1. Ningún crash.
caidas=$(grep -cE 'terminated by signal|TRAP:' "$WORK/server.log" || true)
if [ "$caidas" -ne 0 ]; then
  echo "FALLA: el servidor se cayó $caidas vez/veces:"
  grep -E 'terminated by signal|TRAP:' "$WORK/server.log" | head -10
  fallos=1
else
  echo "  ok   ni un crash ni una aserción"
fi

# 2. Los fallos propios de columnar son los esperados.
#
# Propio no es "falla con columnar y no con heap": un test que con heap falla
# por el build (ICU, lz4, locale) puede fallar con columnar por algo más, y
# el build cambia de máquina en máquina.  Propio es "el diff con columnar no
# es el mismo que con heap".
python3 - "$WORK/heap/regression.diffs" "$WORK/columnar/regression.diffs" \
    "$WORK/fallan-columnar" > "$WORK/propios" <<'PY'
import re, sys

def diffs(path):
    try:
        txt = open(path, errors="replace").read()
    except FileNotFoundError:
        return {}
    out = {}
    parts = re.split(r"^diff .*?/expected/(\S+?)(?:_\d+)?\.out .*$", txt, flags=re.M)
    for i in range(1, len(parts), 2):
        # Los OIDs cambian de corrida en corrida: no son una diferencia.
        body = [re.sub(r"\d{5,}", "N", l) for l in parts[i + 1].splitlines()
                if l[:1] in "+-" and not l.startswith(("+++", "---"))]
        out[parts[i]] = body
    return out

heap, col = diffs(sys.argv[1]), diffs(sys.argv[2])
for t in sorted(set(open(sys.argv[3]).read().split())):
    if heap.get(t) != col.get(t):
        print(t)
PY

if [ -n "${GATE_UPDATE:-}" ]; then
  mkdir -p "$(dirname "$LISTA")"
  {
    echo "# Tests de la suite de PostgreSQL que fallan con dbengine_columnar como"
    echo "# AM por default y no fallan con heap.  La mantiene tools/f1-gate.sh;"
    echo "# regenerar con GATE_UPDATE=1 después de revisar cada cambio a mano."
    cat "$WORK/propios"
  } > "$LISTA"
  echo "  lista actualizada: $(wc -l < "$WORK/propios") tests"
fi

grep -vE '^(#|$)' "$LISTA" | sort -u > "$WORK/esperados"
nuevos=$(comm -23 "$WORK/propios" "$WORK/esperados")
arreglados=$(comm -13 "$WORK/propios" "$WORK/esperados")

if [ -n "$nuevos" ]; then
  echo "FALLA: tests que ahora fallan con columnar y antes no:"
  echo "$nuevos" | sed 's/^/    /'
  echo "  (diffs en $WORK/columnar; GATE_KEEP=1 para conservarlos)"
  fallos=1
else
  echo "  ok   $(wc -l < "$WORK/propios") fallos propios de columnar, todos esperados"
fi
if [ -n "$arreglados" ]; then
  echo "  mejora: estos tests ya no fallan; sacarlos de $LISTA:"
  echo "$arreglados" | sed 's/^/    /'
fi

[ "$fallos" -eq 0 ] && echo "f1-gate: OK" || { echo "f1-gate: FALLA"; exit 1; }
