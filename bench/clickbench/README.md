# ClickBench sobre dbengine

DDL (`create.sql`) y las 43 queries (`queries.sql`) oficiales, bajados de
`ClickHouse/ClickBench` y **sin modificar**. El runner las corre sobre dos
tablas con los mismos datos, una `USING heap` y otra `USING dbengine_columnar`.

## Los números no son un score de ClickBench

El dataset oficial son 100M filas (~70 GB descomprimidos). Acá se usan las
**primeras 10M filas del `hits.tsv` real**, que es lo que entra en el disco de
desarrollo. Comparar esto contra la tabla pública de ClickBench no significa
nada: sirve para comparar dbengine contra heap sobre el mismo dato.

## Correr

```sh
# 1. los datos (~1,7 GB de red, 7 GB en disco)
mkdir -p /config/.dbengine/clickbench && cd /config/.dbengine/clickbench
curl -s https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz \
  | gunzip | head -n 10000000 > hits10m.tsv

# 2. el benchmark (~6 min de carga + ~35 min de queries)
bench/clickbench/run.sh /config/.dbengine/clickbench/hits10m.tsv
```

`SKIP_LOAD=1` reusa las tablas ya cargadas. `RUNS=n` cambia cuántas veces se
mide cada query (default 3, se reporta la mejor).

El TSV se corta con `head -n`, que es seguro porque en el formato text de
`COPY` los saltos de línea dentro de un campo van escapados como `\n`.

## Cómo mide

Cada query corre **una vez sin medirse** antes de las que sí se miden. Sin
ese calentamiento la comparación está sesgada a favor de heap: cualquier seq
scan deja la tabla entera en page cache, así que heap llega caliente a todas
las queries desde la primera, mientras que dbengine sólo cachea las columnas
que fue tocando. Medir sin calentar comparaba heap caliente contra dbengine
frío, y daba 0,13× donde el número real es 0,75×.

Los tiempos son con page cache **caliente**, y conviene decir qué significa
eso: es el régimen donde la compresión no rinde, porque nadie lee de disco.
Es el menos favorable a la ventaja estructural de un column store, y
ClickBench oficial le da al tiempo en frío un 20% de su score combinado.

Que no se pueda vaciar el page cache entero sin root **no** impedía medir en
frío: `bench/evict.py` suelta las páginas de una relación con
`posix_fadvise(DONTNEED)`, que es exactamente lo que hace falta. Esa
herramienta estaba escrita desde F3 y no se usó acá.

## Correctitud antes que tiempos

El runner compara las 43 queries en las dos tablas antes de reportar nada.
**Coinciden las 43.**

La comprobación estuvo mal hecha bastante tiempo. Ordenaba la salida de cada
query y comparaba las filas devueltas, y con eso marcaba cuatro o cinco
fallos fijos en cada corrida: q22, q32, q33, q39 y q41 son
`ORDER BY count(*) DESC LIMIT 10` sobre miles de grupos empatados —en q33 los
10M grupos tienen `count(*) = 1`, en q32 empatan 1.374.133—, así que los dos
motores devuelven **diez filas distintas y las dos correctas**.

El razonamiento de entonces era "prefiero descartar a mano un empate que
perderme una diferencia real". Suena prudente y es al revés: cuatro fallos
falsos permanentes son la mejor forma de que nadie mire el quinto cuando sea
de verdad. Y pasó — cuando apareció q22, el reflejo fue asumir que era otro
empate.

Ahora se compara el resultado **completo**, sin el `LIMIT`, con un agregado
que no depende del orden:

```sql
SELECT count(*), sum(hashtext(t.*::text)::bigint) FROM (<query sin LIMIT>) t;
```

Es a la vez más estricto —todas las filas, no diez— y ciego al orden.
Verificado que sigue detectando una diferencia real: sacar **una fila de diez
millones** cambia la suma.

## En frío

`cold2.sh` corre las 43 —o el subconjunto que se le pase— soltando el page
cache antes de **cada** corrida, y reporta la mejor de tres más la dispersión
entre ellas.

Suelta la tabla, sus índices **y el catálogo de dbengine**
(`dbengine.stripe`, `dbengine.stripe_column`). Lo último se agregó tarde,
sospechando que era la causa de que la dispersión de dbengine llegara a 662%
mientras la de heap se quedaba en 0–16%. **No lo era**: los números casi no
se movieron. La dispersión alta está en las queries donde dbengine tarda
2–400 ms, donde unos pocos ms de ruido son un porcentaje enorme; donde tarda
de verdad —q24 con 19 s, q29 con 21 s— la dispersión es 0%.

Por eso el resumen reporta también una **cota conservadora**: la mejor
corrida de heap contra la **peor** de dbengine. Baja de 13,97× a 10,11× y
pasa de 39/4 a 38/5; ninguna conclusión cambia.

El desalojo del catálogo queda igual, porque en frío corresponde que esté
frío — sólo que no era lo que explicaba la dispersión.

## Resultados

| archivo | heap | dbengine | media geom |
|---|---|---|---|
| `resultado-10m.txt` | sin índices | PGM, sin clusterizar | 1,73× (suma) |
| `resultado-10m-heap-indexado.txt` | 6 índices | PGM, sin clusterizar | 1,49× (suma) |
| `resultado-10m-clusterizado.txt` | 6 índices | PGM + clusterizada + Bloom | 1,46× (suma) |
| `resultado-10m-final.txt` | 6 índices | + FSST + gránulos + Bloom | **2,06× caliente / 13,97× frío** |
| `resultado-10m-linea-base-c2.txt` | sin índices | sin índices ni Bloom (línea base de C2) | 3,97× media geom. / 1,83× suma |
| `resultado-10m-c2.txt` | sin índices | sin índices ni Bloom, después de C2 (`81d5a38`) | **11,80× media geom. / 5,37× suma** |

Las tres primeras son anteriores a FSST, al zone map por gránulo y al arreglo
del fetch por TID, y reportan la suma de tiempos, no la media geométrica.
Se conservan porque documentan cómo se llegó acá, no como resultado vigente.

**Sobre `resultado-10m-clusterizado.txt` en particular**: esa corrida
clusterizaba las **dos** tablas, así que comparaba dbengine-clusterizado
contra heap-clusterizado y nunca midió el efecto del clustering dentro de
dbengine. Medido bien —dos tablas columnares idénticas salvo el orden—
clusterizar da 455× en q18 y 38× en q20, y hunde q43 a 0,17×. Es una
redistribución, no una mejora; el detalle está en el README principal.

Los índices de heap son btree en `(CounterID, EventDate)`, `UserID`,
`URLHash`, `RefererHash`, `AdvEngineID` y BRIN en `EventTime` — uno por cada
filtro selectivo de las 43. Los de dbengine son PGM sobre `UserID`,
`URLHash`, `RefererHash` y `CounterID`; para clusterizar hace falta además un
btree, porque `CLUSTER` necesita un índice ordenado y el PGM declara
`amcanorder = false`.

El resumen está en el README principal, en la sección ClickBench.
