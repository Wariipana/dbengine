# ADR-003 — El block pruning no se puede cablear en F2

**Estado:** aceptado, F2 — implementado en F3

## El problema

F2 pide *"block pruning en el scan usando esas estadísticas"*. No se puede
hacer desde el Table AM: **el seq scan nunca recibe los predicados.**

`scan_begin()` tiene parámetros `nkeys`/`key`, pero el executor los deja en
cero para un seq scan — las condiciones viven en el nodo del plan y se
evalúan con `ExecQual()` *después* de que el AM devolvió la fila. Los
`ScanKey` los usan los index scans y los scans de catálogo, no `SeqScan`.

Es **exactamente el mismo límite que R4** identifica para la vectorización,
pero aplicado al filtrado en vez de a la ejecución. Y es el mismo que ya
apareció en F1 con el column pruning.

## Decisión

Separar las dos mitades:

- **F2 construye y guarda los zone maps**: min, max, nulls y filas por
  segmento, en `dbengine.stripe_column`. Es la estructura que además
  necesitan F5 (zone maps aprendidos, que se miden contra estos) y F6
  (feedback de clustering).
- **F3 los usa**, cuando el Custom Scan sí conoce los predicados. Ahí entra
  también el column pruning que quedó pendiente de F1.

Con lo cual la ganancia medible de F2 es la **compresión**, que es
íntegramente del lado del storage y no depende del planner.

## Consecuencia

El benchmark obligatorio de F2 se mide sobre compresión y tiempo de scan.
El de pruning se mide en F3, y es contra ese número que F5 tiene que
justificar los zone maps aprendidos.

Los zone maps de F2 no quedan sin verificar: se exponen por SQL
(`dbengine.stripe_column`) y hay tests que comprueban que los min/max son
correctos, incluyendo nulos y stripes de un solo valor.


---

## Cómo quedó (F3)

El Custom Scan aplica el filtro **antes de tocar el storage**: los
descriptores de segmento ya traen el zone map, así que descartar un stripe
cuesta lo que ya se gastó leyendo el catálogo y ahorra toda la lectura.

```
col <  v   posible si  min <  v
col <= v   posible si  min <= v
col >  v   posible si  max >  v
col >= v   posible si  max >= v
col =  v   posible si  min <= v  Y  max >= v
```

Las comparaciones usan los operadores de la **familia btree del tipo de la
columna**, con la firma `(tipo_columna, tipo_constante)`. Eso importa más de
lo que parece: `id >= 100` sobre un `bigint` compara int8 con int4, y es el
caso normal. La primera versión exigía tipos iguales y no podaba nada.

**Es conservador por construcción.** Solo devuelve "descartar" cuando el
rango excluye con certeza; sin zone map, con un tipo sin orden btree o con
un operador que no está en la familia, se lee el stripe. Y las cláusulas
siguen en `plan.qual`: descartar un stripe evita leer, no reemplaza la
evaluación del predicado. `test/regress/sql/columnar_scan.sql` compara 16
consultas con y sin el Custom Scan.
