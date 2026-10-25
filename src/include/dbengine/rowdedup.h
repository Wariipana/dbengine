/*-------------------------------------------------------------------------
 *
 * rowdedup.h
 *	  Filas repetidas descartadas dentro de un stripe.
 *
 * Debajo de un GROUP BY cuyos agregados ignoran los duplicados —
 * count(DISTINCT), min, max— dos filas iguales en todas las columnas que
 * suben aportan lo mismo que una.  En ClickBench, q9 manda 10M filas
 * (RegionID, UserID) a un Sort, y distintas hay 1,5M: el Sort de 10M filas
 * que vuelca a disco es el 80% de la query.
 *
 * El descarte es exacto dentro de cada stripe y nada más: la tabla se vacía
 * al cambiar de stripe, así que la memoria queda acotada por las filas de un
 * stripe y no por la tabla.  Un duplicado que cruza stripes sube igual, y
 * lo resuelve el DISTINCT de arriba, que es el que da la semántica.
 *
 * Una columna entra en la clave por su Datum si es por valor, o por su
 * código si es de texto con diccionario en ese stripe.  Si en un stripe
 * alguna no es ninguna de las dos, ese stripe pasa entero.  Dos filas se
 * descartan solo si son binariamente iguales, que implica iguales para
 * cualquier operador de igualdad: en la duda, la fila sube.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_ROWDEDUP_H
#define DBENGINE_ROWDEDUP_H

#include "postgres.h"

#include "executor/tuptable.h"

#include "dbengine/columnar.h"

/* Más columnas que esto no se deduplican: ver dbe_dedup_create(). */
#define DBE_DEDUP_MAX_COLS 8

typedef struct DbeRowDedup DbeRowDedup;

/* `attnums`: lista de enteros, las columnas que forman la clave. */
extern DbeRowDedup *dbe_dedup_create(TupleDesc desc, List *attnums);

/*
 * ¿Ya pasó en este stripe una fila igual a la del slot?  Si no, la anota.
 * El slot tiene que tener cargadas las columnas de la clave, y `cur` estar
 * parado en esa fila.
 */
extern bool dbe_dedup_repeated(DbeRowDedup *dd, DbeRowCursor *cur,
							   TupleTableSlot *slot);

/* Olvida el stripe actual: para un rescan. */
extern void dbe_dedup_reset(DbeRowDedup *dd);

#endif							/* DBENGINE_ROWDEDUP_H */
