/*-------------------------------------------------------------------------
 *
 * catalog.c
 *	  Acceso a dbengine.stripe y dbengine.stripe_column.  Ver catalog.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/procarray.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "dbengine/catalog.h"
#include "dbengine/dbengine.h"

/* Columnas de dbengine.stripe */
#define Anum_stripe_relfilenode		1
#define Anum_stripe_stripe_id		2
#define Anum_stripe_first_row		3
#define Anum_stripe_row_count		4
#define Anum_stripe_data_offset		5
#define Anum_stripe_data_length		6
#define Anum_stripe_insert_xid		7
#define Anum_stripe_insert_cid		8
#define Anum_stripe_insert_cid_max	9
#define Anum_stripe_commit_ts		10
#define Natts_stripe				10

/* Columnas de dbengine.stripe_column */
#define Anum_seg_relfilenode				1
#define Anum_seg_stripe_id			2
#define Anum_seg_attnum				3
#define Anum_seg_offset				4
#define Anum_seg_length				5
#define Anum_seg_null_count			6
#define Anum_seg_encoding			7
#define Anum_seg_min_value			8
#define Anum_seg_max_value			9
#define Anum_seg_bloom				10
#define Anum_seg_granules			11
#define Natts_seg					11

/*
 * Verifica que la tabla del catálogo tenga la forma que este código espera.
 *
 * Los arrays de values/nulls se dimensionan con las constantes Natts_*; si
 * la tabla instalada tuviera menos columnas, heap_deform_tuple() llenaría
 * solo una parte y el resto se leería del stack sin inicializar.  El
 * síntoma es un segfault a distancia, imposible de rastrear.  Pasa de
 * verdad: alcanza con una extensión vieja que quedó sin recrear después de
 * cambiar el esquema.
 */
static void
dbe_check_catalog_shape(Relation rel, int expected)
{
	if (RelationGetDescr(rel)->natts != expected)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dbengine: el catálogo \"%s\" tiene %d columnas y este "
						"binario espera %d",
						RelationGetRelationName(rel),
						RelationGetDescr(rel)->natts, expected),
				 errhint("La extensión instalada no coincide con la biblioteca. "
						 "Recreala con DROP EXTENSION dbengine CASCADE; "
						 "CREATE EXTENSION dbengine;")));
}

/* bytea a partir de bytes crudos; no hay helper en el core para esto. */
static bytea *
dbe_make_bytea(const char *data, uint32 len)
{
	bytea	   *b = (bytea *) palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), data, len);
	return b;
}

static Oid
dbe_namespace_oid(void)
{
	return get_namespace_oid("dbengine", false);
}

static Oid
dbe_lookup_relid(const char *name)
{
	Oid			relid = get_relname_relid(name, dbe_namespace_oid());

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("dbengine: falta el catálogo interno \"dbengine.%s\"",
						name),
				 errhint("¿Se creó la extensión con CREATE EXTENSION dbengine?")));
	return relid;
}

Oid
dbe_stripe_relid(void)
{
	return dbe_lookup_relid("stripe");
}

Oid
dbe_stripe_pkey_relid(void)
{
	return dbe_lookup_relid("stripe_pkey");
}

Oid
dbe_stripe_column_relid(void)
{
	return dbe_lookup_relid("stripe_column");
}

static Oid
dbe_relation_order_relid(void)
{
	return dbe_lookup_relid("relation_order");
}

static Oid
dbe_relation_order_pkey_relid(void)
{
	return dbe_lookup_relid("relation_order_pkey");
}

Oid
dbe_stripe_column_pkey_relid(void)
{
	return dbe_lookup_relid("stripe_column_pkey");
}

/*
 * Regla de visibilidad de un stripe (ADR-002).
 *
 * Los stripes son append-only: no hay xmax.  Esta es, entonces, la
 * definición completa de visibilidad, y coincide con la mitad xmin de
 * HeapTupleSatisfiesMVCC.
 */
/*
 * Cuándo commiteó el stripe.
 *
 * En un stripe normal el xid sigue ahí, así que se lo pregunta a
 * pg_commit_ts: guardar el timestamp en cada stripe sería duplicar lo que
 * Postgres ya lleva.  El caso en que sí hace falta guardarlo es la fusión,
 * que escribe el resultado congelado y con eso pierde el xid original.
 */
bool
dbe_stripe_commit_ts(const DbeStripeDesc *stripe, TimestampTz *out)
{
	TransactionId xid;
	TimestampTz ts;

	if (stripe->has_commit_ts)
	{
		*out = stripe->commit_ts;
		return true;
	}

	xid = XidFromFullTransactionId(stripe->insert_xid);
	if (!TransactionIdIsNormal(xid))
		return false;			/* congelado sin timestamp guardado */

	/*
	 * TransactionIdGetCommitTsData() NO devuelve false con
	 * track_commit_timestamp apagado: tira ERROR.  Sin este guardia,
	 * merge_stripes() aborta en cualquier cluster que no lo tenga
	 * encendido, con un mensaje que no menciona a dbengine.
	 */
	if (!track_commit_timestamp)
		return false;

	if (!TransactionIdGetCommitTsData(xid, &ts, NULL))
		return false;			/* truncado por vacuum de pg_commit_ts */

	*out = ts;
	return true;
}

DbeStripeVisibility
dbe_stripe_visible(const DbeStripeDesc *stripe, Snapshot snapshot)
{
	TransactionId xid = XidFromFullTransactionId(stripe->insert_xid);

	if (!TransactionIdIsValid(xid) || snapshot == NULL)
		return DBE_STRIPE_NONE;

	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
			/*
			 * "Todo lo que exista físicamente".  Lo usa ExecModifyTable
			 * para traer la fila vieja, y el index build no concurrente.
			 * curcid ni siquiera está inicializado en este snapshot, así
			 * que aplicarle la regla MVCC daría basura.
			 */
			return DBE_STRIPE_ALL;

		case SNAPSHOT_SELF:
			return (TransactionIdIsCurrentTransactionId(xid) ||
					TransactionIdDidCommit(xid))
				? DBE_STRIPE_ALL : DBE_STRIPE_NONE;

		case SNAPSHOT_DIRTY:
			/* Ve también lo no commiteado, mientras no haya abortado. */
			return (TransactionIdIsCurrentTransactionId(xid) ||
					TransactionIdIsInProgress(xid) ||
					TransactionIdDidCommit(xid))
				? DBE_STRIPE_ALL : DBE_STRIPE_NONE;

		case SNAPSHOT_NON_VACUUMABLE:

			/*
			 * "¿Alguien podría necesitar todavía esta fila?".  Lo usa
			 * get_actual_variable_range() para estimar selectividad: hace un
			 * index scan de verdad sobre la tabla para encontrar el mínimo o
			 * el máximo real.
			 *
			 * InitNonVacuumableSnapshot() llena solo el tipo y el
			 * GlobalVisState; xmin, xip y xcnt quedan sin inicializar.
			 * Aplicarle la regla MVCC es leer basura: el síntoma era un
			 * segfault en planning adentro de XidInMVCCSnapshot(), que
			 * aparecía únicamente cuando había estadísticas suficientes para
			 * llegar a ineq_histogram_selectivity().
			 *
			 * Los stripes son append-only y no tienen xmax, así que ninguno
			 * es recuperable mientras la transacción que lo escribió no haya
			 * abortado.
			 */
			return (TransactionIdIsCurrentTransactionId(xid) ||
					TransactionIdIsInProgress(xid) ||
					TransactionIdDidCommit(xid))
				? DBE_STRIPE_ALL : DBE_STRIPE_NONE;

		case SNAPSHOT_TOAST:
		case SNAPSHOT_HISTORIC_MVCC:

			/*
			 * Ninguno de los dos llega a una tabla columnar: el TOAST de
			 * nuestros catálogos es heap y la decodificación lógica no
			 * soporta este AM.  Se contesta lo único que se puede afirmar
			 * sin mirar campos que quizás no estén llenos.
			 */
			return TransactionIdDidCommit(xid)
				? DBE_STRIPE_ALL : DBE_STRIPE_NONE;

		case SNAPSHOT_MVCC:
			/* Sigue abajo.  Sin default: que un tipo nuevo dé warning. */
			break;
	}

	/*
	 * Regla MVCC (ADR-002).  Los stripes son append-only: sin xmax, esta
	 * es la definición completa de visibilidad.
	 */

	/*
	 * Escrito por mí.  Un stripe junta filas de varios comandos, así que
	 * acá se decide entre las tres respuestas: todo visible, nada visible,
	 * o hay que mirar el command id de cada fila.
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		if (stripe->insert_cid_max < snapshot->curcid)
			return DBE_STRIPE_ALL;
		if (stripe->insert_cid >= snapshot->curcid)
			return DBE_STRIPE_NONE;
		return DBE_STRIPE_PARTIAL;
	}

	/*
	 * De otra transacción: commitea entera o no commitea, así que el
	 * command id no juega.
	 */

	/*
	 * Más viejo que cualquier transacción viva para este backend: terminó
	 * antes de que se tomara ningún snapshot que tengamos, así que no puede
	 * estar "en curso" para ninguno y el clog ya tiene la respuesta final.
	 *
	 * El atajo no es solo por velocidad.  XidInMVCCSnapshot() consulta
	 * pg_subtrans cuando el snapshot desbordó de subtransacciones, y para
	 * un xid tan viejo esa información ya puede no existir: en una build con
	 * assertions salta el Assert de subtrans.c, y sin assertions se lee una
	 * página de SLRU que no está.  heap no tiene el problema porque para
	 * entonces la tupla ya tiene el hint bit de xmin commiteado; acá no hay
	 * hint bits, así que hay que hacer el chequeo a mano.
	 */
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return TransactionIdDidCommit(xid) ? DBE_STRIPE_ALL : DBE_STRIPE_NONE;

	/* En curso, o commiteado después de que tomé el snapshot. */
	if (XidInMVCCSnapshot(xid, snapshot))
		return DBE_STRIPE_NONE;

	return TransactionIdDidCommit(xid) ? DBE_STRIPE_ALL : DBE_STRIPE_NONE;
}

void
dbe_catalog_insert_stripe(RelFileNumber relnumber, const DbeStripeDesc *stripe)
{
	Relation	rel;
	Datum		values[Natts_stripe];
	bool		nulls[Natts_stripe];
	HeapTuple	tup;

	memset(nulls, false, sizeof(nulls));

	values[Anum_stripe_relfilenode - 1] = ObjectIdGetDatum(relnumber);
	values[Anum_stripe_stripe_id - 1] = Int64GetDatum((int64) stripe->stripe_id);
	values[Anum_stripe_first_row - 1] = Int64GetDatum((int64) stripe->first_row_number);
	values[Anum_stripe_row_count - 1] = Int64GetDatum((int64) stripe->row_count);
	values[Anum_stripe_data_offset - 1] = Int64GetDatum((int64) stripe->data_offset);
	values[Anum_stripe_data_length - 1] = Int64GetDatum((int64) stripe->data_length);
	values[Anum_stripe_insert_xid - 1] = FullTransactionIdGetDatum(stripe->insert_xid);
	values[Anum_stripe_insert_cid - 1] = Int32GetDatum((int32) stripe->insert_cid);
	values[Anum_stripe_insert_cid_max - 1] = Int32GetDatum((int32) stripe->insert_cid_max);

	/*
	 * commit_ts solo lo llena la fusión.  En un stripe normal queda NULL y
	 * el momento se le pregunta a pg_commit_ts a partir del xid, que sigue
	 * ahí; guardarlo en cada stripe sería duplicar lo que Postgres ya tiene.
	 */
	if (stripe->has_commit_ts)
		values[Anum_stripe_commit_ts - 1] = TimestampTzGetDatum(stripe->commit_ts);
	else
		nulls[Anum_stripe_commit_ts - 1] = true;

	rel = table_open(dbe_stripe_relid(), RowExclusiveLock);
	dbe_check_catalog_shape(rel, Natts_stripe);
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

void
dbe_catalog_insert_segment(RelFileNumber relnumber, uint64 stripe_id, const DbeSegDesc *seg)
{
	Relation	rel;
	Datum		values[Natts_seg];
	bool		nulls[Natts_seg];
	HeapTuple	tup;

	memset(nulls, false, sizeof(nulls));

	values[Anum_seg_relfilenode - 1] = ObjectIdGetDatum(relnumber);
	values[Anum_seg_stripe_id - 1] = Int64GetDatum((int64) stripe_id);
	values[Anum_seg_attnum - 1] = Int16GetDatum(seg->attnum);
	values[Anum_seg_offset - 1] = Int64GetDatum((int64) seg->seg_offset);
	values[Anum_seg_length - 1] = Int64GetDatum((int64) seg->seg_length);
	values[Anum_seg_null_count - 1] = Int64GetDatum((int64) seg->null_count);
	values[Anum_seg_encoding - 1] = Int16GetDatum(seg->encoding);

	if (seg->has_minmax)
	{
		values[Anum_seg_min_value - 1] =
			PointerGetDatum(dbe_make_bytea(seg->min_value, seg->min_length));
		values[Anum_seg_max_value - 1] =
			PointerGetDatum(dbe_make_bytea(seg->max_value, seg->max_length));
	}
	else
	{
		nulls[Anum_seg_min_value - 1] = true;
		nulls[Anum_seg_max_value - 1] = true;
	}

	if (seg->bloom != NULL && seg->bloom_length > 0)
		values[Anum_seg_bloom - 1] =
			PointerGetDatum(dbe_make_bytea(seg->bloom, seg->bloom_length));
	else
		nulls[Anum_seg_bloom - 1] = true;

	if (seg->granules != NULL && seg->granules_length > 0)
		values[Anum_seg_granules - 1] =
			PointerGetDatum(dbe_make_bytea(seg->granules, seg->granules_length));
	else
		nulls[Anum_seg_granules - 1] = true;

	rel = table_open(dbe_stripe_column_relid(), RowExclusiveLock);
	dbe_check_catalog_shape(rel, Natts_seg);
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * Lee los stripes de `relid` visibles bajo `snapshot`.
 *
 * El scan del catálogo usa SnapshotSelf y la visibilidad real la decide
 * dbe_stripe_visible(): ver ADR-002 para por qué no alcanza con delegarla
 * en el MVCC de la fila de metadatos.
 */
static int
dbe_stripe_cmp(const ListCell *a, const ListCell *b)
{
	uint64		x = ((const DbeStripeDesc *) lfirst(a))->first_row_number;
	uint64		y = ((const DbeStripeDesc *) lfirst(b))->first_row_number;

	if (x < y)
		return -1;
	return (x > y) ? 1 : 0;
}

List *
dbe_catalog_list_stripes(RelFileNumber relnumber, Snapshot snapshot)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_stripe_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));

	rel = table_open(dbe_stripe_relid(), AccessShareLock);
	dbe_check_catalog_shape(rel, Natts_stripe);
	scan = systable_beginscan(rel, dbe_stripe_pkey_relid(), true,
							  SnapshotSelf, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Datum		values[Natts_stripe];
		bool		nulls[Natts_stripe];
		DbeStripeDesc *s;

		heap_deform_tuple(tup, RelationGetDescr(rel), values, nulls);

		s = (DbeStripeDesc *) palloc(sizeof(DbeStripeDesc));
		s->stripe_id = (uint64) DatumGetInt64(values[Anum_stripe_stripe_id - 1]);
		s->first_row_number = (uint64) DatumGetInt64(values[Anum_stripe_first_row - 1]);
		s->row_count = (uint64) DatumGetInt64(values[Anum_stripe_row_count - 1]);
		s->data_offset = (uint64) DatumGetInt64(values[Anum_stripe_data_offset - 1]);
		s->data_length = (uint64) DatumGetInt64(values[Anum_stripe_data_length - 1]);
		s->insert_xid = DatumGetFullTransactionId(values[Anum_stripe_insert_xid - 1]);
		s->insert_cid = (CommandId) DatumGetInt32(values[Anum_stripe_insert_cid - 1]);
		s->insert_cid_max = (CommandId) DatumGetInt32(values[Anum_stripe_insert_cid_max - 1]);
		s->has_commit_ts = !nulls[Anum_stripe_commit_ts - 1];
		if (s->has_commit_ts)
			s->commit_ts = DatumGetTimestampTz(values[Anum_stripe_commit_ts - 1]);

		if (dbe_stripe_visible(s, snapshot) == DBE_STRIPE_NONE)
		{
			pfree(s);
			continue;
		}

		result = lappend(result, s);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Ordenados por número de fila, no por stripe_id.
	 *
	 * No es lo mismo: dos transacciones concurrentes reservan sus números
	 * de fila en un orden y flushean en otro, así que el stripe con id
	 * menor puede tener filas posteriores.  Devolverlos en orden de
	 * stripe_id haría que el scan entregue TIDs desordenados, y la
	 * construcción de índices los necesita crecientes — GIN se rompe con
	 * eso.
	 */
	list_sort(result, dbe_stripe_cmp);

	return result;
}

void
dbe_catalog_load_segments(RelFileNumber relnumber, uint64 stripe_id, int natts,
						  const bool *wanted, DbeSegDesc *out, bool *found)
{
	dbe_catalog_load_segments_ext(relnumber, stripe_id, natts, wanted, NULL,
								  out, found);
}

void
dbe_catalog_load_segments_ext(RelFileNumber relnumber, uint64 stripe_id,
							  int natts, const bool *wanted,
							  const bool *want_zone, DbeSegDesc *out,
							  bool *found)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;
	int			i;

	for (i = 0; i < natts; i++)
		found[i] = false;

	ScanKeyInit(&key[0], Anum_seg_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	ScanKeyInit(&key[1], Anum_seg_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripe_id));

	rel = table_open(dbe_stripe_column_relid(), AccessShareLock);
	dbe_check_catalog_shape(rel, Natts_seg);
	scan = systable_beginscan(rel, dbe_stripe_column_pkey_relid(), true,
							  SnapshotSelf, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Datum		values[Natts_seg];
		bool		nulls[Natts_seg];
		AttrNumber	attnum;

		heap_deform_tuple(tup, RelationGetDescr(rel), values, nulls);
		attnum = DatumGetInt16(values[Anum_seg_attnum - 1]);

		if (attnum < 1 || attnum > natts)
			continue;
		/* Column pruning: ni siquiera anotamos las columnas que no se piden. */
		if (wanted != NULL && !wanted[attnum - 1])
			continue;

		out[attnum - 1].attnum = attnum;
		out[attnum - 1].seg_offset = (uint64) DatumGetInt64(values[Anum_seg_offset - 1]);
		out[attnum - 1].seg_length = (uint64) DatumGetInt64(values[Anum_seg_length - 1]);
		out[attnum - 1].null_count = (uint64) DatumGetInt64(values[Anum_seg_null_count - 1]);
		out[attnum - 1].encoding = DatumGetInt16(values[Anum_seg_encoding - 1]);
		out[attnum - 1].has_minmax = !nulls[Anum_seg_min_value - 1];

		if (out[attnum - 1].has_minmax)
		{
			bytea	   *mn = DatumGetByteaPP(values[Anum_seg_min_value - 1]);
			bytea	   *mx = DatumGetByteaPP(values[Anum_seg_max_value - 1]);

			/*
			 * Se copian: la tupla del catálogo muere con el systable scan y
			 * el zone map se usa después, para decidir si el stripe se lee.
			 */
			out[attnum - 1].min_length = (uint32) VARSIZE_ANY_EXHDR(mn);
			out[attnum - 1].min_value = (const char *)
				memcpy(palloc(out[attnum - 1].min_length), VARDATA_ANY(mn),
					   out[attnum - 1].min_length);
			out[attnum - 1].max_length = (uint32) VARSIZE_ANY_EXHDR(mx);
			out[attnum - 1].max_value = (const char *)
				memcpy(palloc(out[attnum - 1].max_length), VARDATA_ANY(mx),
					   out[attnum - 1].max_length);
		}

		/*
		 * El filtro de Bloom solo se trae si alguien lo pidió: es un bytea
		 * grande y detoastearlo en cada scan le cobraría a todas las
		 * consultas para servirle a las pocas que tienen un predicado de
		 * igualdad sobre esa columna.
		 */
		if (want_zone != NULL && want_zone[attnum - 1] &&
			!nulls[Anum_seg_bloom - 1])
		{
			bytea	   *bl = DatumGetByteaPP(values[Anum_seg_bloom - 1]);

			out[attnum - 1].bloom_length = (uint32) VARSIZE_ANY_EXHDR(bl);
			out[attnum - 1].bloom = (const char *)
				memcpy(palloc(out[attnum - 1].bloom_length), VARDATA_ANY(bl),
					   out[attnum - 1].bloom_length);
		}

		/* El zone map por gránulo viaja con las mismas reglas. */
		if (want_zone != NULL && want_zone[attnum - 1] &&
			!nulls[Anum_seg_granules - 1])
		{
			bytea	   *gr = DatumGetByteaPP(values[Anum_seg_granules - 1]);

			out[attnum - 1].granules_length = (uint32) VARSIZE_ANY_EXHDR(gr);
			out[attnum - 1].granules = (const char *)
				memcpy(palloc(out[attnum - 1].granules_length), VARDATA_ANY(gr),
					   out[attnum - 1].granules_length);
		}

		found[attnum - 1] = true;
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);
}

/*
 * Copia los metadatos de un relfilenode a otro.
 *
 * Hace falta en ALTER TABLE ... SET TABLESPACE, que copia el storage a un
 * relfilenode nuevo sin pasar por el camino de reescritura.
 */
void
dbe_catalog_copy_relation(RelFileNumber from, RelFileNumber to)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;
	List	   *copies = NIL;
	ListCell   *lc;

	ScanKeyInit(&key[0], Anum_stripe_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(from));
	rel = table_open(dbe_stripe_relid(), RowExclusiveLock);
	scan = systable_beginscan(rel, dbe_stripe_pkey_relid(), true,
							  SnapshotSelf, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		copies = lappend(copies, heap_copytuple(tup));
	systable_endscan(scan);

	foreach(lc, copies)
	{
		HeapTuple	src = (HeapTuple) lfirst(lc);
		Datum		values[Natts_stripe];
		bool		nulls[Natts_stripe];
		HeapTuple	newtup;

		heap_deform_tuple(src, RelationGetDescr(rel), values, nulls);
		values[Anum_stripe_relfilenode - 1] = ObjectIdGetDatum(to);
		newtup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, newtup);
		heap_freetuple(newtup);
	}
	list_free_deep(copies);
	table_close(rel, RowExclusiveLock);

	copies = NIL;
	ScanKeyInit(&key[0], Anum_seg_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(from));
	rel = table_open(dbe_stripe_column_relid(), RowExclusiveLock);
	scan = systable_beginscan(rel, dbe_stripe_column_pkey_relid(), true,
							  SnapshotSelf, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		copies = lappend(copies, heap_copytuple(tup));
	systable_endscan(scan);

	foreach(lc, copies)
	{
		HeapTuple	src = (HeapTuple) lfirst(lc);
		Datum		values[Natts_seg];
		bool		nulls[Natts_seg];
		HeapTuple	newtup;

		heap_deform_tuple(src, RelationGetDescr(rel), values, nulls);
		values[Anum_seg_relfilenode - 1] = ObjectIdGetDatum(to);
		newtup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, newtup);
		heap_freetuple(newtup);
	}
	list_free_deep(copies);
	table_close(rel, RowExclusiveLock);

	CommandCounterIncrement();
}

/*
 * Ubica el segmento de command ids (attnum 0).  Solo existe si el stripe
 * abarca más de un comando.
 */
bool
dbe_catalog_load_cid_segment(RelFileNumber relnumber, uint64 stripe_id,
							 DbeSegDesc *out)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	tup;
	bool		found = false;

	ScanKeyInit(&key[0], Anum_seg_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	ScanKeyInit(&key[1], Anum_seg_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripe_id));
	ScanKeyInit(&key[2], Anum_seg_attnum, BTEqualStrategyNumber,
				F_INT2EQ, Int16GetDatum((int16) DBE_ATTNUM_CID));

	rel = table_open(dbe_stripe_column_relid(), AccessShareLock);
	dbe_check_catalog_shape(rel, Natts_seg);
	scan = systable_beginscan(rel, dbe_stripe_column_pkey_relid(), true,
							  SnapshotSelf, 3, key);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Datum		values[Natts_seg];
		bool		nulls[Natts_seg];

		heap_deform_tuple(tup, RelationGetDescr(rel), values, nulls);
		out->attnum = DBE_ATTNUM_CID;
		out->seg_offset = (uint64) DatumGetInt64(values[Anum_seg_offset - 1]);
		out->seg_length = (uint64) DatumGetInt64(values[Anum_seg_length - 1]);
		out->null_count = 0;
		out->encoding = DatumGetInt16(values[Anum_seg_encoding - 1]);
		out->has_minmax = false;
		found = true;
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}

void
dbe_catalog_delete_stripe(RelFileNumber relnumber, uint64 stripe_id)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	ScanKeyInit(&key[0], Anum_seg_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	ScanKeyInit(&key[1], Anum_seg_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripe_id));
	rel = table_open(dbe_stripe_column_relid(), RowExclusiveLock);
	dbe_check_catalog_shape(rel, Natts_seg);
	scan = systable_beginscan(rel, dbe_stripe_column_pkey_relid(), true,
							  NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	ScanKeyInit(&key[0], Anum_stripe_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	ScanKeyInit(&key[1], Anum_stripe_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripe_id));
	rel = table_open(dbe_stripe_relid(), RowExclusiveLock);
	dbe_check_catalog_shape(rel, Natts_stripe);
	scan = systable_beginscan(rel, dbe_stripe_pkey_relid(), true,
							  NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

/* ------------------------------------------------------------------------
 * Orden físico de las filas
 *
 * pg_index.indisclustered dice con qué índice se corrió CLUSTER la última
 * vez, NO que los datos sigan en ese orden: un INSERT posterior lo rompe y
 * el flag queda igual.  Como el planner necesita lo segundo para poder
 * confiar en un pathkey, se lleva aparte.
 *
 * La marca se escribe una sola vez, al final de CLUSTER, y la borra
 * cualquier escritura.  Es deliberadamente conservadora: perderla de más
 * cuesta un Sort, y no perderla cuando había que perderla da resultados
 * mal ordenados.
 * ------------------------------------------------------------------------ */
void
dbe_catalog_set_order(RelFileNumber relnumber, Oid indexrelid)
{
	Relation	rel;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tup;

	dbe_catalog_clear_order(relnumber);

	values[0] = ObjectIdGetDatum((Oid) relnumber);
	values[1] = ObjectIdGetDatum(indexrelid);

	rel = table_open(dbe_relation_order_relid(), RowExclusiveLock);
	dbe_check_catalog_shape(rel, 2);
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

void
dbe_catalog_clear_order(RelFileNumber relnumber)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;

	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((Oid) relnumber));
	rel = table_open(dbe_relation_order_relid(), RowExclusiveLock);
	scan = systable_beginscan(rel, dbe_relation_order_pkey_relid(), true,
							  NULL, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

Oid
dbe_catalog_get_order(RelFileNumber relnumber)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;
	Oid			result = InvalidOid;

	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((Oid) relnumber));
	rel = table_open(dbe_relation_order_relid(), AccessShareLock);
	scan = systable_beginscan(rel, dbe_relation_order_pkey_relid(), true,
							  NULL, 1, key);
	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		d = heap_getattr(tup, 2, RelationGetDescr(rel), &isnull);

		if (!isnull)
			result = DatumGetObjectId(d);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

void
dbe_catalog_drop_relation(RelFileNumber relnumber)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;

	/* Segmentos primero. */
	ScanKeyInit(&key[0], Anum_seg_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	rel = table_open(dbe_stripe_column_relid(), RowExclusiveLock);
	scan = systable_beginscan(rel, dbe_stripe_column_pkey_relid(), true,
							  NULL, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	ScanKeyInit(&key[0], Anum_stripe_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));
	rel = table_open(dbe_stripe_relid(), RowExclusiveLock);
	scan = systable_beginscan(rel, dbe_stripe_pkey_relid(), true,
							  NULL, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	dbe_catalog_clear_order(relnumber);
}

/* ------------------------------------------------------------------------
 * Limpieza en DROP TABLE.
 *
 * El Table AM no tiene callback de DROP.  PostgreSQL borra el archivo por
 * smgr, que es genérico y no sabe nada del AM, así que las filas de
 * dbengine.stripe y dbengine.stripe_column de la tabla borrada quedaban
 * para siempre.  Medido sobre el banco de ClickBench: 7 relfilenode
 * muertos, 2.597 filas de stripe y 272.685 de stripe_column.
 *
 * El daño directo es bloat, no corrupción: el caso peligroso — que otra
 * tabla reciba ese mismo relfilenode y lea stripes ajenos — ya lo tapa
 * relation_set_new_filelocator(), que limpia el relfilenode nuevo antes de
 * escribir nada en él.  Pero dejar la única defensa ahí es más frágil de
 * lo necesario, y el catálogo crece sin techo con cualquier churn de
 * tablas.
 *
 * object_access_hook es el punto correcto: se dispara dentro de
 * deleteOneObject(), con la relación todavía en pg_class y el
 * AccessExclusiveLock del DROP ya tomado.
 * ------------------------------------------------------------------------ */

static object_access_hook_type prev_object_access_hook = NULL;

static void
dbe_object_access(ObjectAccessType access, Oid classId, Oid objectId,
				  int subId, void *arg)
{
	Relation	rel;
	Oid			nsp;
	RelFileNumber relnumber = InvalidRelFileNumber;

	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/* subId != 0 es el DROP de una columna, no de la tabla. */
	if (access != OAT_DROP || classId != RelationRelationId || subId != 0)
		return;

	/*
	 * DROP EXTENSION borra los catálogos de dbengine junto con todo lo
	 * demás y el orden entre objetos no está garantizado.  Si el esquema ya
	 * no está no hay nada que limpiar, y buscarlo sería un ERROR en pleno
	 * drop.
	 */
	nsp = get_namespace_oid("dbengine", true);
	if (!OidIsValid(nsp) || !OidIsValid(get_relname_relid("stripe", nsp)))
		return;

	rel = try_relation_open(objectId, NoLock);
	if (rel == NULL)
		return;
	if (dbe_is_columnar_relation(rel))
		relnumber = rel->rd_locator.relNumber;
	relation_close(rel, NoLock);

	if (RelFileNumberIsValid(relnumber))
		dbe_catalog_drop_relation(relnumber);
}

void
dbe_catalog_drop_hook_init(void)
{
	prev_object_access_hook = object_access_hook;
	object_access_hook = dbe_object_access;
}

/*
 * Esto lo llama el planner, así que no puede recorrer los segmentos de
 * todos los stripes: con 100M de filas son 1530 stripes y 7650 filas de
 * catálogo, y el planning pasaba de 0,3 ms a 11 ms.
 *
 * Se muestrean unos pocos stripes y se extrapola.  Es un costo estimado:
 * los stripes son parejos porque los corta el mismo umbral de filas, así
 * que muestrear alcanza y de sobra.  Las filas sí se cuentan todas — eso
 * sale de la lista de stripes, que es una sola recorrida.
 */
#define DBE_COST_SAMPLE_STRIPES	32

void
dbe_catalog_column_bytes(RelFileNumber relnumber, Snapshot snapshot,
						 const bool *wanted, int natts,
						 uint64 *nbytes, uint64 *nrows)
{
	List	   *stripes = dbe_catalog_list_stripes(relnumber, snapshot);
	ListCell   *lc;
	DbeSegDesc *segs;
	bool	   *found;
	int			nstripes = list_length(stripes);
	int			sampled = 0;
	uint64		sample_rows = 0;
	uint64		sample_bytes = 0;

	*nbytes = 0;
	*nrows = 0;

	if (stripes == NIL)
		return;

	segs = (DbeSegDesc *) palloc0(sizeof(DbeSegDesc) * natts);
	found = (bool *) palloc0(sizeof(bool) * natts);

	foreach(lc, stripes)
	{
		DbeStripeDesc *s = (DbeStripeDesc *) lfirst(lc);
		int			i;

		*nrows += s->row_count;

		if (sampled >= DBE_COST_SAMPLE_STRIPES)
			continue;

		dbe_catalog_load_segments(relnumber, s->stripe_id, natts, wanted,
								  segs, found);

		for (i = 0; i < natts; i++)
			if (found[i])
				sample_bytes += segs[i].seg_length;

		sample_rows += s->row_count;
		sampled++;
	}

	/* Se extrapola por filas, no por stripes: el último suele estar a medio llenar. */
	if (sample_rows > 0)
		*nbytes = (uint64) ((double) sample_bytes * *nrows / sample_rows);

	(void) nstripes;

	pfree(segs);
	pfree(found);
	list_free_deep(stripes);
}

void
dbe_catalog_summarize(RelFileNumber relnumber, Snapshot snapshot, uint64 *nrows,
					  uint64 *nbytes, uint64 *nstripes)
{
	List	   *stripes = dbe_catalog_list_stripes(relnumber, snapshot);
	ListCell   *lc;

	*nrows = 0;
	*nbytes = 0;
	*nstripes = 0;

	foreach(lc, stripes)
	{
		DbeStripeDesc *s = (DbeStripeDesc *) lfirst(lc);

		*nrows += s->row_count;
		*nbytes += s->data_length;
		(*nstripes)++;
	}

	list_free_deep(stripes);
}
