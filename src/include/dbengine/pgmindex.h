/*-------------------------------------------------------------------------
 *
 * pgmindex.h
 *	  Index AM `dbengine_pgm`: un índice aprendido sobre claves enteras.
 *
 * En vez de un árbol de páginas con punteros, guarda las claves ordenadas y
 * un MODELO —rectas por tramos— que predice dónde está una clave.  La
 * predicción tiene un error acotado por construcción, así que después
 * alcanza con mirar una ventana chica.  Ver src/engine/index/pgm.cpp.
 *
 * Layout en el archivo lógico (el mismo de storage.h, sobre el fork main):
 *
 *	 offset 0            DbePgmMeta                (una página, reescribible)
 *	 meta.model_off      modelo serializado
 *	 meta.keys_off       ndistinct claves int64, ordenadas
 *	 meta.starts_off     ndistinct+1 uint64: fila donde arranca cada corrida
 *	                     (se omite si no hay claves repetidas)
 *	 meta.rows_off       nrows uint64: números de fila, en orden de clave
 *	 meta.delta_head     cadena de DbePgmDeltaChunk con lo insertado después
 *
 * El modelo va sobre las claves DISTINTAS, no sobre las filas.  Con claves
 * repetidas, un modelo sobre las filas necesita una recta por cada corrida
 * —ninguna recta puede mandar mil filas a la misma clave— y termina siendo
 * más grande que los datos.  Sobre las distintas, mil filas iguales son un
 * punto.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_PGMINDEX_H
#define DBENGINE_PGMINDEX_H

#include "postgres.h"

#include "access/amapi.h"
#include "utils/rel.h"

#define DBE_PGM_MAGIC		0x4d47504b	/* "KPGM" */
#define DBE_PGM_VERSION		1

/* Error del modelo, en posiciones.  Ver la nota de dimensionamiento abajo. */
#define DBE_PGM_EPSILON		64

/* Entradas por bloque de la cadena delta. */
#define DBE_PGM_DELTA_ENTRIES	512

/*
 * `starts` guardado en uint32.  Son offsets dentro del arreglo de filas, así
 * que con menos de 2^32 filas indexadas entran de sobra, y son la mitad del
 * índice cuando hay claves repetidas.
 */
#define DBE_PGM_STARTS32		0x0001

typedef struct DbePgmMeta
{
	uint32		magic;
	uint32		version;
	uint32		epsilon;
	uint32		flags;
	uint64		nrows;			/* filas indexadas en el build */
	uint64		ndistinct;		/* claves distintas */
	uint64		model_off;
	uint64		model_len;
	uint64		keys_off;
	uint64		starts_off;		/* 0 si ndistinct == nrows */
	/* flags: DBE_PGM_STARTS32 */
	uint64		rows_off;
	uint64		delta_head;		/* 0 si no hay nada insertado después */
	uint64		delta_tail;
	uint64		delta_rows;
} DbePgmMeta;

typedef struct DbePgmDeltaChunk
{
	uint64		next;			/* offset del próximo bloque, 0 si es el último */
	uint32		used;
	uint32		reserved;
	/* le siguen `used` DbePgmDeltaEntry */
} DbePgmDeltaChunk;

typedef struct DbePgmDeltaEntry
{
	int64		key;
	uint64		rownum;
} DbePgmDeltaEntry;

extern void dbe_pgmindex_init(void);

/*
 * ¿Se puede mapear este tipo a int64 conservando el orden?  Es la
 * restricción real del método: el modelo interpola sobre números.
 */
extern bool dbe_pgm_type_supported(Oid typid);
extern int64 dbe_pgm_key_to_int64(Oid typid, Datum value);

#endif							/* DBENGINE_PGMINDEX_H */
