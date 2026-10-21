/*-------------------------------------------------------------------------
 *
 * workload.h
 *	  Canal de feedback: qué predicados ve el planner, por columna.
 *
 * Es la primera pieza de F6.  La compactación necesita saber POR CUÁL
 * columna conviene reordenar, y la única parte del sistema que ve los
 * predicados es el hook del planner.  Acá se anotan, en memoria compartida,
 * para que después alguien más los lea.
 *
 * Lo que se guarda es deliberadamente poco: por (relfilenode, columna), la
 * cuenta de predicados de rango y de igualdad, y cuándo se vio el último.
 * No se guardan los VALORES de los rangos: eso es lo que haría falta para
 * decidir el orden fino dentro de una columna, y todavía no hay quién lo
 * use.  Agregar un histograma acá es fácil; sacarlo, no.
 *
 * La tabla es de tamaño fijo y sin locks pesados: es estadística, no
 * contabilidad.  Si se llena, las entradas nuevas se descartan; si dos
 * backends anotan a la vez, se pierde alguna cuenta.  Ninguna de las dos
 * cosas cambia un resultado.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_WORKLOAD_H
#define DBENGINE_WORKLOAD_H

#include "postgres.h"

#include "common/relpath.h"
#include "access/attnum.h"

/* Entradas de la tabla.  Fijo: vive en memoria compartida. */
#define DBE_WORKLOAD_ENTRIES	1024

typedef struct DbeWorkloadKey
{
	Oid			dbid;
	RelFileNumber relnumber;
	AttrNumber	attnum;
} DbeWorkloadKey;

typedef struct DbeWorkloadEntry
{
	DbeWorkloadKey key;
	uint64		range_preds;	/* <, <=, >, >= */
	uint64		eq_preds;		/* = */

	/*
	 * Columnas por las que se AGRUPA.  Aparte de eq_preds a propósito:
	 * agrupar y filtrar por igualdad quieren la misma reorganización
	 * física, pero un filtro de Bloom solo sirve para el filtro.  Mezclarlas
	 * hacía construir filtros sobre columnas de texto anchas por las que
	 * nadie filtra nunca.
	 */
	uint64		group_refs;
	TimestampTz last_seen;
} DbeWorkloadEntry;

extern void dbe_workload_init(void);
extern void dbe_workload_shmem_request(void);
extern void dbe_workload_shmem_startup(void);

/*
 * Anota un predicado observado.  `is_equality` distingue `=` de un rango:
 * la compactación los usa distinto — la igualdad se beneficia de agrupar,
 * el rango de ordenar.
 */
extern void dbe_workload_note(RelFileNumber relnumber, AttrNumber attnum,
							  bool is_equality);

/*
 * ¿Vale la pena construir un filtro de Bloom para esta columna?
 *
 * La respuesta sale del workload: solo si el planner vio predicados de
 * igualdad sobre ella.  Es lo que evita pagar el filtro en todas las
 * columnas de todas las tablas para servirle a unas pocas consultas.
 */
extern void dbe_workload_note_group(RelFileNumber relnumber, AttrNumber attnum);
extern void dbe_workload_carry(RelFileNumber from, RelFileNumber to);
extern bool dbe_workload_wants_bloom(RelFileNumber relnumber, AttrNumber attnum);

/* Copia el contenido actual; el caller libera con pfree(). */
extern int	dbe_workload_snapshot(DbeWorkloadEntry **out);

/* Borra todo.  Lo usa el test y el que quiera empezar de cero. */
extern void dbe_workload_reset(void);

#endif							/* DBENGINE_WORKLOAD_H */
