/*-------------------------------------------------------------------------
 *
 * zonemap.h
 *	  Descarte de stripes por zone map.
 *
 * Un zone map guarda el mínimo y el máximo de cada segmento.  Con eso, un
 * predicado de la forma `columna OP constante` alcanza para descartar
 * stripes enteros sin leer un solo byte de datos.
 *
 * El módulo lo usan los dos nodos: el Custom Scan y el de agregación.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_ZONEMAP_H
#define DBENGINE_ZONEMAP_H

#include "access/attnum.h"
#include "access/stratnum.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "dbengine/columnar.h"

/*
 * Un predicado simple `columna OP constante` evaluable contra el zone map
 * de un segmento.
 */
typedef struct DbeStripeFilter
{
	AttrNumber	attnum;
	Datum		value;			/* la constante del predicado */
	Oid			collation;
	int16		typlen;			/* del tipo de la COLUMNA */
	bool		typbyval;

	/*
	 * Las pruebas se arman con los operadores de la propia familia btree,
	 * no con la función de comparación del tipo.  Así funciona sin
	 * inventar nada cuando los dos lados son de tipos distintos —
	 * `bigint >= 100` compara int8 con int4, que es el caso normal, no un
	 * borde.
	 *
	 *   col <  v   posible si  min <  v
	 *   col <= v   posible si  min <= v
	 *   col >  v   posible si  max >  v
	 *   col >= v   posible si  max >= v
	 *   col =  v   posible si  min <= v  Y  max >= v
	 */
	bool		test_min;
	FmgrInfo	min_op;
	bool		test_max;
	FmgrInfo	max_op;

	/*
	 * Filtro de Bloom (F5).  Solo para `=`: es lo único que un filtro de
	 * pertenencia puede contestar.  `hash` es el del valor buscado, sacado
	 * con la función de hash que el operador de igualdad declara para SU
	 * lado — que es lo que hace que `id = 5000` (integer contra bigint)
	 * hashee igual que el bigint guardado.
	 */
	bool		test_bloom;
	uint64		hash;
} DbeStripeFilter;

typedef struct DbeFilterSet
{
	DbeStripeFilter *filters;
	int			nfilters;
} DbeFilterSet;

/*
 * ¿Se puede traducir la cláusula a una prueba sobre el zone map?  Se usa en
 * planning para quedarse solo con las que sirven.
 */
extern bool dbe_filter_usable(Expr *clause, Index relid);

/*
 * Igual que la anterior, pero además dice sobre qué columna y con qué
 * estrategia btree.  Lo usa el canal de feedback de F6.
 */
extern bool dbe_filter_column(Expr *clause, Index relid, AttrNumber *attnum,
							  StrategyNumber *strategy);

/*
 * Arma el conjunto en el contexto de memoria actual.  Las cláusulas que no
 * se pueden traducir se saltean: el descarte es una optimización, nunca
 * reemplaza a la evaluación del predicado.
 */
extern DbeFilterSet *dbe_filterset_build(List *clauses, Index relid,
										 TupleDesc desc);

/*
 * Columnas sobre las que el conjunto filtra.  El cursor lo usa para pedirle
 * al catálogo solo de esas los datos de zona caros —el filtro de Bloom y el
 * zone map por gránulo—, que son bytea grandes: traerlos siempre le
 * cobraría a todas las consultas para servirle a las pocas que filtran.
 * Devuelve NULL si el conjunto está vacío.
 */
extern bool *dbe_filterset_zone_columns(DbeFilterSet *fs, int natts);

/* Callback para dbe_cursor_set_filter(); arg es un DbeFilterSet *. */
extern bool dbe_filterset_passes(void *arg, const DbeSegDesc *segs,
								 const bool *found, int natts);

/*
 * Marca en `mask` los gránulos del stripe que podrían tener filas que pasen
 * el filtro; el resto se puede saltear entero.  `mask` es un bitmap de
 * ngranules bits, que el caller da en cero.
 *
 * Devuelve false si no se pudo descartar ninguno, y ahí no vale la pena ni
 * mirar la máscara: el caller recorre el stripe como siempre.
 */
extern bool dbe_filterset_granules(void *arg, const DbeSegDesc *segs,
								   const bool *found, int natts,
								   uint64 nrows, uint8 *mask);

#endif							/* DBENGINE_ZONEMAP_H */
