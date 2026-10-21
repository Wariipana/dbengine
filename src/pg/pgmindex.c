/*-------------------------------------------------------------------------
 *
 * pgmindex.c
 *	  Index AM `dbengine_pgm`.  Ver pgmindex.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include <math.h>
#include "miscadmin.h"
#include "pgstat.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/spccache.h"
#include "funcapi.h"
#include "utils/tuplesort.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "dbengine/bridge.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"
#include "dbengine/pgmindex.h"
#include "dbengine/storage.h"

PG_FUNCTION_INFO_V1(dbengine_pgm_handler);

/*
 * Estado del build.
 *
 * Las claves van a un tuplesort, no a un arreglo propio: es lo que respeta
 * maintenance_work_mem y derrama a disco.  Antes se juntaba todo en memoria
 * con repalloc_huge(), lo que a 100M filas son varios GB y en una máquina
 * más chica es el OOM killer, no una degradación.
 */
typedef struct DbePgmBuildState
{
	Tuplesortstate *sort;
	TupleTableSlot *slot;		/* virtual, para poner */
	TupleTableSlot *out;		/* minimal, para sacar: tuplesort devuelve
								 * minimal tuples y el slot tiene que serlo */
	uint64		nentries;
	Oid			keytype;
	int64		nulls;
} DbePgmBuildState;

/* Estado de un scan. */
typedef struct DbePgmScanOpaqueData
{
	DbePgmMeta	meta;
	char	   *model;			/* copia del modelo en memoria del scan */
	bool		loaded;

	/* Rango de filas que satisface las claves de búsqueda. */
	uint64		lo;
	uint64		hi;
	uint64		next;
	bool		empty;

	/* Recorrido de la cadena delta. */
	uint64		delta_chunk;
	uint32		delta_pos;
	bool		delta_done;
} DbePgmScanOpaqueData;

typedef DbePgmScanOpaqueData *DbePgmScanOpaque;

static IndexBuildResult *dbe_pgm_ambuild(Relation heap, Relation index,
										 IndexInfo *indexInfo);
static void dbe_pgm_ambuildempty(Relation index);
static bool dbe_pgm_aminsert(Relation index, Datum *values, bool *isnull,
							 ItemPointer heap_tid, Relation heap,
							 IndexUniqueCheck checkUnique,
							 bool indexUnchanged, IndexInfo *indexInfo);
static IndexBulkDeleteResult *dbe_pgm_ambulkdelete(IndexVacuumInfo *info,
												   IndexBulkDeleteResult *stats,
												   IndexBulkDeleteCallback callback,
												   void *callback_state);
static IndexBulkDeleteResult *dbe_pgm_amvacuumcleanup(IndexVacuumInfo *info,
													  IndexBulkDeleteResult *stats);
static void dbe_pgm_amcostestimate(PlannerInfo *root, IndexPath *path,
								   double loop_count, Cost *indexStartupCost,
								   Cost *indexTotalCost, Selectivity *indexSelectivity,
								   double *indexCorrelation, double *indexPages);
static bytea *dbe_pgm_amoptions(Datum reloptions, bool validate);
static bool dbe_pgm_amvalidate(Oid opclassoid);
static IndexScanDesc dbe_pgm_ambeginscan(Relation index, int nkeys, int norderbys);
static void dbe_pgm_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
							 ScanKey orderbys, int norderbys);
static bool dbe_pgm_amgettuple(IndexScanDesc scan, ScanDirection dir);
static int64 dbe_pgm_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
static void dbe_pgm_amendscan(IndexScanDesc scan);

/* ------------------------------------------------------------------------
 * Tipos de clave
 *
 * El método interpola sobre números: la clave tiene que poder mapearse a
 * int64 sin romper el orden.  Los enteros y los tipos de fecha/hora salen
 * directo; el resto se deja al btree, que para eso está.
 * ------------------------------------------------------------------------ */

bool
dbe_pgm_type_supported(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return true;
		default:
			return false;
	}
}

int64
dbe_pgm_key_to_int64(Oid typid, Datum value)
{
	switch (typid)
	{
		case INT2OID:
			return (int64) DatumGetInt16(value);
		case INT4OID:
			return (int64) DatumGetInt32(value);
		case INT8OID:
			return DatumGetInt64(value);
		case OIDOID:
			return (int64) DatumGetObjectId(value);
		case DATEOID:
			return (int64) DatumGetDateADT(value);
		case TIMESTAMPOID:
			return (int64) DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return (int64) DatumGetTimestampTz(value);
		case TIMEOID:
			return (int64) DatumGetTimeADT(value);
		default:
			elog(ERROR, "dbengine: tipo %u no mapeable a int64", typid);
			return 0;			/* no llega */
	}
}

/* ------------------------------------------------------------------------
 * Handler
 * ------------------------------------------------------------------------ */

Datum
dbengine_pgm_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	/* Estrategias btree: <, <=, =, >=, >.  Sin funciones de soporte. */
	amroutine->amstrategies = 5;
	amroutine->amsupport = 0;
	amroutine->amoptsprocnum = 0;

	/*
	 * amcanorder = false: el scan no devuelve las filas ordenadas por la
	 * clave.  Podría —las filas están en orden de clave— pero entonces
	 * habría que soportar también el scan hacia atrás y las marcas de
	 * posición, y eso es otra fase.
	 */
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = true;
	amroutine->amconsistentordering = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = true;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = dbe_pgm_ambuild;
	amroutine->ambuildempty = dbe_pgm_ambuildempty;
	amroutine->aminsert = dbe_pgm_aminsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = dbe_pgm_ambulkdelete;
	amroutine->amvacuumcleanup = dbe_pgm_amvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = dbe_pgm_amcostestimate;
#if PG_VERSION_NUM >= 180000
	amroutine->amgettreeheight = NULL;
#endif
	amroutine->amoptions = dbe_pgm_amoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = dbe_pgm_amvalidate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = dbe_pgm_ambeginscan;
	amroutine->amrescan = dbe_pgm_amrescan;
	amroutine->amgettuple = dbe_pgm_amgettuple;
	amroutine->amgetbitmap = dbe_pgm_amgetbitmap;
	amroutine->amendscan = dbe_pgm_amendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

static bytea *
dbe_pgm_amoptions(Datum reloptions, bool validate)
{
	return NULL;
}

static bool
dbe_pgm_amvalidate(Oid opclassoid)
{
	/*
	 * La opclass no declara funciones de soporte: la conversión de la clave
	 * a int64 la decide el AM por el OID del tipo.  No hay nada que validar
	 * más allá de lo que ya chequeó el catálogo.
	 */
	return true;
}

/* ------------------------------------------------------------------------
 * Lectura del índice
 * ------------------------------------------------------------------------ */

static bool
dbe_pgm_read_meta(Relation index, DbePgmMeta *out)
{
	DbeStorageMeta smeta;

	if (!dbe_storage_read_meta(index, &smeta))
		return false;
	if (smeta.next_offset < sizeof(DbePgmMeta))
		return false;

	dbe_storage_read(index, 0, (char *) out, sizeof(DbePgmMeta));
	if (out->magic != DBE_PGM_MAGIC)
		return false;
	if (out->version != DBE_PGM_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: índice PGM con versión %u, se esperaba %u",
						out->version, DBE_PGM_VERSION),
				 errhint("Reconstruilo con REINDEX.")));
	return true;
}

/* Una clave distinta por su rango. */
static int64
dbe_pgm_key_at(Relation index, const DbePgmMeta *meta, uint64 rank)
{
	int64		key;

	Assert(rank < meta->ndistinct);
	dbe_storage_read(index, meta->keys_off + rank * sizeof(int64),
					 (char *) &key, sizeof(int64));
	return key;
}

/* Fila donde arranca la corrida de la clave distinta `rank`. */
static uint64
dbe_pgm_start_at(Relation index, const DbePgmMeta *meta, uint64 rank)
{
	uint64		start;

	Assert(rank <= meta->ndistinct);
	if (meta->starts_off == 0)
		return rank;			/* sin repetidas: la fila ES el rango */

	if (meta->flags & DBE_PGM_STARTS32)
	{
		uint32		s32;

		dbe_storage_read(index, meta->starts_off + rank * sizeof(uint32),
						 (char *) &s32, sizeof(uint32));
		return s32;
	}

	dbe_storage_read(index, meta->starts_off + rank * sizeof(uint64),
					 (char *) &start, sizeof(uint64));
	return start;
}

static uint64
dbe_pgm_row_at(Relation index, const DbePgmMeta *meta, uint64 pos)
{
	uint64		row;

	Assert(pos < meta->nrows);
	dbe_storage_read(index, meta->rows_off + pos * sizeof(uint64),
					 (char *) &row, sizeof(uint64));
	return row;
}

/*
 * Primer rango distinto cuya clave es >= `key`.
 *
 * El modelo propone una ventana y se verifica con dos comparaciones exactas
 * si la respuesta puede estar afuera.  Si puede, se hace la binaria sobre
 * todo el arreglo: el resultado es siempre el correcto, ande bien o mal el
 * modelo.  Esa es la regla — la predicción acelera, nunca decide.
 */
static uint64
dbe_pgm_lower_bound(Relation index, const DbePgmMeta *meta, const char *model,
					int64 key, bool *fell_back)
{
	uint64		lo = 0;
	uint64		hi = meta->ndistinct;
	DbeStatus	st;

	if (meta->ndistinct == 0)
		return 0;

	st = dbe_pgm_locate(model, meta->model_len, key, &lo, &hi);
	dbe_error_if_not_ok(&st, "consultando el modelo del índice PGM");

	if (lo > meta->ndistinct)
		lo = meta->ndistinct;
	if (hi > meta->ndistinct)
		hi = meta->ndistinct;

	if ((lo > 0 && dbe_pgm_key_at(index, meta, lo - 1) >= key) ||
		(hi < meta->ndistinct && dbe_pgm_key_at(index, meta, hi) < key))
	{
		if (fell_back != NULL)
			*fell_back = true;
		lo = 0;
		hi = meta->ndistinct;
	}

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (dbe_pgm_key_at(index, meta, mid) < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* ------------------------------------------------------------------------
 * Construcción
 * ------------------------------------------------------------------------ */

static void
dbe_pgm_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	DbePgmBuildState *bs = (DbePgmBuildState *) state;

	if (!tupleIsAlive)
		return;

	/*
	 * Sin nulos.  Un índice que no los guarda no puede contestar
	 * `IS NULL`, así que amsearchnulls queda en false y el planner no lo
	 * elige para esa forma.
	 */
	if (isnull[0])
	{
		bs->nulls++;
		return;
	}

	ExecClearTuple(bs->slot);
	bs->slot->tts_values[0] =
		Int64GetDatum(dbe_pgm_key_to_int64(bs->keytype, values[0]));
	bs->slot->tts_isnull[0] = false;
	bs->slot->tts_values[1] = Int64GetDatum((int64) dbe_rownum_from_tid(tid));
	bs->slot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(bs->slot);

	tuplesort_puttupleslot(bs->sort, bs->slot);
	bs->nentries++;
}

/*
 * Escritor con buffer sobre el archivo lógico.
 *
 * dbe_storage_write() escribe un registro de WAL de página completa por
 * llamada: está pensado para volcar un stripe entero de una vez.  Llamarlo
 * de a 8 bytes, que es el tamaño de una clave, hace un WAL de 8 kB por
 * clave — medido, el build de 2M filas pasaba de 0,8 s a 174 s.  Se junta
 * en un buffer y se escribe por bloques.
 */
typedef struct DbePgmWriter
{
	Relation	rel;
	uint64		off;			/* próximo byte lógico a escribir */
	char	   *buf;
	uint32		used;
	uint32		cap;
} DbePgmWriter;

static void
dbe_pgm_writer_init(DbePgmWriter *w, Relation rel, uint64 off)
{
	w->rel = rel;
	w->off = off;
	w->cap = 256 * 1024;
	w->buf = (char *) palloc(w->cap);
	w->used = 0;
}

static void
dbe_pgm_writer_flush(DbePgmWriter *w)
{
	if (w->used == 0)
		return;
	dbe_storage_write(w->rel, w->off, w->buf, w->used);
	w->off += w->used;
	w->used = 0;
}

static void
dbe_pgm_writer_put(DbePgmWriter *w, const void *data, uint32 len)
{
	if (w->used + len > w->cap)
		dbe_pgm_writer_flush(w);
	memcpy(w->buf + w->used, data, len);
	w->used += len;
}

static void
dbe_pgm_writer_done(DbePgmWriter *w)
{
	dbe_pgm_writer_flush(w);
	pfree(w->buf);
	w->buf = NULL;
}

/* Descriptor (clave, fila) del tuplesort. */
static TupleDesc
dbe_pgm_sort_tupdesc(void)
{
	TupleDesc	td = CreateTemplateTupleDesc(2);

	TupleDescInitEntry(td, (AttrNumber) 1, "clave", INT8OID, -1, 0);
	TupleDescInitEntry(td, (AttrNumber) 2, "fila", INT8OID, -1, 0);
	return BlessTupleDesc(td);
}

static IndexBuildResult *
dbe_pgm_ambuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	DbePgmBuildState bs;
	IndexBuildResult *result;
	DbePgmMeta	meta;
	Oid			keytype;
	DbePgmBuilder *builder = NULL;
	const void *model_bytes = NULL;
	uint64		model_len = 0;
	DbeStatus	st;
	TupleDesc	sortdesc;
	AttrNumber	sortcols[2] = {1, 2};
	Oid			sortops[2];
	Oid			sortcolls[2] = {InvalidOid, InvalidOid};
	bool		nullsfirst[2] = {false, false};
	uint64		ndistinct = 0;
	int64		prev_key = 0;
	bool		have_prev = false;
	bool		has_dups;
	bool		starts32_ok;
	uint64		base;
	uint64		total;
	uint64		starts_bytes;
	DbePgmWriter wkeys = {0};
	DbePgmWriter wstarts = {0};
	DbePgmWriter wrows = {0};

	if (indexInfo->ii_NumIndexAttrs != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: el índice PGM es de una sola columna")));
	if (indexInfo->ii_Unique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: el índice PGM no puede imponer unicidad"),
				 errhint("Usá un btree para una restricción UNIQUE o PRIMARY KEY.")));

	keytype = TupleDescAttr(RelationGetDescr(index), 0)->atttypid;
	if (!dbe_pgm_type_supported(keytype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: el índice PGM no soporta el tipo %s",
						format_type_be(keytype)),
				 errdetail("El modelo interpola sobre números: la clave tiene "
						   "que mapearse a un entero conservando el orden."),
				 errhint("Los tipos enteros y los de fecha/hora sí andan.")));

	dbe_storage_init(index);

	/*
	 * Se ordena por (clave, fila).  Ordenar también por fila no hace falta
	 * para la corrección, pero deja los TIDs de una misma clave en orden
	 * creciente, que es como conviene leerlos de la tabla después.
	 */
	sortops[0] = get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID,
									 BTLessStrategyNumber);
	sortops[1] = sortops[0];

	memset(&bs, 0, sizeof(bs));
	bs.keytype = keytype;
	sortdesc = dbe_pgm_sort_tupdesc();
	bs.slot = MakeSingleTupleTableSlot(sortdesc, &TTSOpsVirtual);
	bs.out = MakeSingleTupleTableSlot(sortdesc, &TTSOpsMinimalTuple);

	/*
	 * TUPLESORT_RANDOMACCESS: hacen falta DOS pasadas sobre el resultado
	 * ordenado.  La primera alimenta el modelo y cuenta las claves
	 * distintas; recién con eso se sabe cuánto ocupa cada arreglo y se puede
	 * reservar el espacio exacto.  La segunda escribe.
	 */
	bs.sort = tuplesort_begin_heap(sortdesc, 2, sortcols, sortops, sortcolls,
								   nullsfirst, maintenance_work_mem, NULL,
								   TUPLESORT_RANDOMACCESS);

	table_index_build_scan(heap, index, indexInfo, true, true,
						   dbe_pgm_build_callback, &bs, NULL);

	tuplesort_performsort(bs.sort);

	st = dbe_pgm_builder_create(DBE_PGM_EPSILON, &builder);
	dbe_error_if_not_ok(&st, "creando el modelo del índice PGM");

	PG_TRY();
	{
		uint64		i;

		/* --- pasada 1: modelo y conteo ------------------------------- */
		while (tuplesort_gettupleslot(bs.sort, true, false, bs.out, NULL))
		{
			int64		k;

			slot_getallattrs(bs.out);
			k = DatumGetInt64(bs.out->tts_values[0]);

			if (!have_prev || k != prev_key)
			{
				st = dbe_pgm_builder_add(builder, k);
				dbe_error_if_not_ok(&st, "alimentando el modelo del índice PGM");
				ndistinct++;
				prev_key = k;
				have_prev = true;
			}
		}

		st = dbe_pgm_builder_finish(builder);
		dbe_error_if_not_ok(&st, "cerrando el modelo del índice PGM");
		st = dbe_pgm_bytes(builder, &model_bytes, &model_len);
		dbe_error_if_not_ok(&st, "serializando el modelo del índice PGM");

		has_dups = (ndistinct != bs.nentries);
		starts32_ok = has_dups && bs.nentries <= PG_UINT32_MAX;
		starts_bytes = has_dups
			? (ndistinct + 1) * (starts32_ok ? sizeof(uint32) : sizeof(uint64))
			: 0;

		memset(&meta, 0, sizeof(meta));
		meta.magic = DBE_PGM_MAGIC;
		meta.version = DBE_PGM_VERSION;
		meta.epsilon = DBE_PGM_EPSILON;
		meta.nrows = bs.nentries;
		meta.ndistinct = ndistinct;
		if (starts32_ok)
			meta.flags |= DBE_PGM_STARTS32;

		/* La metapágina va primero, para que quede en el offset 0. */
		(void) dbe_storage_reserve_bytes(index, sizeof(DbePgmMeta), NULL);

		total = MAXALIGN(model_len)
			+ MAXALIGN(ndistinct * sizeof(int64))
			+ MAXALIGN(starts_bytes)
			+ MAXALIGN(bs.nentries * sizeof(uint64));

		base = dbe_storage_reserve_bytes(index, Max(total, 1), NULL);

		meta.model_off = base;
		meta.model_len = model_len;
		base += MAXALIGN(model_len);
		meta.keys_off = base;
		base += MAXALIGN(ndistinct * sizeof(int64));
		if (has_dups)
		{
			meta.starts_off = base;
			base += MAXALIGN(starts_bytes);
		}
		meta.rows_off = base;

		if (model_len > 0)
			dbe_storage_write(index, meta.model_off, model_bytes, model_len);

		/* --- pasada 2: escribir claves, arranques y filas ------------ */
		dbe_pgm_writer_init(&wkeys, index, meta.keys_off);
		dbe_pgm_writer_init(&wstarts, index, meta.starts_off);
		dbe_pgm_writer_init(&wrows, index, meta.rows_off);

		tuplesort_rescan(bs.sort);
		have_prev = false;
		ndistinct = 0;
		i = 0;

		while (tuplesort_gettupleslot(bs.sort, true, false, bs.out, NULL))
		{
			int64		k;
			uint64		row;

			slot_getallattrs(bs.out);
			k = DatumGetInt64(bs.out->tts_values[0]);
			row = (uint64) DatumGetInt64(bs.out->tts_values[1]);

			if (!have_prev || k != prev_key)
			{
				dbe_pgm_writer_put(&wkeys, &k, sizeof(int64));
				if (has_dups)
				{
					if (starts32_ok)
					{
						uint32		s32 = (uint32) i;

						dbe_pgm_writer_put(&wstarts, &s32, sizeof(uint32));
					}
					else
						dbe_pgm_writer_put(&wstarts, &i, sizeof(uint64));
				}
				ndistinct++;
				prev_key = k;
				have_prev = true;
			}

			dbe_pgm_writer_put(&wrows, &row, sizeof(uint64));
			i++;
		}

		if (has_dups)
		{
			/* El cierre: dónde termina la última corrida. */
			if (starts32_ok)
			{
				uint32		s32 = (uint32) bs.nentries;

				dbe_pgm_writer_put(&wstarts, &s32, sizeof(uint32));
			}
			else
				dbe_pgm_writer_put(&wstarts, &bs.nentries, sizeof(uint64));
		}

		dbe_pgm_writer_done(&wkeys);
		dbe_pgm_writer_done(&wstarts);
		dbe_pgm_writer_done(&wrows);

		dbe_storage_write(index, 0, (const char *) &meta, sizeof(meta));
	}
	PG_FINALLY();
	{
		dbe_pgm_destroy(builder);
		tuplesort_end(bs.sort);
	}
	PG_END_TRY();

	ExecDropSingleTupleTableSlot(bs.slot);
	ExecDropSingleTupleTableSlot(bs.out);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = (double) (bs.nentries + bs.nulls);
	result->index_tuples = (double) bs.nentries;
	return result;
}

static void
dbe_pgm_ambuildempty(Relation index)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("dbengine: el índice PGM no soporta tablas unlogged"),
			 errdetail("Reconstruir el índice desde el fork init pediría "
					   "escribir el modelo ahí, y el modelo se arma leyendo "
					   "la tabla entera."),
			 errhint("Usá una tabla logged, o un btree.")));
}

/* ------------------------------------------------------------------------
 * Inserción: cadena delta
 *
 * El modelo se arma sobre el arreglo entero, así que no se puede insertar
 * de a una fila adentro.  Lo que se inserta después del build va a una
 * cadena de bloques que los scans recorren linealmente, además de consultar
 * el modelo.  Es el "buffer delta" de F4: barato de escribir, y caro de
 * leer solo si se deja crecer.  REINDEX lo absorbe.
 * ------------------------------------------------------------------------ */

static bool
dbe_pgm_aminsert(Relation index, Datum *values, bool *isnull,
				 ItemPointer heap_tid, Relation heap,
				 IndexUniqueCheck checkUnique, bool indexUnchanged,
				 IndexInfo *indexInfo)
{
	DbePgmMeta	meta;
	DbePgmDeltaChunk chunk;
	DbePgmDeltaEntry entry;
	Oid			keytype;
	uint64		off;

	if (isnull[0])
		return false;

	keytype = TupleDescAttr(RelationGetDescr(index), 0)->atttypid;
	entry.key = dbe_pgm_key_to_int64(keytype, values[0]);
	entry.rownum = dbe_rownum_from_tid(heap_tid);

	/*
	 * Agregar al delta es leer-modificar-escribir sobre dos lugares: el
	 * contador del último bloque y la metapágina.  Sin serializar, dos
	 * backends leen el mismo `used`, escriben los dos en el mismo slot y una
	 * de las dos entradas desaparece — un índice al que le falta una fila,
	 * sin ningún error.
	 *
	 * Se toma un lock de página sobre el bloque 0.  Serializa las
	 * inserciones al índice, que es el precio de tener el delta en una
	 * cadena y no en páginas propias por backend.
	 */
	LockPage(index, DBE_META_BLOCK, ExclusiveLock);

	if (!dbe_pgm_read_meta(index, &meta))
	{
		UnlockPage(index, DBE_META_BLOCK, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("dbengine: el índice PGM no está construido"),
				 errhint("Corré REINDEX.")));
	}

	if (meta.delta_tail != 0)
	{
		dbe_storage_read(index, meta.delta_tail, (char *) &chunk,
						 sizeof(chunk));
		if (chunk.used < DBE_PGM_DELTA_ENTRIES)
		{
			off = meta.delta_tail + sizeof(DbePgmDeltaChunk) +
				(uint64) chunk.used * sizeof(DbePgmDeltaEntry);
			dbe_storage_write(index, off, (const char *) &entry, sizeof(entry));

			chunk.used++;
			dbe_storage_write(index, meta.delta_tail, (const char *) &chunk,
							  sizeof(chunk));

			meta.delta_rows++;
			dbe_storage_write(index, 0, (const char *) &meta, sizeof(meta));
			UnlockPage(index, DBE_META_BLOCK, ExclusiveLock);
			return false;
		}
	}

	/* Bloque nuevo. */
	{
		uint64		newoff;

		newoff = dbe_storage_reserve_bytes(index,
										   sizeof(DbePgmDeltaChunk) +
										   (uint64) DBE_PGM_DELTA_ENTRIES *
										   sizeof(DbePgmDeltaEntry),
										   NULL);

		memset(&chunk, 0, sizeof(chunk));
		chunk.used = 1;
		dbe_storage_write(index, newoff, (const char *) &chunk, sizeof(chunk));
		dbe_storage_write(index, newoff + sizeof(DbePgmDeltaChunk),
						  (const char *) &entry, sizeof(entry));

		if (meta.delta_tail != 0)
		{
			DbePgmDeltaChunk tail;

			dbe_storage_read(index, meta.delta_tail, (char *) &tail,
							 sizeof(tail));
			tail.next = newoff;
			dbe_storage_write(index, meta.delta_tail, (const char *) &tail,
							  sizeof(tail));
		}
		else
			meta.delta_head = newoff;

		meta.delta_tail = newoff;
		meta.delta_rows++;
		dbe_storage_write(index, 0, (const char *) &meta, sizeof(meta));
	}

	UnlockPage(index, DBE_META_BLOCK, ExclusiveLock);
	return false;
}

/* ------------------------------------------------------------------------
 * VACUUM
 *
 * El storage columnar es append-only: no hay filas muertas que sacar del
 * índice.  Se aceptan las dos llamadas y no hacen nada, así que el
 * autovacuum y los scripts genéricos siguen andando.
 * ------------------------------------------------------------------------ */

static IndexBulkDeleteResult *
dbe_pgm_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
					 IndexBulkDeleteCallback callback, void *callback_state)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	return stats;
}

static IndexBulkDeleteResult *
dbe_pgm_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	DbePgmMeta	meta;

	if (info->analyze_only)
		return stats;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	if (dbe_pgm_read_meta(info->index, &meta))
		stats->num_index_tuples = (double) (meta.nrows + meta.delta_rows);

	return stats;
}

/* ------------------------------------------------------------------------
 * Scan
 * ------------------------------------------------------------------------ */

static IndexScanDesc
dbe_pgm_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	DbePgmScanOpaque so;

	so = (DbePgmScanOpaque) palloc0(sizeof(DbePgmScanOpaqueData));
	so->loaded = false;
	scan->opaque = so;
	return scan;
}

static void
dbe_pgm_amendscan(IndexScanDesc scan)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;

	if (so == NULL)
		return;
	if (so->model != NULL)
	{
		pfree(so->model);
		so->model = NULL;
	}
	pfree(so);
	scan->opaque = NULL;
}

/*
 * Traduce las claves de búsqueda a un rango [lo, hi) del arreglo de filas.
 *
 * Cada cláusula acota por un lado; se intersecan.  Como las filas están en
 * orden de clave, cualquier combinación de <, <=, =, >=, > es un rango
 * contiguo — que es exactamente de dónde sale la ventaja del método.
 */
static void
dbe_pgm_compute_range(IndexScanDesc scan)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	Oid			keytype = TupleDescAttr(RelationGetDescr(index), 0)->atttypid;
	int			i;

	so->lo = 0;
	so->hi = so->meta.nrows;
	so->empty = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int64		k;
		uint64		rank;
		uint64		bound;

		if (key->sk_flags & SK_ISNULL)
		{
			so->empty = true;	/* nada indexado es NULL */
			return;
		}

		/*
		 * El argumento se interpreta con sk_subtype, no con el tipo de la
		 * columna: con los operadores cruzados de la familia, `id = 5000`
		 * sobre un bigint trae un integer del otro lado.  Leer 4 bytes como
		 * 8 daría una clave inventada.
		 */
		k = dbe_pgm_key_to_int64(OidIsValid(key->sk_subtype)
								 ? key->sk_subtype : keytype,
								 key->sk_argument);
		rank = dbe_pgm_lower_bound(index, &so->meta, so->model, k, NULL);

		switch (key->sk_strategy)
		{
			case BTLessStrategyNumber:	/* col < k */
				bound = dbe_pgm_start_at(index, &so->meta, rank);
				if (bound < so->hi)
					so->hi = bound;
				break;

			case BTLessEqualStrategyNumber:		/* col <= k */
				if (rank < so->meta.ndistinct &&
					dbe_pgm_key_at(index, &so->meta, rank) == k)
					rank++;
				bound = dbe_pgm_start_at(index, &so->meta, rank);
				if (bound < so->hi)
					so->hi = bound;
				break;

			case BTEqualStrategyNumber: /* col = k */
				if (rank >= so->meta.ndistinct ||
					dbe_pgm_key_at(index, &so->meta, rank) != k)
				{
					so->empty = true;
					return;
				}
				bound = dbe_pgm_start_at(index, &so->meta, rank);
				if (bound > so->lo)
					so->lo = bound;
				bound = dbe_pgm_start_at(index, &so->meta, rank + 1);
				if (bound < so->hi)
					so->hi = bound;
				break;

			case BTGreaterEqualStrategyNumber:	/* col >= k */
				bound = dbe_pgm_start_at(index, &so->meta, rank);
				if (bound > so->lo)
					so->lo = bound;
				break;

			case BTGreaterStrategyNumber:		/* col > k */
				if (rank < so->meta.ndistinct &&
					dbe_pgm_key_at(index, &so->meta, rank) == k)
					rank++;
				bound = dbe_pgm_start_at(index, &so->meta, rank);
				if (bound > so->lo)
					so->lo = bound;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("dbengine: estrategia %d no soportada por el índice PGM",
								key->sk_strategy)));
		}
	}

	if (so->lo >= so->hi)
		so->empty = true;
}

static void
dbe_pgm_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
				 ScanKey orderbys, int norderbys)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;

	if (keys != NULL && nkeys > 0)
		memcpy(scan->keyData, keys, nkeys * sizeof(ScanKeyData));
	scan->numberOfKeys = nkeys;

	if (!so->loaded)
	{
		if (!dbe_pgm_read_meta(scan->indexRelation, &so->meta))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("dbengine: el índice PGM no está construido"),
					 errhint("Corré REINDEX.")));

		so->model = (char *) palloc(Max(so->meta.model_len, 1));
		if (so->meta.model_len > 0)
			dbe_storage_read(scan->indexRelation, so->meta.model_off,
							 so->model, so->meta.model_len);
		so->loaded = true;
	}

	/* Para que pg_stat_user_indexes cuente los scans como los demás AM. */
	pgstat_count_index_scan(scan->indexRelation);

	dbe_pgm_compute_range(scan);
	so->next = so->lo;
	so->delta_chunk = so->meta.delta_head;
	so->delta_pos = 0;
	so->delta_done = (so->meta.delta_head == 0);
}

/* ¿La entrada del delta cumple todas las cláusulas? */
static bool
dbe_pgm_delta_matches(IndexScanDesc scan, int64 key)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;
	Oid			keytype = TupleDescAttr(RelationGetDescr(scan->indexRelation),
									   0)->atttypid;
	int			i;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		sk = &scan->keyData[i];
		int64		k;

		if (sk->sk_flags & SK_ISNULL)
			return false;

		k = dbe_pgm_key_to_int64(OidIsValid(sk->sk_subtype)
								 ? sk->sk_subtype : keytype,
								 sk->sk_argument);

		switch (sk->sk_strategy)
		{
			case BTLessStrategyNumber:
				if (!(key < k))
					return false;
				break;
			case BTLessEqualStrategyNumber:
				if (!(key <= k))
					return false;
				break;
			case BTEqualStrategyNumber:
				if (key != k)
					return false;
				break;
			case BTGreaterEqualStrategyNumber:
				if (!(key >= k))
					return false;
				break;
			case BTGreaterStrategyNumber:
				if (!(key > k))
					return false;
				break;
			default:
				return false;
		}
	}
	(void) so;
	return true;
}

/*
 * Próxima fila del delta que cumple.  Devuelve false cuando se acabó.
 */
static bool
dbe_pgm_delta_next(IndexScanDesc scan, uint64 *rownum_out)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;

	while (!so->delta_done)
	{
		DbePgmDeltaChunk chunk;

		dbe_storage_read(index, so->delta_chunk, (char *) &chunk, sizeof(chunk));

		while (so->delta_pos < chunk.used)
		{
			DbePgmDeltaEntry entry;
			uint64		off = so->delta_chunk + sizeof(DbePgmDeltaChunk) +
				(uint64) so->delta_pos * sizeof(DbePgmDeltaEntry);

			dbe_storage_read(index, off, (char *) &entry, sizeof(entry));
			so->delta_pos++;

			if (dbe_pgm_delta_matches(scan, entry.key))
			{
				*rownum_out = entry.rownum;
				return true;
			}
		}

		if (chunk.next == 0)
		{
			so->delta_done = true;
			return false;
		}
		so->delta_chunk = chunk.next;
		so->delta_pos = 0;
	}
	return false;
}

static bool
dbe_pgm_amgettuple(IndexScanDesc scan, ScanDirection dir)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;
	uint64		rownum;

	if (dir != ForwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: el índice PGM solo recorre hacia adelante")));

	/* Los TIDs no cambian: el storage es append-only. */
	scan->xs_recheck = false;

	if (!so->empty && so->next < so->hi)
	{
		rownum = dbe_pgm_row_at(scan->indexRelation, &so->meta, so->next);
		so->next++;
		dbe_tid_from_rownum(rownum, &scan->xs_heaptid);
		return true;
	}

	if (dbe_pgm_delta_next(scan, &rownum))
	{
		dbe_tid_from_rownum(rownum, &scan->xs_heaptid);
		return true;
	}

	return false;
}

static int64
dbe_pgm_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	DbePgmScanOpaque so = (DbePgmScanOpaque) scan->opaque;
	int64		ntids = 0;
	uint64		rownum;

	if (!so->empty)
	{
		while (so->next < so->hi)
		{
			ItemPointerData tid;

			rownum = dbe_pgm_row_at(scan->indexRelation, &so->meta, so->next);
			so->next++;
			dbe_tid_from_rownum(rownum, &tid);
			tbm_add_tuples(tbm, &tid, 1, false);
			ntids++;
		}
	}

	while (dbe_pgm_delta_next(scan, &rownum))
	{
		ItemPointerData tid;

		dbe_tid_from_rownum(rownum, &tid);
		tbm_add_tuples(tbm, &tid, 1, false);
		ntids++;
	}

	return ntids;
}

/* ------------------------------------------------------------------------
 * Costo
 * ------------------------------------------------------------------------ */

static void
dbe_pgm_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					   Cost *indexStartupCost, Cost *indexTotalCost,
					   Selectivity *indexSelectivity, double *indexCorrelation,
					   double *indexPages)
{
	GenericCosts costs = {0};
	DbePgmMeta	meta;
	Relation	index;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * La búsqueda no baja un árbol: son dos o tres multiplicaciones y una
	 * binaria acotada a 2ε+1 posiciones.  Lo que se lee son las páginas de
	 * esa ventana, no un camino de raíz a hoja.
	 *
	 * El descuento se aplica al costo de arranque, que es donde el genérico
	 * cobra el descenso; el de recorrer las coincidencias queda igual,
	 * porque eso sí es leer.
	 */
	index = index_open(path->indexinfo->indexoid, AccessShareLock);
	if (dbe_pgm_read_meta(index, &meta))
	{
		double		window_pages;
		double		spc_random_page_cost;

		get_tablespace_page_costs(index->rd_rel->reltablespace,
								  &spc_random_page_cost, NULL);

		window_pages = ceil((double) (2 * meta.epsilon + 1) * sizeof(int64) /
							BLCKSZ) + 1.0;
		costs.indexStartupCost = window_pages * spc_random_page_cost;

		/*
		 * El delta se recorre entero en cada scan: mientras esté chico no
		 * se nota, y cuando crece el planner tiene que verlo.
		 */
		costs.indexTotalCost += cpu_index_tuple_cost * (double) meta.delta_rows;
	}
	index_close(index, AccessShareLock);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
dbe_pgmindex_init(void)
{
	/*
	 * Nada que registrar en tiempo de carga: el AM entra por su handler,
	 * igual que el Table AM.  La función existe para dejar el punto de
	 * enganche donde va a hacer falta cuando el índice tenga estructuras
	 * en memoria compartida.
	 */
}
