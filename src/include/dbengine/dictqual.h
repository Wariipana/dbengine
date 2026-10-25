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
 * Lo usan el Custom Scan y el pushdown de agregados.  Las cláusulas que no
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

/* Olvida los resultados: en un rescan pueden haber cambiado los Params. */
extern void dbe_dictqual_reset(DbeDictQual *dq);

#endif							/* DBENGINE_DICTQUAL_H */
