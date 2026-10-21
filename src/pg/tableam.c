/*-------------------------------------------------------------------------
 *
 * tableam.c
 *	  TableAmRoutine del storage columnar.
 *
 * F1 es append-only: INSERT, COPY, SELECT, índices y ANALYZE.  UPDATE,
 * DELETE y el tuple locking fallan con un error claro en vez de dar
 * resultados incorrectos — es lo que pide el plan para esta fase.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/snapmgr.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"
#include "dbengine/storage.h"
#include "dbengine/workload.h"

/* PG 19 pasó VacuumParams a const. */
/*
 * PG 19 tocó varias firmas del Table AM.  Los cambios se concentran acá
 * para que el cuerpo de los callbacks sea uno solo.
 */
#if PG_VERSION_NUM >= 190000
#define DBE_INSERT_OPTIONS uint32
#define DBE_FETCH_BEGIN_ARGS Relation rel, uint32 flags
/* changingPart salió de la firma y pasó a ser un bit de `options`. */
#define DBE_DELETE_ARGS Relation relation, ItemPointer tid, CommandId cid, \
	uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, \
	TM_FailureData *tmfd
#define DBE_UPDATE_ARGS Relation relation, ItemPointer otid, \
	TupleTableSlot *slot, CommandId cid, uint32 options, Snapshot snapshot, \
	Snapshot crosscheck, bool wait, TM_FailureData *tmfd, \
	LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes
#define DBE_VACUUM_PARAMS const VacuumParams *
/* PG 19 le agregó el snapshot a relation_copy_for_cluster. */
#define DBE_CLUSTER_ARGS Relation OldTable, Relation NewTable, \
	Relation OldIndex, bool use_sort, TransactionId OldestXmin, \
	Snapshot snapshot, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, \
	double *num_tuples, double *tups_vacuumed, double *tups_recently_dead
/* PG 19 sacó OldestXmin de scan_analyze_next_tuple. */
#define DBE_ANALYZE_TUPLE_ARGS TableScanDesc sscan, \
	double *liverows, double *deadrows, TupleTableSlot *slot
#else
#define DBE_INSERT_OPTIONS int
#define DBE_FETCH_BEGIN_ARGS Relation rel
#define DBE_DELETE_ARGS Relation relation, ItemPointer tid, CommandId cid, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, \
	TM_FailureData *tmfd, bool changingPart
#define DBE_UPDATE_ARGS Relation relation, ItemPointer otid, \
	TupleTableSlot *slot, CommandId cid, Snapshot snapshot, \
	Snapshot crosscheck, bool wait, TM_FailureData *tmfd, \
	LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes
#define DBE_CLUSTER_ARGS Relation OldTable, Relation NewTable, \
	Relation OldIndex, bool use_sort, TransactionId OldestXmin, \
	TransactionId *xid_cutoff, MultiXactId *multi_cutoff, \
	double *num_tuples, double *tups_vacuumed, double *tups_recently_dead
#define DBE_VACUUM_PARAMS struct VacuumParams *
#define DBE_ANALYZE_TUPLE_ARGS TableScanDesc sscan, TransactionId OldestXmin, \
	double *liverows, double *deadrows, TupleTableSlot *slot
#endif

PG_FUNCTION_INFO_V1(dbengine_columnar_handler);

static const bool *dbe_wanted_for(Relation rel);

typedef struct DbeScanDescData
{
	TableScanDescData rs_base;
	DbeRowCursor *cursor;

	/* ANALYZE: ver el comentario largo sobre el muestreo, más abajo. */
	uint64		analyze_total_rows;
	BlockNumber analyze_total_blocks;
	BlockNumber analyze_blocks_done;
	uint64		analyze_rows_done;

	/*
	 * Bitmap scan: offsets del bloque de TID que se está recorriendo.  Un
	 * "bloque" acá son DBE_ROWS_PER_TIDBLOCK filas consecutivas, así que
	 * los TIDs de un bloque caen casi siempre en el mismo stripe y el
	 * cursor no tiene que recargarlo.
	 */
	OffsetNumber *bm_offsets;
	int			bm_noffsets;
	int			bm_next;
	BlockNumber bm_blockno;
	bool		bm_lossy;
	bool		bm_recheck;
	bool		bm_exhausted;
} DbeScanDescData;

typedef DbeScanDescData *DbeScanDesc;

typedef struct DbeParallelScanDescData
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 next_stripe;
} DbeParallelScanDescData;

typedef struct DbeIndexFetchData
{
	IndexFetchTableData base;
	DbeRowCursor *cursor;
	Snapshot	cursor_snapshot;
} DbeIndexFetchData;

pg_noreturn static void
dbe_unsupported(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("dbengine: %s no está soportado en tablas columnares", what),
			 errdetail("El storage columnar de dbengine es append-only en esta "
					   "versión."),
			 errhint("Usá una tabla heap para datos que se modifican fila a fila.")));
}

/* ------------------------------------------------------------------------
 * Slot
 * ------------------------------------------------------------------------ */

/*
 * Slots: virtuales, pero con TID.
 *
 * TTSOpsVirtual arma el HeapTuple de la copia con heap_form_tuple(), que
 * deja t_self inválido.  A heap no le pasa porque sus slots copian el tuple
 * físico, TID incluido.  El resultado es que cualquiera que haga
 * ExecCopySlotHeapTuple() sobre una fila nuestra se queda sin TID.
 *
 * Quien lo nota primero es ANALYZE: acquire_sample_rows() ordena la muestra
 * con compare_rows(), que llama a ItemPointerGetBlockNumber() sobre t_self.
 * En una build con assertions eso aborta el backend; sin ellas, todas las
 * filas comparan iguales y el orden queda arbitrario.  No hay callback del
 * AM para reemplazar acquire_sample_rows(), así que la única forma de
 * cumplir el contrato es que el slot conserve el TID.
 *
 * Se reusa TTSOpsVirtual entero y se reemplaza solo copy_heap_tuple.  No se
 * puede armar la struct de forma estática porque las funciones de
 * TTSOpsVirtual son static en execTuples.c: se copia en dbe_tableam_init().
 */
static TupleTableSlotOps dbe_tts_ops;

static HeapTuple
dbe_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTuple	tuple = TTSOpsVirtual.copy_heap_tuple(slot);

	tuple->t_self = slot->tts_tid;
	tuple->t_tableOid = slot->tts_tableOid;
	return tuple;
}

void
dbe_tableam_init(void)
{
	dbe_tts_ops = TTSOpsVirtual;
	dbe_tts_ops.copy_heap_tuple = dbe_slot_copy_heap_tuple;
}

static const TupleTableSlotOps *
dbe_slot_callbacks(Relation relation)
{
	/* Las filas se reconstruyen columna por columna: son virtuales. */
	return &dbe_tts_ops;
}

/* ------------------------------------------------------------------------
 * Scan
 * ------------------------------------------------------------------------ */

static TableScanDesc
dbe_scan_begin(Relation relation, Snapshot snapshot, int nkeys, ScanKey key,
			   ParallelTableScanDesc parallel_scan, uint32 flags)
{
	DbeScanDesc scan;

	RelationIncrementReferenceCount(relation);

	scan = (DbeScanDesc) palloc0(sizeof(DbeScanDescData));
	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = parallel_scan;

	/*
	 * Las columnas que la query pidió, si se pudieron determinar.  Para un
	 * bitmap scan esto es la diferencia entre cargar los segmentos de las
	 * 105 columnas del stripe y cargar los dos que hacen falta.
	 */
	scan->cursor = dbe_cursor_begin(relation, snapshot,
									dbe_wanted_for(relation));

	if (parallel_scan != NULL)
		dbe_cursor_set_stripe(scan->cursor, -1);

	/*
	 * ANALYZE: cuántas filas devolver por cada bloque que muestree
	 * analyze.c.  Ver el comentario largo más abajo: es lo que hace que
	 * (liverows / bs.m) * totalblocks dé la cantidad real de filas.
	 */
	if ((flags & SO_TYPE_ANALYZE) != 0)
	{
		scan->analyze_total_blocks = RelationGetNumberOfBlocksInFork(relation,
																	 MAIN_FORKNUM);
		scan->analyze_total_rows = dbe_cursor_total_rows(scan->cursor);
		scan->analyze_blocks_done = 0;
		scan->analyze_rows_done = 0;
	}

	return (TableScanDesc) scan;
}

static void
dbe_scan_end(TableScanDesc sscan)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	if (scan->cursor != NULL)
		dbe_cursor_end(scan->cursor);

	RelationDecrementReferenceCount(scan->rs_base.rs_rd);

	/*
	 * table_beginscan_parallel() y table_beginscan_catalog() registran el
	 * snapshot y delegan en el AM desregistrarlo.  Si no, el resource owner
	 * avisa "snapshot reference was not closed" al cerrar la transacción.
	 */
	if ((scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT) != 0)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	pfree(scan);
}

static void
dbe_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	dbe_cursor_rescan(scan->cursor);
}

static bool
dbe_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
					 TupleTableSlot *slot)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	if (direction == BackwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: el scan hacia atrás no está soportado en "
						"tablas columnares"),
				 errhint("Usá un cursor SCROLL con materialización, o "
						 "ORDER BY ... DESC.")));

	if (sscan->rs_parallel != NULL)
	{
		DbeParallelScanDescData *pscan =
			(DbeParallelScanDescData *) sscan->rs_parallel;

		for (;;)
		{
			uint64		idx;

			if (dbe_cursor_next_in_stripe(scan->cursor, slot))
				return true;

			/* Cada worker se lleva el siguiente stripe entero. */
			idx = pg_atomic_fetch_add_u64(&pscan->next_stripe, 1);
			if (idx >= (uint64) dbe_cursor_nstripes(scan->cursor))
				return false;
			if (!dbe_cursor_set_stripe(scan->cursor, (int) idx))
				return false;
		}
	}

	return dbe_cursor_next(scan->cursor, slot);
}

/* ------------------------------------------------------------------------
 * Scan paralelo
 * ------------------------------------------------------------------------ */

static Size
dbe_parallelscan_estimate(Relation rel)
{
	return sizeof(DbeParallelScanDescData);
}

static Size
dbe_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	DbeParallelScanDescData *p = (DbeParallelScanDescData *) pscan;

	/*
	 * Los campos de la base los inicializa el AM, no el core:
	 * table_beginscan_parallel() verifica con un Assert que phs_locator
	 * coincida con la relación.  heap lo hace en
	 * table_block_parallelscan_initialize(), que este AM no usa.
	 */
	p->base.phs_locator = rel->rd_locator;
	p->base.phs_syncscan = false;

	pg_atomic_init_u64(&p->next_stripe, 0);
	return sizeof(DbeParallelScanDescData);
}

static void
dbe_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	DbeParallelScanDescData *p = (DbeParallelScanDescData *) pscan;

	pg_atomic_write_u64(&p->next_stripe, 0);
}

/* ------------------------------------------------------------------------
 * Acceso por TID
 * ------------------------------------------------------------------------ */

/*
 * Columnas a cargar para traer filas sueltas de `rel`.
 *
 * Sin esto se cargaban los segmentos de TODAS las columnas del stripe para
 * devolver una fila: 4.932 buffers contra el 1 que costó encontrarla en el
 * índice.  Devuelve NULL cuando no se pudo determinar, que el cursor
 * interpreta como "todas".
 */
static const bool *
dbe_wanted_for(Relation rel)
{
	int			natts = RelationGetDescr(rel)->natts;
	bool	   *wanted = (bool *) palloc(sizeof(bool) * natts);

	if (dbe_scan_columns_for(RelationGetRelid(rel), natts, wanted))
		return wanted;

	pfree(wanted);
	return NULL;
}

static IndexFetchTableData *
dbe_index_fetch_begin(DBE_FETCH_BEGIN_ARGS)
{
	DbeIndexFetchData *f = palloc0(sizeof(DbeIndexFetchData));

	f->base.rel = rel;
	return &f->base;
}

static void
dbe_index_fetch_reset(IndexFetchTableData *scan)
{
	DbeIndexFetchData *f = (DbeIndexFetchData *) scan;

	if (f->cursor != NULL)
	{
		dbe_cursor_end(f->cursor);
		f->cursor = NULL;
		f->cursor_snapshot = NULL;
	}
}

static void
dbe_index_fetch_end(IndexFetchTableData *scan)
{
	dbe_index_fetch_reset(scan);
	pfree(scan);
}

static bool
dbe_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid,
					  Snapshot snapshot, TupleTableSlot *slot,
					  bool *call_again, bool *all_dead)
{
	DbeIndexFetchData *f = (DbeIndexFetchData *) scan;

	/* Sin cadenas de versiones: una llamada, una fila. */
	*call_again = false;
	if (all_dead != NULL)
		*all_dead = false;

	if (f->cursor != NULL && f->cursor_snapshot != snapshot)
		dbe_index_fetch_reset(scan);

	if (f->cursor == NULL)
	{
		f->cursor = dbe_cursor_begin(scan->rel, snapshot,
									 dbe_wanted_for(scan->rel));
		f->cursor_snapshot = snapshot;
	}

	return dbe_cursor_fetch_rownum(f->cursor, dbe_rownum_from_tid(tid), slot);
}

/* ------------------------------------------------------------------------
 * Bitmap scan
 *
 * Es la forma de que un índice y el storage columnar sumen en vez de
 * competir: el índice dice QUÉ filas y el scan las lee aprovechando lo suyo
 * —leer solo las columnas que hacen falta, y saltear las que no—.  Sin esto
 * un índice sobre una tabla columnar es decoración: el planner solo puede
 * elegir entre recorrerla entera o pedir fila por fila.
 *
 * El costo que ataca está medido: de los 149 ms de un filtro por igualdad
 * que devuelve 81.558 de 10M filas, 146 son decodificar los valores que se
 * van a descartar y 3 son recorrer el catálogo de stripes.
 *
 * Los TID de dbengine son el número de fila partido en bloques de
 * DBE_ROWS_PER_TIDBLOCK, así que un bloque del bitmap son filas
 * consecutivas: el cursor las encuentra casi siempre en el stripe que ya
 * tiene cargado.
 * ------------------------------------------------------------------------ */

static bool
dbe_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot,
						   bool *recheck, uint64 *lossy_pages,
						   uint64 *exact_pages)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	if (scan->bm_exhausted)
		return false;

	if (scan->cursor == NULL)
		scan->cursor = dbe_cursor_begin(sscan->rs_rd, sscan->rs_snapshot,
										dbe_wanted_for(sscan->rs_rd));

	if (scan->bm_offsets == NULL)
		scan->bm_offsets = (OffsetNumber *)
			palloc(sizeof(OffsetNumber) * (DBE_ROWS_PER_TIDBLOCK + 1));

	for (;;)
	{
		TBMIterateResult tbmres;

		/* Lo que quedó del bloque actual. */
		while (scan->bm_next < scan->bm_noffsets)
		{
			OffsetNumber off = scan->bm_offsets[scan->bm_next++];
			ItemPointerData tid;
			uint64		rownum;

			ItemPointerSet(&tid, scan->bm_blockno, off);
			rownum = dbe_rownum_from_tid(&tid);

			if (!dbe_cursor_fetch_rownum(scan->cursor, rownum, slot))
				continue;		/* fila inexistente o no visible */

			*recheck = scan->bm_recheck;
			return true;
		}

		if (!tbm_iterate(&sscan->st.rs_tbmiterator, &tbmres))
		{
			scan->bm_exhausted = true;
			return false;
		}

		scan->bm_blockno = tbmres.blockno;
		scan->bm_lossy = tbmres.lossy;
		scan->bm_recheck = tbmres.recheck;
		scan->bm_next = 0;

		if (tbmres.lossy)
		{
			/*
			 * Bloque impreciso: el bitmap solo dice que ADENTRO hay algo, y
			 * hay que devolver todas sus filas con recheck.  Los offsets
			 * arrancan en 1 (ver dbe_tid_from_rownum).
			 */
			int			i;

			for (i = 0; i < (int) DBE_ROWS_PER_TIDBLOCK; i++)
				scan->bm_offsets[i] = (OffsetNumber) (i + 1);
			scan->bm_noffsets = (int) DBE_ROWS_PER_TIDBLOCK;
			scan->bm_recheck = true;
			(*lossy_pages)++;
		}
		else
		{
			scan->bm_noffsets =
				tbm_extract_page_tuple(&tbmres, scan->bm_offsets,
									   (uint32) DBE_ROWS_PER_TIDBLOCK + 1);
			(*exact_pages)++;
		}
	}
}

static bool
dbe_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
							TupleTableSlot *slot)
{
	DbeRowCursor *cur = dbe_cursor_begin(rel, snapshot, NULL);
	bool		found = dbe_cursor_fetch_rownum(cur, dbe_rownum_from_tid(tid), slot);

	/*
	 * El slot queda apuntando adentro de los buffers del cursor, y acá el
	 * cursor muere ya mismo (a diferencia del scan, donde vive toda la
	 * iteración).  Hay que materializar antes de liberarlos.
	 */
	if (found)
		ExecMaterializeSlot(slot);

	dbe_cursor_end(cur);
	return found;
}

static bool
dbe_tuple_tid_valid(TableScanDesc sscan, ItemPointer tid)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	return dbe_cursor_rownum_exists(scan->cursor, dbe_rownum_from_tid(tid));
}

static void
dbe_tuple_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
	/* Append-only: una fila no tiene versiones posteriores. */
}

static bool
dbe_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							 Snapshot snapshot)
{
	DbeRowCursor *cur;
	TupleTableSlot *tmp;
	bool		visible;

	cur = dbe_cursor_begin(rel, snapshot, NULL);
	tmp = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	visible = dbe_cursor_fetch_rownum(cur, dbe_rownum_from_tid(&slot->tts_tid), tmp);
	ExecDropSingleTupleTableSlot(tmp);
	dbe_cursor_end(cur);

	return visible;
}

static TransactionId
dbe_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/*
	 * Nada para borrar: sin DELETE ni UPDATE no hay tuplas muertas que un
	 * índice pueda podar.  Devolver InvalidTransactionId es la respuesta
	 * correcta, no un stub.
	 */
	delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/* ------------------------------------------------------------------------
 * Escritura
 * ------------------------------------------------------------------------ */

static void
dbe_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
				 DBE_INSERT_OPTIONS options, BulkInsertState bistate)
{
	uint64		rownum;

	(void) options;

	dbe_staging_add(relation, slot, cid, &rownum);

	/*
	 * El TID queda definido ya mismo: ExecInsertIndexTuples lo usa apenas
	 * volvemos, y ON CONFLICT lo usa para no confundir una fila consigo
	 * misma.  Por eso los números de fila se reservan al abrir el buffer y
	 * no al flushearlo.
	 */
	dbe_tid_from_rownum(rownum, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(relation);
}

static void
dbe_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				 CommandId cid, DBE_INSERT_OPTIONS options,
				 BulkInsertState bistate)
{
	int			i;

	for (i = 0; i < ntuples; i++)
		dbe_tuple_insert(relation, slots[i], cid, options, bistate);
}

static void
dbe_finish_bulk_insert(Relation relation, DBE_INSERT_OPTIONS options)
{
	dbe_staging_flush_relation(RelationGetRelid(relation));
}

static void
dbe_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
							 CommandId cid, DBE_INSERT_OPTIONS options,
							 BulkInsertState bistate, uint32 specToken)
{
	dbe_unsupported("INSERT ... ON CONFLICT");
}

static void
dbe_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
							   uint32 specToken, bool succeeded)
{
	dbe_unsupported("INSERT ... ON CONFLICT");
}

static TM_Result
dbe_tuple_delete(DBE_DELETE_ARGS)
{
	dbe_unsupported("DELETE");
}

static TM_Result
dbe_tuple_update(DBE_UPDATE_ARGS)
{
	dbe_unsupported("UPDATE");
}

static TM_Result
dbe_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
			   TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
			   LockWaitPolicy wait_policy, uint8 flags,
			   TM_FailureData *tmfd)
{
	dbe_unsupported("SELECT ... FOR UPDATE/SHARE");
}

/* ------------------------------------------------------------------------
 * DDL y storage
 * ------------------------------------------------------------------------ */

static void
dbe_relation_set_new_filelocator(Relation rel,
								 const RelFileLocator *newrlocator,
								 char persistence,
								 TransactionId *freezeXid,
								 MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * El AM no usa xmin por fila ni multixacts, así que no hay nada que
	 * congelar.  InvalidTransactionId es lo que corresponde y saca a la
	 * tabla del cálculo de wraparound.
	 */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);
	}

	smgrclose(srel);

	/*
	 * Se llama en CREATE TABLE y en TRUNCATE.  En los dos casos hay que
	 * limpiar los metadatos previos de este OID.  Eso además cierra un
	 * agujero real: si un OID se reusa, los stripes viejos no pueden
	 * aparecer como filas de la tabla nueva.
	 */
	dbe_staging_forget(RelationGetRelid(rel));

	/*
	 * Los metadatos se indexan por relfilenode, no por OID: el storage vive
	 * en el relfilenode, y una reescritura (ALTER TABLE ... TYPE, TRUNCATE,
	 * CTAS sobre una tabla existente) lo cambia dejando el OID igual.
	 *
	 * Se limpian los dos: el nuevo por si el número se está reusando, y el
	 * viejo porque su storage está por desaparecer.
	 */
	dbe_catalog_drop_relation(newrlocator->relNumber);
	if (RelFileNumberIsValid(rel->rd_locator.relNumber) &&
		rel->rd_locator.relNumber != newrlocator->relNumber)
		dbe_catalog_drop_relation(rel->rd_locator.relNumber);
}

static void
dbe_relation_nontransactional_truncate(Relation rel)
{
	dbe_staging_forget(RelationGetRelid(rel));
	dbe_catalog_drop_relation(rel->rd_locator.relNumber);
	RelationTruncate(rel, 0);
}

static void
dbe_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	/*
	 * RelationCopyStorage() lee del disco por smgr, salteando los shared
	 * buffers.  Sin este flush, todo lo que la relación tenga sucio en
	 * memoria — empezando por la página de metadatos — no llega a la copia
	 * y la tabla nueva queda ilegible.
	 */
	FlushRelationBuffers(rel);

	dstrel = smgropen(*newrlocator, rel->rd_backend);

	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);

	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* Los metadatos van con el storage: están indexados por relfilenode. */
	dbe_catalog_copy_relation(rel->rd_locator.relNumber, newrlocator->relNumber);

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(dstrel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
		smgrimmedsync(dstrel, INIT_FORKNUM);
	}

	smgrclose(dstrel);
}

/*
 * Reescritura completa de la tabla: es lo que hay debajo de CLUSTER y de
 * VACUUM FULL.
 *
 * Para un storage columnar esto es más que reordenar filas.  Al reescribir
 * se rearman los stripes, y con ellos todo lo que se calcula al escribir:
 * los zone maps quedan con rangos ajustados —que es el punto de F6,
 * reordenar es lo que hace que el min/max pode—, los filtros de Bloom se
 * construyen para las columnas que el canal de feedback pide, y la
 * compresión se vuelve a elegir.
 *
 * Ojo con lo último: reordenar NO mejora la compresión en general.  Medido
 * a 10M filas, ordenar por una columna de baja cardinalidad la colapsa
 * —16 MB a 47 kB, porque RLE encuentra los iguales juntos— pero empeora un
 * 53% la que venía ordenada por el orden de inserción, que era la que mejor
 * comprimía antes.  El neto depende de la tabla.
 *
 * `use_sort` se ignora a propósito.  Postgres lo usa para elegir entre
 * ordenar o recorrer por el índice, una decisión de costo pensada para el
 * heap; acá recorrer por el índice sería un acceso aleatorio al storage
 * columnar, que es justo lo que no conviene.  El orden de salida es el
 * mismo en los dos casos.
 */
static void
dbe_relation_copy_for_cluster(DBE_CLUSTER_ARGS)
{
	TupleTableSlot *slot;
	DbeRowCursor *cur;
	Tuplesortstate *sort = NULL;
	double		ntuples = 0;

	*num_tuples = 0;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;

	/*
	 * Append-only: no hay filas muertas ni versiones viejas que recuperar,
	 * así que tampoco hay nada que congelar.  Es lo mismo que devuelve
	 * relation_set_new_filelocator().
	 */
	*xid_cutoff = InvalidTransactionId;
	*multi_cutoff = InvalidMultiXactId;

	/*
	 * El canal de feedback se indexa por relfilenode, y esta reescritura
	 * escribe bajo uno nuevo.  Hay que llevárselo ANTES de escribir el
	 * primer stripe: una reescritura es el único momento en que se
	 * construyen los filtros de Bloom, y la decisión de construirlos se
	 * toma por columna consultando el canal.  Sin esto se preguntaba por un
	 * relfilenode que el canal nunca había visto, así que la respuesta era
	 * que no para todas las columnas y el Bloom adaptativo no se activaba
	 * nunca — solo andaba forzándolo con dbengine.bloom_bits_per_row.
	 *
	 * No sirve hacerlo en relation_set_new_filelocator(): ahí la relación
	 * que se está creando es la tabla transitoria de CLUSTER, que no sabe
	 * de cuál viene.  Acá están las dos.
	 */
	dbe_workload_carry(OldTable->rd_locator.relNumber,
					   NewTable->rd_locator.relNumber);

	if (OldIndex != NULL)
		sort = tuplesort_begin_cluster(RelationGetDescr(OldTable), OldIndex,
									   maintenance_work_mem, NULL, 0);

	slot = MakeSingleTupleTableSlot(RelationGetDescr(OldTable), &TTSOpsVirtual);

	/*
	 * SnapshotSelf: se copia todo lo que commiteó, incluido lo que escribió
	 * esta misma transacción.  No hace falta más — sin UPDATE ni DELETE, un
	 * stripe commiteado es un stripe vivo.
	 */
	cur = dbe_cursor_begin(OldTable, SnapshotSelf, NULL);

	PG_TRY();
	{
		while (dbe_cursor_next(cur, slot))
		{
			CHECK_FOR_INTERRUPTS();

			if (sort != NULL)
			{
				HeapTuple	tuple = ExecCopySlotHeapTuple(slot);

				tuplesort_putheaptuple(sort, tuple);
				heap_freetuple(tuple);
			}
			else
			{
				/*
				 * Sin índice —VACUUM FULL— se copia en el orden en que
				 * está.  Igual sirve: compacta el espacio y rearma zone
				 * maps y filtros.
				 */
				dbe_staging_add(NewTable, slot, GetCurrentCommandId(true),
								NULL);
			}
			ntuples++;
		}

		if (sort != NULL)
		{
			TupleTableSlot *outslot;
			HeapTuple	tuple;

			tuplesort_performsort(sort);

			/*
			 * Un tuplesort de cluster devuelve HeapTuple, no un slot: se
			 * arrancó con tuplesort_putheaptuple().  Pedirle un slot da
			 * "trying to store a minimal tuple into wrong type of slot".
			 */
			outslot = MakeSingleTupleTableSlot(RelationGetDescr(OldTable),
											   &TTSOpsHeapTuple);

			while ((tuple = tuplesort_getheaptuple(sort, true)) != NULL)
			{
				CHECK_FOR_INTERRUPTS();
				ExecStoreHeapTuple(tuple, outslot, false);
				slot_getallattrs(outslot);
				dbe_staging_add(NewTable, outslot, GetCurrentCommandId(true),
								NULL);
			}
			ExecDropSingleTupleTableSlot(outslot);
		}

		/* El buffer tiene que estar en el storage antes de que se swapeen. */
		dbe_staging_flush_relation(RelationGetRelid(NewTable));

		/*
		 * Recién ahora las filas están ordenadas y volcadas.  La marca va
		 * después del flush a propósito: el flush la borra, porque no
		 * distingue quién lo llamó.
		 *
		 * Sin OldIndex esto es un VACUUM FULL, que copia en el orden en
		 * que estaba: si venía ordenado sigue estándolo, pero el orden lo
		 * heredaría del relfilenode viejo y este es otro, así que no se
		 * afirma nada.
		 */
		if (OldIndex != NULL)
			dbe_catalog_set_order(NewTable->rd_locator.relNumber,
								  RelationGetRelid(OldIndex));
	}
	PG_FINALLY();
	{
		dbe_cursor_end(cur);
		if (sort != NULL)
			tuplesort_end(sort);
		ExecDropSingleTupleTableSlot(slot);
	}
	PG_END_TRY();

	*num_tuples = ntuples;
}

static void
dbe_relation_vacuum(Relation rel, DBE_VACUUM_PARAMS params,
					BufferAccessStrategy bstrategy)
{
	/*
	 * Sin UPDATE ni DELETE no hay filas muertas que recuperar.  El VACUUM
	 * se acepta y no hace nada, en vez de fallar: así los scripts de
	 * mantenimiento genéricos y el autovacuum siguen andando.
	 */
}

/* ------------------------------------------------------------------------
 * ANALYZE
 *
 * La API es de una fila por bloque físico y un column store no tiene ese
 * mapeo: un bloque guarda parte del segmento de una columna, no filas.
 *
 * Se resuelve por proporción.  analyze.c calcula
 *     totalrows = (liverows / bs.m) * totalblocks
 * donde bs.m es la cantidad de bloques que muestreó.  Si por cada bloque
 * muestreado devolvemos (filas_totales / totalblocks) filas, la cuenta da
 * exactamente filas_totales.  Las filas salen recorriendo el cursor, así
 * que la muestra cubre toda la tabla.
 *
 * La cuota se calcula como un ACUMULADO, no sumando una fracción por
 * bloque: sumar 240,9759... ochenta y tres veces no da 20001 sino
 * 20000,9999..., y ahí se pierde una fila.  Con el acumulado, además, el
 * último bloque devuelve exactamente el resto.
 * ------------------------------------------------------------------------ */

/* Cuántas filas corresponde haber devuelto después de `blocks` bloques. */
static uint64
dbe_analyze_quota(DbeScanDesc scan, BlockNumber blocks)
{
	if (scan->analyze_total_blocks == 0)
		return 0;
	if (blocks >= scan->analyze_total_blocks)
		return scan->analyze_total_rows;

	return (uint64) ((double) scan->analyze_total_rows * blocks /
					 scan->analyze_total_blocks);
}

static bool
dbe_scan_analyze_next_block(TableScanDesc sscan, ReadStream *stream)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;
	Buffer		buf;

	buf = read_stream_next_buffer(stream, NULL);
	if (!BufferIsValid(buf))
		return false;

	/* No nos interesa el contenido del bloque, solo llevar la cuenta. */
	ReleaseBuffer(buf);

	scan->analyze_blocks_done++;
	return true;
}

static bool
dbe_scan_analyze_next_tuple(DBE_ANALYZE_TUPLE_ARGS)
{
	DbeScanDesc scan = (DbeScanDesc) sscan;

	if (scan->analyze_rows_done >= dbe_analyze_quota(scan, scan->analyze_blocks_done))
		return false;

	if (!dbe_cursor_next(scan->cursor, slot))
	{
		/* Se acabaron las filas antes que los bloques. */
		scan->analyze_total_rows = scan->analyze_rows_done;
		return false;
	}

	scan->analyze_rows_done++;
	(*liverows)++;
	return true;
}

/* ------------------------------------------------------------------------
 * Índices
 * ------------------------------------------------------------------------ */

static double
dbe_index_build_range_scan(Relation table_rel, Relation index_rel,
						   IndexInfo *index_info, bool allow_sync,
						   bool anyvisible, bool progress,
						   BlockNumber start_blockno, BlockNumber numblocks,
						   IndexBuildCallback callback, void *callback_state,
						   TableScanDesc scan)
{
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *predicate;
	TupleTableSlot *slot;
	Snapshot	snapshot = NULL;
	bool		own_snapshot = false;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples = 0;

	if (start_blockno != 0 || numblocks != InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: la construcción de índices por rango de "
						"bloques no está soportada en tablas columnares"),
				 errdetail("Los TIDs columnares son lógicos y no se "
						   "corresponden con bloques físicos.")));

	/*
	 * Si viene un TableScanDesc hay que USARLO, no abrir un cursor propio.
	 *
	 * En una construcción de índice paralela cada worker recibe el mismo
	 * scan compartido, que es el que reparte los stripes.  Ignorarlo hacía
	 * que cada worker recorriera la tabla entera y metiera las mismas filas
	 * en el índice: entradas duplicadas, y TIDs fuera de orden que rompían
	 * el build de GIN.  Un btree lo hubiera aceptado en silencio.
	 */
	if (scan == NULL)
	{
		/*
		 * CREATE INDEX toma ShareLock, así que no entran escrituras
		 * concurrentes: un snapshot nuevo ve todo lo commiteado.
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		own_snapshot = true;
		scan = table_beginscan_strat(table_rel, snapshot, 0, NULL,
									 true, allow_sync);
	}

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(table_rel, NULL);
	econtext->ecxt_scantuple = slot;

	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;

		FormIndexDatum(index_info, slot, estate, values, isnull);

		/* Todas las filas visibles están vivas: no hay DELETE. */
		callback(index_rel, &slot->tts_tid, values, isnull, true,
				 callback_state);

		reltuples++;

		if (progress)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 (int64) reltuples);
	}

	/*
	 * El scan se cierra siempre, venga de afuera o no: es lo que hace
	 * heapam, y el core cuenta con eso.  Cerrarlo solo cuando lo abrimos
	 * nosotros dejaba la relación sin liberar en cada worker de un build
	 * paralelo.
	 */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	if (own_snapshot)
		UnregisterSnapshot(snapshot);

	index_info->ii_PredicateState = NULL;
	index_info->ii_ExpressionsState = NIL;

	return reltuples;
}

static void
dbe_index_validate_scan(Relation table_rel, Relation index_rel,
						IndexInfo *index_info, Snapshot snapshot,
						ValidateIndexState *state)
{
	dbe_unsupported("CREATE INDEX CONCURRENTLY");
}

/* ------------------------------------------------------------------------
 * Tamaño y estimaciones (R6)
 * ------------------------------------------------------------------------ */

static uint64
dbe_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64		nblocks = 0;

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return 0;

	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i <= MAX_FORKNUM; i++)
			nblocks += smgrnblocks(RelationGetSmgr(rel), i);
	}
	else
		nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);

	return nblocks * BLCKSZ;
}

static bool
dbe_relation_needs_toast_table(Relation rel)
{
	/*
	 * No hay tabla TOAST: los varlena se guardan ya materializados adentro
	 * del segmento, y el archivo lógico los reparte entre páginas sin
	 * límite de 8 kB.
	 */
	return false;
}

static void
dbe_relation_estimate_size(Relation rel, int32 *attr_widths,
						   BlockNumber *pages, double *tuples,
						   double *allvisfrac)
{
	uint64		nrows = 0;
	uint64		nbytes = 0;
	uint64		nstripes = 0;

	*pages = RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM);
	*allvisfrac = 0.0;

	/*
	 * La cuenta de filas sale del catálogo interno y es exacta, así que no
	 * depende de que se haya corrido ANALYZE.  ANALYZE sigue haciendo
	 * falta para las estadísticas por columna (histogramas, MCV).
	 */
	if (ActiveSnapshotSet())
	{
		dbe_catalog_summarize(rel->rd_locator.relNumber, GetActiveSnapshot(),
							  &nrows, &nbytes, &nstripes);
		*tuples = (double) nrows;
	}
	else if (rel->rd_rel->reltuples >= 0)
		*tuples = rel->rd_rel->reltuples;
	else
		*tuples = 0;
}

/* ------------------------------------------------------------------------
 * Muestreo (TABLESAMPLE)
 * ------------------------------------------------------------------------ */

static bool
dbe_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	dbe_unsupported("TABLESAMPLE");
}

static bool
dbe_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
						   TupleTableSlot *slot)
{
	dbe_unsupported("TABLESAMPLE");
}

/* ------------------------------------------------------------------------
 * La tabla de callbacks
 * ------------------------------------------------------------------------ */

static const TableAmRoutine dbe_columnar_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = dbe_slot_callbacks,

	.scan_begin = dbe_scan_begin,
	.scan_end = dbe_scan_end,
	.scan_rescan = dbe_scan_rescan,
	.scan_getnextslot = dbe_scan_getnextslot,

	/* Los TID ranges (ctid BETWEEN ...) no aplican a TIDs lógicos. */
	.scan_set_tidrange = NULL,
	.scan_getnextslot_tidrange = NULL,

	.parallelscan_estimate = dbe_parallelscan_estimate,
	.parallelscan_initialize = dbe_parallelscan_initialize,
	.parallelscan_reinitialize = dbe_parallelscan_reinitialize,

	.index_fetch_begin = dbe_index_fetch_begin,
	.index_fetch_reset = dbe_index_fetch_reset,
	.index_fetch_end = dbe_index_fetch_end,
	.index_fetch_tuple = dbe_index_fetch_tuple,

	.tuple_fetch_row_version = dbe_tuple_fetch_row_version,
	.tuple_tid_valid = dbe_tuple_tid_valid,
	.tuple_get_latest_tid = dbe_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = dbe_tuple_satisfies_snapshot,
	.index_delete_tuples = dbe_index_delete_tuples,

	.tuple_insert = dbe_tuple_insert,
	.tuple_insert_speculative = dbe_tuple_insert_speculative,
	.tuple_complete_speculative = dbe_tuple_complete_speculative,
	.multi_insert = dbe_multi_insert,
	.tuple_delete = dbe_tuple_delete,
	.tuple_update = dbe_tuple_update,
	.tuple_lock = dbe_tuple_lock,
	.finish_bulk_insert = dbe_finish_bulk_insert,

	.relation_set_new_filelocator = dbe_relation_set_new_filelocator,
	.relation_nontransactional_truncate = dbe_relation_nontransactional_truncate,
	.relation_copy_data = dbe_relation_copy_data,
	.relation_copy_for_cluster = dbe_relation_copy_for_cluster,
	.relation_vacuum = dbe_relation_vacuum,
	.scan_analyze_next_block = dbe_scan_analyze_next_block,
	.scan_analyze_next_tuple = dbe_scan_analyze_next_tuple,
	.index_build_range_scan = dbe_index_build_range_scan,
	.index_validate_scan = dbe_index_validate_scan,

	.relation_size = dbe_relation_size,
	.relation_needs_toast_table = dbe_relation_needs_toast_table,
	.relation_toast_am = NULL,
	.relation_fetch_toast_slice = NULL,

	.relation_estimate_size = dbe_relation_estimate_size,

	/* Sin bitmap scan: requiere TIDs con semántica de bloque físico. */
	.scan_bitmap_next_tuple = dbe_scan_bitmap_next_tuple,

	.scan_sample_next_block = dbe_scan_sample_next_block,
	.scan_sample_next_tuple = dbe_scan_sample_next_tuple,
};

Datum
dbengine_columnar_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&dbe_columnar_methods);
}

bool
dbe_is_columnar_relation(Relation rel)
{
	return rel != NULL &&
		rel->rd_rel->relkind == RELKIND_RELATION &&
		rel->rd_tableam == &dbe_columnar_methods;
}
