# ADR-004 — Dónde vive el predicado de un Custom Scan

**Estado:** aceptado (F3)

## El problema

Los dos nodos de F3 —`DbengineColumnarScan` y `DbengineVectorAgg`— necesitan
evaluar el `WHERE` ellos mismos, y no dejárselo al executor:

- el **scan** lo necesita para *late materialization*: evaluar el predicado
  con las columnas que mira y decodificar el resto de la fila solo si la
  pasa. Eso obliga a intercalar la evaluación entre dos pasos que ocurren
  adentro del nodo, donde `ExecScan()` no puede meterse;
- la **agregación** lo necesita porque no devuelve filas: consume las
  columnas y entrega un resultado. Si el predicado quedara arriba, no habría
  nada sobre lo cual aplicarlo.

Un `CustomScan` tiene tres lugares donde poner expresiones, y no son
intercambiables. La diferencia está en qué les hace `setrefs.c`.

## Lo que hace `setrefs.c` con cada uno

`set_customscan_references()` se bifurca según haya o no `custom_scan_tlist`:

| campo | con `custom_scan_tlist` | sin él (`scanrelid > 0`) |
|---|---|---|
| `plan.targetlist` | reescrito contra el tlist, `varno = INDEX_VAR` | `fix_scan_list`, `varno += rtoffset` |
| `plan.qual` | ídem | ídem |
| `custom_exprs` | ídem | ídem |
| `custom_private` | **intacto** | **intacto** |

Y `create_customscan_plan()`, además, corre `replace_nestloop_params()` sobre
`plan.qual` y `custom_exprs` —nunca sobre `custom_private`— cuando el path
está parametrizado.

## Decisión

**Cada nodo usa el campo que corresponde a su forma, y son distintos.**

### El scan: `custom_exprs`

`custom_scan_tlist` es `NIL` y `scanrelid > 0`, así que `custom_exprs` recibe
exactamente el mismo tratamiento que `scanrelid`: el `rtoffset` se aplica a
los dos, y siguen coincidiendo. Es también el único campo donde las
referencias laterales se convierten en `Param`.

`plan.qual` queda en `NIL` — el nodo evalúa y el `Filter:` del `EXPLAIN` lo
imprime el nodo, con el mismo texto que imprimiría `explain.c`.

Antes, los predicados podables para el zone map viajaban en `custom_private`.
Funcionaba solo mientras el `rtoffset` fuera 0: con más de un nivel de
subconsulta los `varno` dejaban de coincidir con `scanrelid`, `dbe_extract_filter()`
no reconocía ninguna cláusula y **el descarte de stripes se apagaba en
silencio**. No daba resultados incorrectos, que es peor de encontrar: solo
dejaba de andar.

### La agregación: `custom_private`

Acá `custom_scan_tlist` no es `NIL` —son los `Aggref` de salida— así que
`setrefs.c` reescribiría los `Var` del predicado contra ese tlist. No están
ahí: el predicado mira columnas de la tabla, no agregados. La reescritura no
fallaría en silencio, fallaría con "variable not found in subplan target
list".

Van entonces en `custom_private`, que nadie toca, con sus `Var` originales
(`varno` = relid de la tabla, `varattno` = número de columna). Se evalúan
contra un slot con el descriptor de la relación: `ExecInitExpr()` traduce
cualquier `varno` que no sea `INNER_VAR`/`OUTER_VAR` a `EEOP_SCAN_VAR`, que
lee `ecxt_scantuple` por `varattno` sin volver a mirar el `varno`.

El precio es que este nodo **no puede ser parametrizado**: sin
`replace_nestloop_params()`, una referencia lateral quedaría como `Var` y se
leería del slot equivocado. No es una pérdida: la agregación se planifica
sobre una relación base sola, sin outer del cual depender.

## Consecuencias

- El `EXPLAIN` de los dos nodos imprime el predicado por su cuenta. El del
  scan usa las mismas etiquetas que Postgres (`Filter`, `Rows Removed by
  Filter`) para que un plan se lea igual; el de la agregación usa `Filtro` y
  `Filas descartadas por el filtro`, que son propias del nodo.
- El descarte por zone map se deriva del mismo lugar en los dos casos: las
  cláusulas que `dbe_filter_usable()` sabe traducir a una comparación contra
  el min/max del segmento. Nunca reemplazan la evaluación del predicado.
- Un predicado con `Param` (path parametrizado) no aporta descarte de
  stripes: `dbe_extract_filter()` exige una `Const`. Es correcto y
  conservador — se leen los stripes y se evalúa igual.
