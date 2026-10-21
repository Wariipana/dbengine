#!/usr/bin/env bash
# Benchmark de F3 a escala: mide en CALIENTE (todo en cache, mide CPU) y en
# FRÍO (page cache soltado con posix_fadvise, mide I/O de verdad).
#
# Las tablas big_col / big_heap las tiene que haber cargado antes quien
# llame; a 100M filas eso lleva varios minutos y no conviene rehacerlo en
# cada corrida.
set -euo pipefail

PSQL="${PSQL:-psql}"
DB="${DB:-postgres}"
DATADIR="${PGDATA:-$HOME/.dbengine/pg18/data}"
HERE="$(cd "$(dirname "$0")" && pwd)"
Q() { $PSQL -d "$DB" -Aqt "$@"; }

frio() {  # $1 = tabla
    Q -c "CHECKPOINT;" >/dev/null
    "$HERE/evict.py" "$DATADIR" "$(Q -c "SELECT pg_relation_filepath('$1');")" >/dev/null
}

tiempo() { # $1 = query, $2 = SET previo
    $PSQL -d "$DB" -Aqt -c "$2" -c "\timing on" -c "$1" 2>&1 |
        grep '^Time' | sed 's/Time: //; s/ ms.*//'
}

mejor() { for _ in 1 2 3; do tiempo "$1" "$2"; done | sort -n | head -1; }

bloques() { # $1 = query, $2 = SET previo
    $PSQL -d "$DB" -Aqt -c "$2" \
      -c "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" |
      grep -oE "read=[0-9]+" | sed 's/read=//' |
      awk '{s+=$1} END {print s+0}'
}

echo "=== TAMAÑO ==="
Q -c "SELECT 'columnar: ' || pg_size_pretty(pg_relation_size('big_col')) ||
             '   heap: ' || pg_size_pretty(pg_relation_size('big_heap')) ||
             '   ratio: ' || round(pg_relation_size('big_heap')::numeric /
                                   pg_relation_size('big_col'), 2) || 'x';"
Q -c "SELECT 'stripes: ' || count(*) ||
             '   filas de catálogo: ' ||
             (SELECT count(*) FROM dbengine.stripe_column
              WHERE relfilenode = pg_relation_filenode('big_col'))
      FROM dbengine.stripe WHERE relfilenode = pg_relation_filenode('big_col');"

echo
echo "=== PLANNING (el costeo recorre el catálogo interno) ==="
for t in big_col big_heap; do
  p=$(Q -c "EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, BUFFERS OFF)
            SELECT count(*) FROM $t WHERE id BETWEEN 100 AND 200;" |
      grep -A1 "^Planning" | grep -oE "[0-9]+\.[0-9]+" | head -1)
  printf "  %-10s %s ms\n" "$t" "${p:-n/d}"
done

echo
echo "=== EN CALIENTE (mejor de 3; todo en cache, mide CPU) ==="
printf "%-52s %9s %9s %9s\n" "consulta" "F3" "F2" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT sum(monto) FROM %s" \
         "SELECT count(*) FROM %s WHERE id BETWEEN 100 AND 200" \
         "SELECT sum(id) FROM %s WHERE id > 99000000" \
         "SELECT count(*) FROM %s WHERE region = 'norte'"; do
  printf "%-52s %9s %9s %9s\n" "$(echo "$q" | cut -c1-52)" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=on")" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=off")" \
    "$(mejor "$(printf "$q" big_heap)" "SET dbengine.enable_vectorized=off")"
done

echo
echo "=== AGREGACIÓN EMPUJADA AL SCAN ==="
printf "%-52s %9s %9s %9s\n" "consulta" "con push" "sin push" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT min(id), max(id) FROM %s" \
         "SELECT sum(monto) FROM %s"; do
  printf "%-52s %9s %9s %9s\n" "$(echo "$q" | cut -c1-52)" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_agg_pushdown=on")" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_agg_pushdown=off")" \
    "$(mejor "$(printf "$q" big_heap)" "SET dbengine.enable_vectorized=off")"
done

echo
echo "=== AGREGACIÓN CON PREDICADO ==="
printf "%-52s %9s %9s %9s\n" "consulta" "con push" "sin push" "heap"
for q in "SELECT sum(monto) FROM %s WHERE region = 'norte'" \
         "SELECT count(*) FROM %s WHERE id > 99000000" \
         "SELECT sum(id) FROM %s WHERE id BETWEEN 100 AND 200" \
         "SELECT avg(monto) FROM %s WHERE id %% 2 = 0"; do
  printf "%-52s %9s %9s %9s\n" "$(echo "$q" | cut -c1-52)" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_agg_pushdown=on")" \
    "$(mejor "$(printf "$q" big_col)"  "SET dbengine.enable_agg_pushdown=off")" \
    "$(mejor "$(printf "$q" big_heap)" "SET dbengine.enable_vectorized=off")"
done

echo
echo "=== LATE MATERIALIZATION ==="
# Caso aislado: `region` se repite en TODOS los stripes, así que el zone map
# no descarta ninguno y el descarte no puede llevarse el crédito.  Pasa el
# 25% de las filas.  La columna cara es `nota`, que el predicado NO mira.
#
# La contraprueba fuerza a decodificarla siempre metiéndola en el predicado:
# `nota IS NOT NULL` no filtra nada, solo obliga a leerla.
printf "%-52s %9s\n" "caso" "ms"
printf "%-52s %9s\n" "SELECT * WHERE region='norte'  (con late mat.)" \
  "$(mejor "SELECT count(*) FROM (SELECT * FROM big_col WHERE region='norte') z" "SET dbengine.enable_vectorized=on")"
printf "%-52s %9s\n" "  ídem, forzando decodificar nota siempre" \
  "$(mejor "SELECT count(*) FROM (SELECT * FROM big_col WHERE region='norte' AND nota IS NOT NULL) z" "SET dbengine.enable_vectorized=on")"
printf "%-52s %9s\n" "  el mismo scan por el Table AM (sin Custom Scan)" \
  "$(mejor "SELECT count(*) FROM (SELECT * FROM big_col WHERE region='norte') z" "SET dbengine.enable_vectorized=off")"
printf "%-52s %9s\n" "  heap" \
  "$(mejor "SELECT count(*) FROM (SELECT * FROM big_heap WHERE region='norte') z" "SET dbengine.enable_vectorized=off")"

echo
echo "=== BLOQUES LEÍDOS ==="
printf "%-52s %9s %9s %9s\n" "consulta" "F3" "F2" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT sum(monto) FROM %s" \
         "SELECT count(*) FROM %s WHERE id BETWEEN 100 AND 200"; do
  frio big_col; a=$(bloques "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=on")
  frio big_col; b=$(bloques "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=off")
  frio big_heap; c=$(bloques "$(printf "$q" big_heap)" "SET dbengine.enable_vectorized=off")
  printf "%-52s %9s %9s %9s\n" "$(echo "$q" | cut -c1-52)" "$a" "$b" "$c"
done

echo
echo "=== EN FRÍO (page cache soltado antes de cada medición; mide I/O) ==="
printf "%-52s %9s %9s %9s\n" "consulta" "F3" "F2" "heap"
for q in "SELECT count(*) FROM %s" \
         "SELECT sum(monto) FROM %s" \
         "SELECT count(*) FROM %s WHERE id BETWEEN 100 AND 200"; do
  frio big_col;  a=$(tiempo "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=on")
  frio big_col;  b=$(tiempo "$(printf "$q" big_col)"  "SET dbengine.enable_vectorized=off")
  frio big_heap; c=$(tiempo "$(printf "$q" big_heap)" "SET dbengine.enable_vectorized=off")
  printf "%-52s %9s %9s %9s\n" "$(echo "$q" | cut -c1-52)" "$a" "$b" "$c"
done
