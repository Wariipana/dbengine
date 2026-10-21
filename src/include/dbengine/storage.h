/*-------------------------------------------------------------------------
 *
 * storage.h
 *	  Archivo lógico direccionable por bytes sobre el fork `main`.
 *
 * Ver doc/adr-001-storage-fisico.md.  Resumen: los segmentos por columna
 * son lógicos, no archivos separados; van al fork main a través del buffer
 * manager, así siguen andando DROP/TRUNCATE, recovery, tablespaces y las
 * vistas de tamaño.
 *
 * Layout:
 *	 bloque 0        página de metadatos (DbeStorageMeta)
 *	 bloques 1..N    bytes crudos en el área utilizable de páginas PageInit
 *
 * Los stripes empiezan siempre en borde de página.  Eso desperdicia hasta
 * 8 kB por stripe (despreciable con ~65k filas por stripe) y a cambio cada
 * página de datos se escribe UNA sola vez: no hay read-modify-write, el
 * WAL es un log_newpage_buffer() limpio y dos backends nunca tocan la misma
 * página de datos.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_STORAGE_H
#define DBENGINE_STORAGE_H

#include "postgres.h"

#include "storage/bufpage.h"
#include "utils/rel.h"

#define DBE_STORAGE_MAGIC		0x44424553	/* "DBES" */
#define DBE_STORAGE_VERSION		1

#define DBE_META_BLOCK			0
#define DBE_FIRST_DATA_BLOCK	1

/* Bytes de datos que entran en una página. */
#define DBE_PAGE_USABLE			((uint64) (BLCKSZ - SizeOfPageHeaderData))

/*
 * Metadatos que viven en el bloque 0.  Se actualizan bajo lock exclusivo
 * de ese bloque, que es lo que serializa la reserva de espacio.
 */
typedef struct DbeStorageMeta
{
	uint32		magic;
	uint32		version;
	uint64		next_offset;	/* próximo byte lógico libre */
	uint64		next_stripe_id;
	uint64		next_row_number;
} DbeStorageMeta;

/* Crea el bloque 0 si la relación todavía no tiene storage inicializado. */
extern void dbe_storage_init(Relation rel);

/* Lee los metadatos.  Devuelve false si la relación está vacía (0 bloques). */
extern bool dbe_storage_read_meta(Relation rel, DbeStorageMeta *out);

/*
 * Reserva `nbytes` alineados a página.  Devuelve el offset lógico donde
 * escribir y, si stripe_id_out no es NULL, el id del stripe.
 */
extern uint64 dbe_storage_reserve_bytes(Relation rel, uint64 nbytes,
										uint64 *stripe_id_out);

/*
 * Reserva `nrows` números de fila consecutivos.
 *
 * Se reservan al ABRIR el buffer de escritura, no al flushearlo: el TID de
 * una fila tiene que ser definitivo apenas se inserta, porque el executor
 * lo usa para armar las entradas de índice.  Los números que no se llegan
 * a usar quedan como huecos, y eso está bien: un TID solo tiene que ser
 * único y estable.
 */
extern uint64 dbe_storage_reserve_rows(Relation rel, uint64 nrows);

/*
 * Devuelve la cola no usada de una reserva, si nadie reservó después.
 * Mantiene denso el espacio de números de fila, que es lo que va a
 * permitir fusionar stripes contiguos sin mover TIDs.
 */
extern void dbe_storage_release_rows(Relation rel, uint64 base,
									 uint64 reserved, uint64 used);

/* Escribe/lee bytes en el archivo lógico. */
extern void dbe_storage_write(Relation rel, uint64 offset,
							  const char *data, uint64 len);
extern void dbe_storage_read(Relation rel, uint64 offset,
							 char *dest, uint64 len);

#endif							/* DBENGINE_STORAGE_H */
