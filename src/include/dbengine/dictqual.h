/*-------------------------------------------------------------------------
 *
 * dictqual.h
 *	  Predicados evaluados una vez por entrada del diccionario.
 *
 * En un segmento con diccionario, las filas no tienen valores: tienen
 * códigos, y cada código aparece muchas veces.  En ClickBench una URL se
 * repite ~5 veces por stripe y un Referer ~3.  Una cláusula que mira una
 * sola columna da el mismo resultado para el mismo valor, así que alcanza
 * con evaluarla la primera vez que aparece cada código y recordarlo.
 *
 * Lo usan el Custom Scan y el pushdown de agregados, y el Custom Scan
 * también para las expresiones de su salida.  Las cláusulas que no
 * califican —varias columnas, funciones volátiles, subplans— y las que caen
 * en un segmento sin diccionario se evalúan como siempre.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_DICTQUAL_H
#define DBENGINE_DICTQUAL_H

#include "postgres.h"

#include "nodes/execnodes.h"

#include "dbengine/columnar.h"

typedef struct DbeDictQual DbeDictQual;

/*
 * Compila `quals` (lista implícita de AND).  Con parent NULL se compila
 * como ExecPrepareQual(), para un nodo que evalúa contra un slot propio.
 * Devuelve NULL si no hay cláusulas.
 */
extern DbeDictQual *dbe_dictqual_build(List *quals, Index varno,
									   TupleDesc desc, PlanState *parent,
									   EState *estate);

/*
 * Columnas perezosas: las que solo miran cláusulas de diccionario.  Con el
 * resultado ya calculado para un código no hace falta ni leer el valor, así
 * que quien llama no las carga en el slot antes de evaluar: dbe_dictqual_eval
 * pide cada una con `fill` recién cuando tiene que evaluar de verdad.
 */
typedef void (*DbeDictFillFn) (void *arg, AttrNumber attnum);

extern bool dbe_dictqual_lazy_column(DbeDictQual *dq, AttrNumber attnum);

/*
 * ¿Pasa la fila `row` del stripe cargado en `cur`?  El slot de
 * econtext->ecxt_scantuple ya tiene que tener las columnas de los quals,
 * salvo las perezosas.
 */
extern bool dbe_dictqual_eval(DbeDictQual *dq, DbeRowCursor *cur, uint64 row,
							  ExprContext *econtext,
							  DbeDictFillFn fill, void *fill_arg);

/*
 * Para quien recorre un stripe entero: lee los códigos de todas sus filas de
 * una vez y deja en sel[r], para cada una de las `nrows`:
 *
 *   DBE_SEL_FAIL   no pasa alguna cláusula de diccionario
 *   DBE_SEL_PASS   pasa el WHERE entero
 *   DBE_SEL_CHECK  hay que evaluarla con dbe_dictqual_eval(): quedan
 *                  cláusulas que no son de diccionario, o el segmento de
 *                  alguna no tiene diccionario en este stripe
 *
 * econtext->ecxt_scantuple tiene que ser un slot virtual de la relación;
 * se usa para evaluar las cláusulas sobre los valores del diccionario y
 * queda como estaba.  Devuelve false si ninguna cláusula se pudo decidir
 * por código: todas las filas quedan para evaluar.
 */
#define DBE_SEL_FAIL	0
#define DBE_SEL_PASS	1
#define DBE_SEL_CHECK	2

extern bool dbe_dictqual_prefilter(DbeDictQual *dq, DbeRowCursor *cur,
								   uint32 nrows, uint8 *sel,
								   ExprContext *econtext);

/* Olvida los resultados: en un rescan pueden haber cambiado los Params. */
extern void dbe_dictqual_reset(DbeDictQual *dq);

/*
 * Lo mismo para una expresión de la salida: `REGEXP_REPLACE(Referer, ...)`
 * se calcula una vez por entrada del diccionario y el valor se reusa en las
 * demás filas con el mismo código.  NULL si la expresión no califica (mira
 * más de una columna, es volátil, o es la columna sola).
 */
typedef struct DbeDictExpr DbeDictExpr;

extern DbeDictExpr *dbe_dictexpr_build(Expr *expr, Index varno,
									   TupleDesc desc, PlanState *parent);

/*
 * Valor de la expresión para la fila `row`.  El slot de
 * econtext->ecxt_scantuple tiene que tener la columna cargada.  El valor
 * vale hasta que se cambie de stripe.
 */
extern Datum dbe_dictexpr_eval(DbeDictExpr *de, DbeRowCursor *cur, uint64 row,
							   ExprContext *econtext, bool *isnull);
extern void dbe_dictexpr_reset(DbeDictExpr *de);

#endif							/* DBENGINE_DICTQUAL_H */
