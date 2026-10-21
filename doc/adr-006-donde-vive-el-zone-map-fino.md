# ADR-006 — El zone map por gránulo va al catálogo, no al segmento

**Estado:** aceptado

## El problema

La medición de stripes chicos (ver README, "Stripes más chicos") dejó un
diagnóstico claro: **la unidad de escritura y la unidad de descarte no tienen
por qué ser la misma**. Bajar el stripe de 65.536 a 8.192 filas mejora el
descarte apenas —92,8% a 93,8%— y cuesta entre 20% y 33% de tiempo, porque
ocho veces más stripes son ocho veces más filas de catálogo, más aperturas de
segmento y peor compresión. Un scan completo cae a 0,41×.

ORC, Parquet y ClickHouse resuelven lo mismo separando las dos unidades:
gránulos de 8.192 filas dentro de un *part*, row-groups dentro de un stripe,
page indexes dentro de un row group. Faltaba hacer eso acá.

La pregunta de este ADR no es si conviene —está medido que sí, 1,2× a 1,4×
sobre predicados selectivos y 0,99× a 1,01× donde no aplica— sino **dónde se
guardan** los min/max de cada gránulo.

## Las opciones

**(a) Adentro del segmento**, en un bloque nuevo del formato serializado.
Es donde lo pondría un formato de archivo: el segmento es autocontenido y el
bloque viaja con los datos que describe.

**(b) En el catálogo**, como una columna más de `dbengine.stripe_column`,
al lado del zone map de stripe y del filtro de Bloom.

## Decisión

**(b), el catálogo.** Dos razones, y las dos son restricciones reales del
diseño, no preferencias:

### El motor no conoce los tipos

El min/max no se puede calcular comparando bytes. Se calcula con el operador
de comparación de la familia btree del tipo —`FunctionCall2Coll(&zm->cmp, ...)`
en `columnar_write.c`—, que es conocimiento de tipos de Postgres. El motor C++
sólo ve "valores de ancho fijo de W bytes" o "valores de largo variable"; no
tiene forma de decidir si un `date` es menor que otro, ni de saber que un
`float8` compara distinto que un `int8` con los mismos bytes.

Es la misma razón por la que el zone map de stripe vive en la capa C y no en
el motor (ver el comentario en `columnar_write.c`). Meterlo en el segmento
habría obligado a una de dos cosas feas: cruzar el airlock una vez por valor
para comparar, o enseñarle tipos al motor.

### No rompe las tablas que ya existen

El bloque es una columna nueva que queda en `NULL` para todo lo escrito antes.
Una tabla vieja simplemente no descarta por gránulo hasta que se reescriba.
Meterlo en el segmento habría cambiado el formato serializado y obligado a
reescribir todo, o a mantener dos versiones del layout.

## Consecuencias

- **Se paga catálogo.** 776 kB para una tabla de 1,8 GB con 153 stripes y 105
  columnas: 0,04%. Medido, no estimado.
- **Se carga con las mismas reglas que el filtro de Bloom**: sólo para las
  columnas sobre las que la query filtra. Traerlo siempre le cobraría a todas
  las consultas para servirle a las pocas que filtran. Por eso
  `dbe_filterset_bloom_columns()` pasó a llamarse
  `dbe_filterset_zone_columns()` y ahora marca todas las columnas con filtro,
  no sólo las de igualdad.
- **Sólo tipos por valor de 1, 2, 4 u 8 bytes.** Un min/max de largo variable
  por gránulo multiplicaría el catálogo por la cantidad de gránulos, y son
  justo los tipos donde el descarte por rango sirve menos: sobre `RefererHash`
  y `URLHash` el descarte medido es 0%.
- **No ahorra I/O**, y eso está confirmado por la medición: el número en frío
  (1,37×–1,41×) es casi igual al de caliente (1,39×–1,43×). El segmento se lee
  entero; lo que se saltea es decodificar y evaluar.

## Lo que este ADR NO decide

El tamaño del gránulo. `DBE_GRANULE_ROWS` es 8.192 porque es lo que se midió y
lo que usa ClickHouse por default, no porque esté optimizado. Hacerlo
configurable es barato —el bloque ya guarda `granule_rows` en su header— y no
se hizo porque no hay medición que lo justifique todavía.
