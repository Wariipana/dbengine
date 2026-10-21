#!/usr/bin/env bash
# ClickBench en FRÍO: las mismas queries, pero soltando el page cache de la
# tabla antes de cada una.
#
# El benchmark en caliente mide CPU y esconde la compresión — es el régimen
# menos favorable a un column store, y ClickBench oficial le da al tiempo en
# frío un 20% de su score combinado.  No hace falta root: posix_fadvise
# suelta las páginas de UNA relación, que es lo que se necesita
# (bench/evict.py).
#
# Hace falta un CHECKPOINT antes de soltar: las páginas sucias no se sueltan.
#
#   bench/clickbench/cold.sh [queries...]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PGBIN="${PGBIN:-/usr/lib/postgresql/18/bin}"
PGDATA="${PGDATA:-/config/.dbengine/pg18/data}"
export PGHOST="${PGHOST:-/config/.dbengine/pg18/run}"
export PGPORT="${PGPORT:-54318}"
export PGDATABASE="${PGDATABASE:-postgres}"
P="$PGBIN/psql -X -q -At"

# Números de query a correr; por default las que dependen de leer datos.
QUERIES=("${@:-1 2 3 4 5 6 7 13 20 21 22 24 25 29 30 33 34 37 41 43}")
read -ra QLIST <<< "${QUERIES[*]}"

enfriar() {   # $1 = tabla
	local rel
	$P -c "CHECKPOINT;" >/dev/null
	rel=$($P -c "SELECT pg_relation_filepath('$1');")
	python3 "$ROOT/bench/evict.py" "$PGDATA" "$rel" >/dev/null
	# Y sus índices, que si no quedan calientes y falsean la comparación.
	while read -r idx; do
		[ -z "$idx" ] && continue
		python3 "$ROOT/bench/evict.py" "$PGDATA" "$idx" >/dev/null
	done < <($P -c "SELECT pg_relation_filepath(indexrelid)
	                FROM pg_index WHERE indrelid = '$1'::regclass;")
}

medir() {     # $1 = query, $2 = tabla
	local q="${1//FROM hits/FROM $2}"
	enfriar "$2"
	$P -c "\\timing on" -c "$q" 2>/dev/null \
		| grep -oE '^Time: [0-9.]+' | grep -oE '[0-9.]+' | tail -1
}

echo "=== ClickBench en frío (page cache de la relación soltado por query) ==="
printf '%-4s %12s %12s %8s\n' "q" "heap(ms)" "dbe(ms)" "ratio"

n=0
while IFS= read -r q; do
	n=$((n + 1))
	for want in "${QLIST[@]}"; do
		[ "$want" = "$n" ] || continue
		h=$(medir "$q" hits_heap)
		d=$(medir "$q" hits_dbengine)
		if [ -n "$h" ] && [ -n "$d" ]; then
			awk -v n="$n" -v h="$h" -v d="$d" \
				'BEGIN{printf "%-4s %12.1f %12.1f %7.2fx\n", n, h, d, (10+h)/(10+d)}'
		else
			printf '%-4s %12s %12s %8s\n' "$n" "${h:-error}" "${d:-error}" "-"
		fi
	done
done < "$HERE/queries.sql"
