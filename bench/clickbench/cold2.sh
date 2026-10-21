#!/usr/bin/env bash
# ClickBench en frío, con repeticiones y dispersión.
#
# Igual que cold.sh pero corre cada query RUNS veces por motor, soltando el
# page cache antes de CADA corrida.  Reporta la mejor y cuánto se movieron
# entre sí: sobre este mismo banco, el mismo heap con la misma query se movió
# 20% entre dos corridas, así que sin dispersión no se distingue una mejora
# de ruido.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PGBIN="${PGBIN:-/usr/lib/postgresql/18/bin}"
PGDATA="${PGDATA:-/config/.dbengine/pg18/data}"
export PGHOST="${PGHOST:-/config/.dbengine/pg18/run}"
export PGPORT="${PGPORT:-54318}"
export PGDATABASE="${PGDATABASE:-postgres}"
P="$PGBIN/psql -X -q -At"
RUNS="${RUNS:-3}"

read -ra QLIST <<< "${*:-1 2 3 4 5 6 7 13 20 21 22 24 25 29 30 33 34 37 41 43}"

# Suelta del page cache TODO lo que la query va a tener que leer: la tabla,
# sus índices y —esto faltaba— el catálogo de dbengine.
#
# dbengine.stripe_column son 35 MB para esta tabla: dónde está cada segmento,
# su zone map, su filtro de Bloom y su zone map por gránulo.  Sin desalojarlo,
# solo la PRIMERA de las tres corridas era fría de verdad y el arnés reporta
# la mejor: las otras dos leían el catálogo de memoria.  Se veía en la
# dispersión, que del lado de dbengine llegaba a 665% mientras la de heap
# —que no tiene catálogo aparte— se quedaba en 0-16%.
enfriar() {
	$P -c "CHECKPOINT;" >/dev/null
	for rel in "$1" dbengine.stripe dbengine.stripe_column; do
		python3 "$ROOT/bench/evict.py" "$PGDATA" \
			"$($P -c "SELECT pg_relation_filepath('$rel');")" >/dev/null
		while read -r idx; do
			[ -z "$idx" ] && continue
			python3 "$ROOT/bench/evict.py" "$PGDATA" "$idx" >/dev/null
		done < <($P -c "SELECT pg_relation_filepath(indexrelid)
		                FROM pg_index WHERE indrelid = '$rel'::regclass;")
	done
}

# Devuelve "mejor dispersión%" tras RUNS corridas en frío.
medir() {
	local q="${1//FROM hits/FROM $2}" todos="" ms
	for _ in $(seq "$RUNS"); do
		enfriar "$2"
		ms=$($P -c "\\timing on" -c "$q" 2>/dev/null \
			| grep -oE '^Time: [0-9.]+' | grep -oE '[0-9.]+' | tail -1)
		[ -n "$ms" ] && todos="$todos $ms"
	done
	echo "$todos" | tr ' ' '\n' | grep -v '^$' \
		| awk 'NR==1{lo=hi=$1} $1<lo{lo=$1} $1>hi{hi=$1}
		       END{if(NR&&lo>0) printf "%.1f %.0f", lo, 100*(hi-lo)/lo}'
}

echo "=== ClickBench en frío · $RUNS corridas por query, cache soltado antes de cada una ==="
printf '%-4s %11s %6s %11s %6s %9s\n' "q" "heap(ms)" "disp" "dbe(ms)" "disp" "ratio"

n=0
while IFS= read -r q; do
	n=$((n + 1))
	for want in "${QLIST[@]}"; do
		[ "$want" = "$n" ] || continue
		read -r h hs <<< "$(medir "$q" hits_heap)"
		read -r d ds <<< "$(medir "$q" hits_dbengine)"
		if [ -n "${h:-}" ] && [ -n "${d:-}" ]; then
			awk -v n="$n" -v h="$h" -v hs="$hs" -v d="$d" -v ds="$ds" \
				'BEGIN{printf "%-4s %11.1f %5s%% %11.1f %5s%% %8.2fx\n",
				       n, h, hs, d, ds, (10+h)/(10+d)}'
		else
			printf '%-4s %11s %6s %11s %6s %9s\n' "$n" "${h:-error}" "" "${d:-error}" "" "-"
		fi
	done
done < "$HERE/queries.sql"
