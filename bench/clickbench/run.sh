#!/usr/bin/env bash
# ClickBench (subconjunto de 10M filas) sobre dbengine y sobre heap nativo.
#
# Los datos son los reales de ClickBench (hits.tsv), truncados a las primeras
# 10M filas: el dataset completo son 100M filas / ~70 GB, que no entran en el
# disco de desarrollo. Los números NO son comparables con la tabla oficial de
# ClickBench; sirven para comparar dbengine contra heap sobre el mismo dato.
#
# Las 43 queries y el DDL son los oficiales, sin tocar.
#
#   bench/clickbench/run.sh [ruta-al-tsv]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TSV="${1:-/config/.dbengine/clickbench/hits10m.tsv}"
PGBIN="${PGBIN:-/usr/lib/postgresql/18/bin}"
export PGHOST="${PGHOST:-/config/.dbengine/pg18/run}"
export PGPORT="${PGPORT:-54318}"
export PGDATABASE="${PGDATABASE:-postgres}"
PSQL="$PGBIN/psql -X -q -v ON_ERROR_STOP=1"
RUNS="${RUNS:-3}"

[ -r "$TSV" ] || { echo "no encuentro el TSV: $TSV" >&2; exit 1; }

echo "=== ClickBench / 10M filas — dbengine vs heap ==="
echo "datos: $TSV"

$PSQL -c "CREATE EXTENSION IF NOT EXISTS dbengine;" >/dev/null

# --- esquema ---------------------------------------------------------------
# El DDL oficial crea "hits"; lo emitimos dos veces con nombres distintos.
# El access method se llama dbengine_columnar; "dbengine" es la etiqueta
# corta que usamos en la salida.
am_of() { [ "$1" = dbengine ] && echo dbengine_columnar || echo heap; }

for am in heap dbengine; do
	[ -n "${SKIP_LOAD:-}" ] && continue
	tbl="hits_$am"
	$PSQL -c "DROP TABLE IF EXISTS $tbl;" >/dev/null
	sed "s/^CREATE TABLE hits\$/CREATE TABLE $tbl/" "$HERE/create.sql" \
		| sed "s/^);\$/) USING $(am_of "$am");/" \
		| $PSQL >/dev/null
done

# --- carga -----------------------------------------------------------------
# SKIP_LOAD=1 reusa las tablas ya cargadas (para repetir la medición sin
# volver a pagar los ~6 minutos de COPY).
if [ -z "${SKIP_LOAD:-}" ]; then
for am in heap dbengine; do
	tbl="hits_$am"
	t0=$(date +%s.%N)
	$PSQL -c "\\copy $tbl FROM '$TSV'" >/dev/null
	sync
	t1=$(date +%s.%N)
	$PSQL -c "VACUUM ANALYZE $tbl;" >/dev/null
	printf 'carga %-9s %8.1f s\n' "$am" \
		"$(awk -v a="$t0" -v b="$t1" 'BEGIN{print b-a}')"
done
else
	echo "(carga salteada: SKIP_LOAD=1)"
fi

echo
echo "--- tamaño en disco ---"
$PSQL -c "SELECT 'heap' AS am, pg_size_pretty(pg_total_relation_size('hits_heap')) AS tam
          UNION ALL
          SELECT 'dbengine', pg_size_pretty(pg_total_relation_size('hits_dbengine'));"

# --- queries ---------------------------------------------------------------
# Tres corridas por query, nos quedamos con la mejor.
#
# Cache CALIENTE, y eso es una limitación real de esta medición, no una
# condición neutra: es el régimen donde la compresión no puede lucirse, o sea
# el menos favorable a la única ventaja estructural del motor.  ClickBench
# oficial pesa el tiempo en frío un 20% de su score combinado.
#
# No hace falta root para medir en frío: bench/evict.py suelta las páginas de
# UNA relación con posix_fadvise(DONTNEED), que es justo lo que se necesita.
# Ver bench/clickbench/README.md.
# --- correctitud ------------------------------------------------------------
# Un motor que responde rápido y mal no sirve: antes de medir, las 43 queries
# tienen que devolver exactamente lo mismo en las dos tablas.
echo
echo "--- correctitud: heap vs dbengine, 43 queries ---"
# Se compara el resultado COMPLETO, sin el LIMIT, y con un agregado que no
# depende del orden.
#
# Ordenar la salida antes de comparar no alcanzaba: cinco de las 43 son
# `ORDER BY count(*) DESC LIMIT 10` sobre miles de grupos empatados, y ahí
# los dos motores devuelven diez filas DISTINTAS, las dos igual de válidas.
# El arnés las marcaba como DIFIERE en todas las corridas — cuatro fallos
# falsos fijos, que es la mejor forma de que nadie mire el quinto cuando sea
# de verdad.  Sacar el LIMIT además compara más: todas las filas, no diez.
canon() {
	local q="${1%;}"
	q=$(printf '%s' "$q" |
		sed -E 's/[[:space:]]+LIMIT[[:space:]]+[0-9]+([[:space:]]+OFFSET[[:space:]]+[0-9]+)?[[:space:]]*$//I')
	printf 'SELECT count(*), sum(hashtext(t.*::text)::bigint) FROM (%s) t;' "$q"
}

diffs=0
n=0
while IFS= read -r q; do
	n=$((n + 1))
	a=$($PSQL -At -c "$(canon "${q//FROM hits/FROM hits_heap}")" 2>&1)
	b=$($PSQL -At -c "$(canon "${q//FROM hits/FROM hits_dbengine}")" 2>&1)
	if [ "$a" != "$b" ]; then
		echo "  q$n: DIFIERE  (heap: $a / dbengine: $b)"
		diffs=$((diffs + 1))
	fi
done < "$HERE/queries.sql"
if [ "$diffs" -eq 0 ]; then
	echo "  las 43 coinciden"
else
	echo "  $diffs queries difieren — los tiempos de abajo no significan nada"
fi

echo
printf '%-4s %10s %10s %8s  %9s  %s\n' "q" "heap(ms)" "dbe(ms)" "ratio" "disp h/d" "plan dbengine"
n=0
while IFS= read -r q; do
	n=$((n + 1))
	for am in heap dbengine; do
		tbl="hits_$am"
		qq="${q//FROM hits/FROM $tbl}"
		# Una corrida de calentamiento que NO se mide.  Sin esto, la
		# primera vez que una query toca columnas anchas dbengine paga
		# I/O frío que heap ya pagó: cualquier seq scan de heap deja la
		# tabla entera en page cache, mientras que dbengine sólo cachea
		# las columnas que fue leyendo.  Medir sin calentar comparaba
		# heap caliente contra dbengine frío.
		$PGBIN/psql -X -q -At -c "$qq" >/dev/null 2>&1 || true

		# \timing mide lo que ve el cliente. Todas las queries devuelven
		# pocas filas (LIMIT o agregado), así que el transporte no pesa.
		# Se guardan TODAS las corridas, no solo la mejor: sin dispersión no
		# se puede distinguir una mejora de ruido.  Medido sobre este mismo
		# banco, el mismo heap con la misma query se movió 20% entre dos
		# corridas, y una query llegó a 1,8×.  Cualquier diferencia menor a
		# eso entre configuraciones no significa nada.
		todos=""
		for _ in $(seq "$RUNS"); do
			ms=$($PGBIN/psql -X -q -At -c "\\timing on" -c "$qq" 2>/dev/null \
				| grep -oE '^Time: [0-9.]+' | grep -oE '[0-9.]+' | head -1)
			[ -n "$ms" ] && todos="$todos $ms"
		done
		best=$(echo "$todos" | tr ' ' '\n' | grep -v '^$' \
			| awk 'NR==1{m=$1} $1<m{m=$1} END{if(NR)printf "%.1f", m}')
		spread=$(echo "$todos" | tr ' ' '\n' | grep -v '^$' \
			| awk 'NR==1{lo=hi=$1} $1<lo{lo=$1} $1>hi{hi=$1}
			       END{if(NR&&lo>0)printf "%.0f", 100*(hi-lo)/lo; else printf "0"}')
		if [ "$am" = heap ]; then heap_spread="$spread"; else dbe_spread="$spread"; fi
		if [ "$am" = heap ]; then heap_ms="${best:-}"; else dbe_ms="${best:-}"; fi
	done

	# ¿qué nodo usó dbengine?
	explain=$($PSQL -At -c "EXPLAIN (COSTS off) ${q//FROM hits/FROM hits_dbengine}" 2>/dev/null) || true
	case "$explain" in
		*DbengineVectorAgg*) plan="agg pushdown" ;;
		*DbengineColumnarScan*) plan="custom scan" ;;
		*"Seq Scan"*)        plan="seq scan (fallback)" ;;
		*)                   plan="?" ;;
	esac

	if [ -n "${heap_ms:-}" ] && [ -n "${dbe_ms:-}" ]; then
		# Ratio con la fórmula oficial de ClickBench: el offset de 10 ms
		# evita que una query de milisegundos domine la media geométrica.
		ratio=$(awk -v h="$heap_ms" -v d="$dbe_ms" \
			'BEGIN{printf "%.2f", (10+h)/(10+d)}')
		printf '%-4s %10.1f %10.1f %7sx  %3s%% %3s%%  %s\n' \
			"$n" "$heap_ms" "$dbe_ms" "$ratio" \
			"${heap_spread:-0}" "${dbe_spread:-0}" "$plan"
	else
		printf '%-4s %10s %10s %8s  %9s  %s\n' "$n" "${heap_ms:-error}" "${dbe_ms:-error}" "-" "" "$plan"
	fi
done < "$HERE/queries.sql"
