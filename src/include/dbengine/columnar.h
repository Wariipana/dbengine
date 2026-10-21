/*-------------------------------------------------------------------------
 *
 * columnar.h
 *	  Storage columnar: buffer de escritura (R8) y cursor de lectura.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_COLUMNAR_H
#define DBENGINE_COLUMNAR_H

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "dbengine/bridge.h"
#include "dbengine/catalog.h"

/*
 * TIDs.  El número de fila es lógico y monótono por relación; el TID se
 * deriva de él.  No hay relación con el bloque físico, y no hace falta:
 * un Table AM define sus TIDs como quiera mientras sean estables y únicos.
 * Los stripes son inmutables, así que un TID nunca cambia de significado.
 *
 * Pero el offset NO es libre, y el techo lo pone la estructura más
 * restrictiva del core, no la nuestra:
 *
 *   - GIN empaqueta los TIDs de su posting list suponiendo 11 bits
 *     (MaxHeapTuplesPerPageBits, privado de ginpostinglist.c): offset <=
 *     2047.  Usar MaxOffsetNumber (2048) se pasaba por uno y rompía
 *     cualquier índice GIN sobre una tabla columnar.
 *   - TIDBitmap es más estricto todavía: valida cada offset contra
 *     MaxHeapTuplesPerPage —291 con BLCKSZ de 8 kB— y aborta con "tuple
 *     offset out of range".  Con 2047 el bitmap scan era IMPOSIBLE, y no
 *     por el Table AM: el índice fallaba al armar el bitmap, antes de que
 *     el AM viera nada.
 *
 * Así que el techo es MaxHeapTuplesPerPage.  Cuesta más bloques de TID por
 * fila, que no importa: con BlockNumber de 32 bits entran 1,2 billones de
 * filas igual.  Y a cambio el bitmap scan pasa a ser posible, que es lo que
 * hace que un índice y el storage columnar sumen en vez de competir.
 */
#define DBE_ROWS_PER_TIDBLOCK	((uint64) MaxHeapTuplesPerPage)

static inline void
dbe_tid_from_rownum(uint64 rownum, ItemPointer tid)
{
	ItemPointerSet(tid,
				   (BlockNumber) (rownum / DBE_ROWS_PER_TIDBLOCK),
				   (OffsetNumber) (rownum % DBE_ROWS_PER_TIDBLOCK) + 1);
}

static inline uint64
dbe_rownum_from_tid(ItemPointer tid)
{
	return (uint64) ItemPointerGetBlockNumber(tid) * DBE_ROWS_PER_TIDBLOCK
		+ (uint64) ItemPointerGetOffsetNumber(tid) - 1;
}

/* --- Buffer de escritura (staging) ------------------------------------- */

extern void dbe_staging_register_callbacks(void);
/*
 * Agrega una fila al buffer y devuelve en *rownum_out (que puede ser NULL,
 * como en la reescritura de CLUSTER) el número de fila
 * que le tocó.  El TID tiene que quedar definido acá y no en el flush: el
 * executor lo usa enseguida para las entradas de índice.
 */
extern void dbe_staging_add(Relation rel, TupleTableSlot *slot, CommandId cid,
							uint64 *rownum_out);
extern void dbe_staging_flush_relation(Oid relid);

/*
 * Escritor de stripes con números de fila dados (F6).
 *
 * Lo usa la fusión de stripes: escribir un stripe conservando los números
 * de fila de las filas que reemplaza es lo que hace que fusionar no mueva
 * ningún TID, y por lo tanto no obligue a tocar ningún índice.
 */
typedef struct DbeStripeWriter DbeStripeWriter;

extern DbeStripeWriter *dbe_stripe_writer_begin(Relation rel,
												uint64 first_row_number);
extern void dbe_stripe_writer_add(DbeStripeWriter *w, TupleTableSlot *slot);
/* Momento del commit que hereda el stripe fusionado; ver F7. */
extern void dbe_stripe_writer_set_commit_ts(DbeStripeWriter *w, TimestampTz ts);
extern uint64 dbe_stripe_writer_rows(DbeStripeWriter *w);
extern uint64 dbe_stripe_writer_finish(DbeStripeWriter *w);
extern void dbe_stripe_writer_end(DbeStripeWriter *w);
extern void dbe_staging_flush_all(void);
extern void dbe_staging_forget(Oid relid);

/* --- Cursor de lectura -------------------------------------------------- */

typedef struct DbeRowCursor DbeRowCursor;

/*
 * Filtro de stripe.  Recibe los descriptores de segmento del stripe — que
 * ya incluyen el zone map — y decide si hace falta leerlo.  Devolver false
 * ahorra la lectura del storage entera, que es lo caro.
 *
 * Tiene que ser CONSERVADOR: ante la duda, devolver true.  Descartar un
 * stripe que sí tenía filas es un resultado incorrecto; leer uno de más
 * solo cuesta tiempo.
 */
typedef bool (*DbeStripeFilterFn) (void *arg, const DbeSegDesc *segs,
								   const bool *found, int natts);

/*
 * `wanted` es un bitmap de attnums (indexado por attnum-1) o NULL para
 * todas.  Es el column pruning: los segmentos que no se piden ni se leen
 * del storage.
 */
/*
 * Columnas que la query pidió de esta relación, sacadas del plan por
 * ExecutorStart_hook (ver src/pg/scancols.c).  false = no se sabe, leer
 * todas.
 */
extern void dbe_scancols_init(void);
extern bool dbe_scan_columns_for(Oid relid, int natts, bool *wanted);

extern DbeRowCursor *dbe_cursor_begin(Relation rel, Snapshot snapshot,
									  const bool *wanted);
extern void dbe_cursor_rescan(DbeRowCursor *cur);
extern void dbe_cursor_set_filter(DbeRowCursor *cur, DbeStripeFilterFn fn,
								  void *arg, const bool *want_zone);
extern uint64 dbe_cursor_stripes_pruned(DbeRowCursor *cur);

/*
 * Descarte anidado: dentro de un stripe que pasó el filtro, saltear los
 * gránulos que igual no pueden tener filas que pasen.  Devuelve la primera
 * fila >= `row` que vale la pena mirar, o el total de filas del stripe si
 * no queda ninguna.  Ver DBE_GRANULE_ROWS en catalog.h.
 */
extern uint64 dbe_cursor_next_live_row(DbeRowCursor *cur, uint64 row);
extern uint64 dbe_cursor_granules_pruned(DbeRowCursor *cur);

/*
 * Cantidad exacta de filas visibles, sin leer ni un byte de datos.
 *
 * El row_count de cada stripe ya está en el catálogo, y el storage es
 * append-only: no hay filas muertas que descontar.  Solo hay que recorrer
 * de verdad los stripes con visibilidad parcial, que son los que quedan a
 * caballo del snapshot.
 */
extern uint64 dbe_cursor_count_rows(DbeRowCursor *cur);

/*
 * Zone map de una columna en un stripe, leyendo solo el catálogo.
 *
 * Devuelve false si el stripe no tiene visibilidad total — ahí el zone map
 * cubre filas que quien pregunta no puede ver, así que no sirve para
 * contestar min/max — o si no hay zone map utilizable.  `all_null` marca el
 * caso en que el segmento es todo nulo: ese stripe no aporta a min/max,
 * pero tampoco invalida el atajo.
 */
extern bool dbe_cursor_stripe_zonemap(DbeRowCursor *cur, int stripe_index,
									  AttrNumber attnum, DbeSegDesc *out,
									  bool *all_null);
/*
 * Late materialization: primero las columnas del predicado, y el resto solo
 * para las filas que lo pasan.  Sin set_filter_columns() el cursor se
 * comporta como siempre.
 */
extern void dbe_cursor_set_filter_columns(DbeRowCursor *cur, const bool *cols);
extern bool dbe_cursor_next_keys(DbeRowCursor *cur, TupleTableSlot *slot);
extern bool dbe_cursor_next_keys_in_stripe(DbeRowCursor *cur,
										   TupleTableSlot *slot);
extern void dbe_cursor_fill_rest(DbeRowCursor *cur, TupleTableSlot *slot);

extern bool dbe_cursor_next(DbeRowCursor *cur, TupleTableSlot *slot);
extern bool dbe_cursor_fetch_rownum(DbeRowCursor *cur, uint64 rownum,
									TupleTableSlot *slot);
extern void dbe_cursor_end(DbeRowCursor *cur);

/*
 * --- Lectura por columna, sin pasar por la interfaz de tuplas (R4) ------
 *
 * Es el camino que usa la agregación empujada al scan: en vez de armar una
 * tupla por fila, se pide el arreglo de la columna una vez por stripe y se
 * lo recorre en un lazo propio.
 */
typedef struct DbeColumnVector
{
	bool		contiguous;		/* hay arreglo de ancho fijo */
	const char *data;			/* solo si contiguous */
	uint32		width;
	const char *nulls;			/* bitmap: bit prendido = NO nulo; NULL = sin nulos */
	DbeSegReader *reader;		/* camino lento: largo variable o diccionario */
} DbeColumnVector;

/* Avanza al siguiente stripe con filas.  false cuando se acabaron. */
extern bool dbe_cursor_next_stripe(DbeRowCursor *cur);
extern uint64 dbe_cursor_stripe_rows(DbeRowCursor *cur);
extern uint64 dbe_cursor_stripe_first_row(DbeRowCursor *cur);

/* true si TODAS las filas del stripe son visibles: evita chequear una por una. */
extern bool dbe_cursor_stripe_fully_visible(DbeRowCursor *cur);

/*
 * ¿Quedan filas por procesar en el stripe cargado?  Da false si el zone map
 * lo descartó o si está vacío.  Lo usa el recorrido paralelo, que elige los
 * stripes por índice y no puede usar dbe_cursor_next_stripe().
 */
extern bool dbe_cursor_stripe_has_pending(DbeRowCursor *cur);
extern bool dbe_cursor_row_is_visible(DbeRowCursor *cur, uint64 row);

/* Vista de una columna del stripe actual.  false si no se cargó. */
extern bool dbe_cursor_column(DbeRowCursor *cur, AttrNumber attnum,
							  DbeColumnVector *out);

/* Cuántos stripes ve el cursor; lo usa el scan paralelo. */
extern int	dbe_cursor_nstripes(DbeRowCursor *cur);
/* Metadatos del stripe `index`; NULL si está fuera de rango. */
extern const DbeStripeDesc *dbe_cursor_stripe_at(DbeRowCursor *cur, int index);
extern bool dbe_cursor_set_stripe(DbeRowCursor *cur, int stripe_index);
extern bool dbe_cursor_next_in_stripe(DbeRowCursor *cur, TupleTableSlot *slot);
extern uint64 dbe_cursor_total_rows(DbeRowCursor *cur);

/*
 * ¿Hay una fila visible con ese número?  No alcanza con comparar contra la
 * cantidad total: los números de fila tienen huecos, porque se reservan de
 * a bloques al abrir el buffer de escritura.
 */
extern bool dbe_cursor_rownum_exists(DbeRowCursor *cur, uint64 rownum);

#endif							/* DBENGINE_COLUMNAR_H */
