# ADR-005 — En un índice aprendido, el modelo nunca decide

**Estado:** aceptado (F4)

## El problema

Un PGM-Index reemplaza el árbol de un btree por rectas: para una clave `k`,
`pos ≈ a·(k − k₀) + b`. La construcción garantiza que el error nunca supera
ε posiciones, así que después de predecir alcanza con mirar una ventana de
2ε+1 elementos.

Esa garantía es real, pero es **más chica de lo que parece**. Vale para las
claves que se usaron para construir el modelo. Para una clave que cae entre
dos de ellas —o sea, para casi cualquier consulta— hay que razonar con
monotonía y bordes de tramo, y ahí es fácil equivocarse por uno. Un índice
que se equivoca por uno no devuelve resultados lentos: devuelve resultados
**incorrectos**.

## Decisión

**La ventana del modelo es una pista. Antes de usarla se verifica, con
comparaciones exactas contra las claves reales, si la respuesta puede estar
afuera; si puede, se hace la binaria sobre el arreglo entero.**

```c
locate(k) -> [lo, hi)

si (lo > 0    y  claves[lo-1] >= k)  -> binaria completa
si (hi < n    y  claves[hi]    <  k) -> binaria completa
si no                                -> binaria dentro de [lo, hi]
```

Las dos condiciones son exactas y cuestan dos lecturas de clave. Si se
cumplen, el `lower_bound` global está dentro de la ventana y buscar ahí da
la respuesta correcta. Si no, la binaria completa la da igual.

La consecuencia es la que importa: **el resultado es correcto ande bien o
mal el modelo.** Un modelo degradado —por datos que cambiaron de forma, por
un bug de construcción, por una versión vieja en disco— cuesta tiempo, nunca
corrección. Lo mismo se aplica en cada nivel del descenso, no solo al final.

Es la misma regla que el plan fija para los zone maps aprendidos de F5, y
por la misma razón: un modelo es una heurística sobre los datos, y los datos
cambian.

## Lo que costó no tenerla

El modelo se probó primero contra una referencia exacta
(`std::lower_bound`) sobre doce distribuciones. Con la ventana como
autoridad fallaban cuatro:

| caso | fallos | causa |
|---|---|---|
| lognormal, claves hasta 9·10¹⁷ | 85.339 / 500.000 | las claves se convertían a `double` **antes** de restarlas; arriba de 2^53 se pierden los bits bajos y el error de la recta se vuelve de cientos de posiciones |
| uniforme con ε=4 | 143 / 200.000 | ventana corta por uno en el descenso entre niveles: la clave consultada no es un punto de construcción del nivel de arriba |
| 1000 duplicados por clave | 500.001 / 500.000 | ninguna recta puede mandar mil filas a la misma clave |
| todas las claves iguales | 100.000 / 100.000 | ídem, y el cono ni siquiera cerraba el tramo |

Los dos primeros son bugs y se arreglaron (la resta va en `__int128`, la
ventana se ensanchó en uno). Los dos últimos **no son bugs**: son el límite
del método, y llevaron a cambiar qué se modela.

## Corolario: el modelo va sobre las claves distintas

Con duplicados, un modelo sobre `(clave, fila)` necesita una recta por
corrida y termina más grande que los datos. Medido: 500 claves distintas en
500.000 filas daban **7.003 tramos, 168 kB de modelo y respaldo en el 96% de
las consultas**.

El modelo va entonces sobre `(clave distinta, rango)`. Mil filas iguales son
un punto. El mismo caso pasa a **1 tramo, 80 bytes y 0% de respaldo**. El
índice guarda:

- las claves distintas, ordenadas;
- dónde arranca la corrida de cada una (se omite si no hay repetidas, y va
  en `uint32` mientras las filas entren);
- los números de fila, en orden de clave.

Con eso el respaldo cae a entre 0% y 2,7% en las distribuciones medidas, y
el índice queda más chico que el btree equivalente en las tres del
benchmark.

## La misma regla, aplicada al revés, en F5

El PGM aproxima una función monótona: ahí un modelo es la herramienta
correcta, y lo que hace falta es no dejarlo decidir. F5 planteaba un modelo
por bloque para descartar stripes, y la medición mostró que el único hueco
que dejan los zone maps min/max es el predicado de igualdad sobre una
columna de alta cardinalidad sin correlación con el orden de inserción.

Eso **no** es aproximar una función: es responder si una clave está entre
las 65.536 del stripe. Pertenencia a un conjunto. Ninguna regresión ni red
chica lo hace —no hay frontera que separe 65.536 enteros arbitrarios del
resto del dominio— y una lo bastante grande para memorizarlos sería una
tabla de hash cara. Lo que corresponde es un filtro de Bloom, que además
tiene la propiedad que acá vale más que la precisión: **error de un solo
lado**. No hay que disciplinarlo para que no sea autoridad; no puede serlo.

La regla, entonces, es más precisa que "verificá siempre": **elegí la
estructura por la forma de la pregunta**. Si la pregunta es "¿dónde está,
más o menos?", un modelo con verificación. Si es "¿está?", un filtro con
error de un solo lado. Confundirlas es lo que hace que un índice aprendido
parezca mágico en un paper y devuelva resultados incorrectos en producción.
