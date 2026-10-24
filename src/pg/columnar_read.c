/*-------------------------------------------------------------------------
 *
 * columnar_read.c
 *	  Cursor de lectura sobre los stripes de una relación columnar.
 *
 * Lo comparten el seq scan del Table AM, ANALYZE y la construcción de
 * índices, así que la lógica de visibilidad y de decodificación vive en un
 * solo lugar.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"

#include "dbengine/bridge.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"
#include "dbengine/storage.h"
#include "dbengine/zonemap.h"

struct DbeRowCursor
{
	Relation	rel;
	Snapshot	snapshot;
	bool	   *wanted;			/* column pruning; NULL = todas */

	List	   *stripes;		/* DbeStripeDesc * visibles, por stripe_id */
	int			nstripes;
	int			stripe_idx;		/* índice del stripe cargado, -1 si ninguno */
	DbeStripeDesc *stripe;
	DbeStripeVisibility stripe_vis;
	DbeSegReader *cid_reader;	/* solo si stripe_vis == PARTIAL */
	uint64		row_in_stripe;

	int			natts;
	/*
	 * Lista de columnas pedidas, precalculada.  Recorrer solo estas en vez
	 * de las natts con un if adentro importa cuando la query pide 1 de 8
	 * columnas, que es el caso para el que existe el column pruning.
	 */
	int		   *wanted_atts;
	int			n_wanted_atts;

	/*
	 * Late materialization: `filter_atts` son las columnas que mira el
	 * predicado y `rest_atts` las demás pedidas.  Se llenan en dos pasos —
	 * primero las del filtro, y las otras solo si la fila sobrevive.  Sin
	 * filtro configurado, filter_atts queda vacío y rest_atts es todo.
	 */
	int		   *filter_atts;
	int			n_filter_atts;
	int		   *rest_atts;
	int			n_rest_atts;
	uint64		last_row;		/* fila que llenó dbe_cursor_next_keys() */

	DbeSegReader **readers;
	char	  **segbufs;
	bool	   *seg_found;

	DbeStripeFilterFn filter;
	void	   *filter_arg;
	bool	   *want_zone;		/* columnas con datos de zona caros que traer */
	uint64		stripes_pruned;

	/*
	 * Máscara de gránulos del stripe actual: bit prendido = puede tener
	 * filas que pasen.  NULL cuando no se pudo descartar ninguno, que es lo
	 * mismo que "todos vivos" pero sin pagar la comprobación por fila.
	 */
	uint8	   *gran_mask;
	uint32		gran_count;
	uint64		granules_pruned;

	MemoryContext cxt;			/* contexto del cursor */
	MemoryContext stripe_cxt;	/* se resetea al cambiar de stripe */
	bool		own_snapshot;

	/*
	 * Snapshot MVCC tomado antes de listar los stripes.  Con él se buscan
	 * los segmentos de un stripe que una fusión concurrente borró después
	 * de que el cursor lo listara; ver dbe_catalog_load_segments_ext().
	 * NULL si no se pudo tomar, y ahí se usa solo SnapshotSelf.
	 */
	Snapshot	catalog_snapshot;
	bool		own_catalog_snapshot;
};

static void dbe_cursor_unload_stripe(DbeRowCursor *cur);
static bool dbe_cursor_load_stripe(DbeRowCursor *cur, int index);
static bool dbe_row_visible(DbeRowCursor *cur, uint64 row);
static bool dbe_cursor_take_row(DbeRowCursor *cur, uint64 *row);

DbeRowCursor *
dbe_cursor_begin(Relation rel, Snapshot snapshot, const bool *wanted)
{
	DbeRowCursor *cur;
	MemoryContext cxt;
	MemoryContext old;
	int			natts = RelationGetDescr(rel)->natts;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"dbengine cursor",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);

	cur = (DbeRowCursor *) palloc0(sizeof(DbeRowCursor));

	/*
	 * table_beginscan_analyze() pasa snapshot NULL: ANALYZE no trae uno
	 * propio.  El storage columnar sí necesita uno para decidir qué stripes
	 * ve.
	 *
	 * Si hay que tomar uno nuevo hay que REGISTRARLO: GetLatestSnapshot()
	 * devuelve un estático del snapshot manager que la próxima llamada
	 * pisa, y el cursor se lo queda mientras dure el scan.  Guardárselo sin
	 * registrar es leer un snapshot que puede haber cambiado abajo.
	 */
	if (snapshot == NULL)
	{
		if (ActiveSnapshotSet())
			snapshot = GetActiveSnapshot();
		else
		{
			snapshot = RegisterSnapshot(GetLatestSnapshot());
			cur->own_snapshot = true;
		}
	}

	cur->rel = rel;
	cur->snapshot = snapshot;
	cur->natts = natts;
	cur->cxt = cxt;
	cur->stripe_idx = -1;
	cur->readers = (DbeSegReader **) palloc0(sizeof(DbeSegReader *) * natts);
	cur->segbufs = (char **) palloc0(sizeof(char *) * natts);
	cur->seg_found = (bool *) palloc0(sizeof(bool) * natts);

	cur->wanted_atts = (int *) palloc(sizeof(int) * natts);
	cur->filter_atts = (int *) palloc(sizeof(int) * natts);
	cur->rest_atts = (int *) palloc(sizeof(int) * natts);
	if (wanted != NULL)
	{
		int			i;

		cur->wanted = (bool *) palloc(sizeof(bool) * natts);
		memcpy(cur->wanted, wanted, sizeof(bool) * natts);

		for (i = 0; i < natts; i++)
			if (wanted[i])
				cur->wanted_atts[cur->n_wanted_atts++] = i;
	}
	else
	{
		int			i;

		for (i = 0; i < natts; i++)
			cur->wanted_atts[i] = i;
		cur->n_wanted_atts = natts;
	}

	/* Sin filtro configurado: todas las columnas pedidas son "el resto". */
	dbe_cursor_set_filter_columns(cur, NULL);

	cur->stripe_cxt = AllocSetContextCreate(cxt, "dbengine stripe",
											ALLOCSET_DEFAULT_SIZES);

	/*
	 * Las filas que todavía están en el buffer de escritura tienen que
	 * estar en el storage antes de listar los stripes.  El (xid, cid) que
	 * el flush registra es el original, así que la visibilidad sale bien
	 * aunque el flush ocurra recién ahora.
	 */
	MemoryContextSwitchTo(old);
	dbe_staging_flush_relation(RelationGetRelid(rel));
	MemoryContextSwitchTo(cxt);

	/*
	 * El snapshot de respaldo para los metadatos tiene que ser ANTERIOR a
	 * la lista de stripes: es lo que garantiza que vea las filas de
	 * stripe_column de todo stripe listado, aunque una fusión las borre
	 * después.  Si el del cursor es MVCC ya cumple — el caller lo tomó
	 * antes de llamarnos —; si no (SnapshotSelf, SnapshotDirty) se toma uno.
	 * En modo paralelo no se puede tomar uno nuevo, pero ahí el del cursor
	 * siempre es el MVCC compartido.
	 */
	if (IsMVCCSnapshot(snapshot))
		cur->catalog_snapshot = snapshot;
	else if (!IsInParallelMode())
	{
		cur->catalog_snapshot = RegisterSnapshot(GetLatestSnapshot());
		cur->own_catalog_snapshot = true;
	}

	cur->stripes = dbe_catalog_list_stripes(rel->rd_locator.relNumber, snapshot);
	cur->nstripes = list_length(cur->stripes);

	MemoryContextSwitchTo(old);

	return cur;
}

void
dbe_cursor_set_filter(DbeRowCursor *cur, DbeStripeFilterFn fn, void *arg,
					  const bool *want_zone)
{
	cur->filter = fn;
	cur->filter_arg = arg;
	cur->want_zone = NULL;

	/*
	 * El filtro de Bloom es un bytea grande.  Solo se trae para las
	 * columnas que un predicado de igualdad podría usar: pedirlo siempre
	 * le cobraría el detoast a todas las consultas.
	 */
	if (want_zone != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(cur->cxt);

		cur->want_zone = (bool *) palloc(sizeof(bool) * cur->natts);
		memcpy(cur->want_zone, want_zone, sizeof(bool) * cur->natts);
		MemoryContextSwitchTo(old);
	}
}

uint64
dbe_cursor_stripes_pruned(DbeRowCursor *cur)
{
	return cur->stripes_pruned;
}

void
dbe_cursor_rescan(DbeRowCursor *cur)
{
	dbe_cursor_unload_stripe(cur);
	cur->stripe_idx = -1;
	cur->row_in_stripe = 0;
}

const DbeStripeDesc *
dbe_cursor_stripe_at(DbeRowCursor *cur, int index)
{
	if (index < 0 || index >= cur->nstripes)
		return NULL;
	return (const DbeStripeDesc *) list_nth(cur->stripes, index);
}

int
dbe_cursor_nstripes(DbeRowCursor *cur)
{
	return cur->nstripes;
}

uint64
dbe_cursor_total_rows(DbeRowCursor *cur)
{
	uint64		total = 0;
	ListCell   *lc;

	foreach(lc, cur->stripes)
		total += ((DbeStripeDesc *) lfirst(lc))->row_count;

	return total;
}

bool
dbe_cursor_next_stripe(DbeRowCursor *cur)
{
	for (;;)
	{
		if (!dbe_cursor_load_stripe(cur, cur->stripe_idx + 1))
			return false;
		if (cur->stripe->row_count > 0 &&
			cur->row_in_stripe < cur->stripe->row_count)
			return true;
		/* Stripe vacío o descartado por el zone map: seguir. */
	}
}

uint64
dbe_cursor_stripe_rows(DbeRowCursor *cur)
{
	return cur->stripe != NULL ? cur->stripe->row_count : 0;
}

uint64
dbe_cursor_stripe_first_row(DbeRowCursor *cur)
{
	return cur->stripe != NULL ? cur->stripe->first_row_number : 0;
}

bool
dbe_cursor_stripe_fully_visible(DbeRowCursor *cur)
{
	return cur->stripe_vis == DBE_STRIPE_ALL;
}

bool
dbe_cursor_stripe_has_pending(DbeRowCursor *cur)
{
	return cur->stripe != NULL && cur->row_in_stripe < cur->stripe->row_count;
}

bool
dbe_cursor_row_is_visible(DbeRowCursor *cur, uint64 row)
{
	return dbe_row_visible(cur, row);
}

bool
dbe_cursor_column(DbeRowCursor *cur, AttrNumber attnum, DbeColumnVector *out)
{
	DbeStatus	st;
	int32		is_vector = 0;
	const void *data = NULL;
	const void *nulls = NULL;
	uint32		width = 0;

	memset(out, 0, sizeof(*out));

	if (attnum < 1 || attnum > cur->natts || cur->readers[attnum - 1] == NULL)
		return false;

	out->reader = cur->readers[attnum - 1];

	st = dbe_segreader_vector(out->reader, &is_vector, &data, &width, &nulls);
	dbe_error_if_not_ok(&st, "pidiendo el vector de una columna");

	out->contiguous = (is_vector != 0);
	out->data = (const char *) data;
	out->width = width;
	out->nulls = (const char *) nulls;
	out->attalign = TupleDescAttr(RelationGetDescr(cur->rel), attnum - 1)->attalign;
	out->cxt = cur->stripe_cxt;
	return true;
}

Datum
dbe_varlena_datum(const void *data, char attalign, MemoryContext cxt)
{
	const struct varlena *v = (const struct varlena *) data;
	Size		size;
	void	   *copy;

	if (!VARATT_IS_4B(v) ||
		(uintptr_t) data == att_align_nominal((uintptr_t) data, attalign))
		return PointerGetDatum(data);

	/*
	 * palloc devuelve memoria MAXALIGN, que alcanza para cualquier typalign.
	 * Vive lo que el stripe: los punteros a valores alineados también
	 * apuntan adentro del stripe y mueren con él.
	 */
	size = VARSIZE(v);
	copy = MemoryContextAlloc(cxt, size);
	memcpy(copy, data, size);
	return PointerGetDatum(copy);
}

bool
dbe_cursor_stripe_zonemap(DbeRowCursor *cur, int stripe_index,
						  AttrNumber attnum, DbeSegDesc *out, bool *all_null)
{
	DbeStripeDesc *stripe;
	DbeSegDesc *segs;
	bool	   *found;
	bool	   *wanted;
	bool		ok = false;

	*all_null = false;

	if (stripe_index < 0 || stripe_index >= cur->nstripes)
		return false;
	if (attnum < 1 || attnum > cur->natts)
		return false;

	stripe = (DbeStripeDesc *) list_nth(cur->stripes, stripe_index);

	/*
	 * Con visibilidad parcial el zone map abarca filas de comandos que este
	 * snapshot todavía no ve: usarlo daría un mínimo o un máximo que no
	 * existe para quien pregunta.
	 */
	if (dbe_stripe_visible(stripe, cur->snapshot) != DBE_STRIPE_ALL)
		return false;

	segs = (DbeSegDesc *) palloc0(sizeof(DbeSegDesc) * cur->natts);
	found = (bool *) palloc0(sizeof(bool) * cur->natts);
	wanted = (bool *) palloc0(sizeof(bool) * cur->natts);
	wanted[attnum - 1] = true;

	dbe_catalog_load_segments_ext(cur->rel->rd_locator.relNumber,
								  stripe->stripe_id, cur->natts, wanted, NULL,
								  cur->catalog_snapshot, segs, found);

	if (found[attnum - 1])
	{
		if (segs[attnum - 1].null_count >= stripe->row_count)
		{
			*all_null = true;
			ok = true;
		}
		else if (segs[attnum - 1].has_minmax)
		{
			*out = segs[attnum - 1];
			ok = true;
		}
	}

	pfree(found);
	pfree(wanted);
	/* segs no se libera: out apunta adentro. */
	return ok;
}

uint64
dbe_cursor_count_rows(DbeRowCursor *cur)
{
	uint64		total = 0;
	int			i;

	for (i = 0; i < cur->nstripes; i++)
	{
		DbeStripeDesc *s = (DbeStripeDesc *) list_nth(cur->stripes, i);

		if (dbe_stripe_visible(s, cur->snapshot) == DBE_STRIPE_ALL)
		{
			total += s->row_count;
			continue;
		}

		/*
		 * Visibilidad parcial: hay que mirar el command id de cada fila.
		 * Cargar el stripe no trae ninguna columna — el cursor de un
		 * count(*) no pide ninguna.
		 */
		if (!dbe_cursor_load_stripe(cur, i))
			continue;

		while (cur->row_in_stripe < cur->stripe->row_count)
		{
			if (dbe_row_visible(cur, cur->row_in_stripe))
				total++;
			cur->row_in_stripe++;
		}
	}

	dbe_cursor_rescan(cur);
	return total;
}

bool
dbe_cursor_rownum_exists(DbeRowCursor *cur, uint64 rownum)
{
	ListCell   *lc;

	foreach(lc, cur->stripes)
	{
		DbeStripeDesc *s = (DbeStripeDesc *) lfirst(lc);

		if (rownum >= s->first_row_number &&
			rownum < s->first_row_number + s->row_count)
			return true;
	}

	return false;
}

static void
dbe_cursor_unload_stripe(DbeRowCursor *cur)
{
	int			i;

	for (i = 0; i < cur->natts; i++)
	{
		if (cur->readers[i] != NULL)
		{
			dbe_segreader_close(cur->readers[i]);
			cur->readers[i] = NULL;
		}
		cur->segbufs[i] = NULL;
		cur->seg_found[i] = false;
	}

	if (cur->cid_reader != NULL)
	{
		dbe_segreader_close(cur->cid_reader);
		cur->cid_reader = NULL;
	}

	MemoryContextReset(cur->stripe_cxt);
	cur->stripe = NULL;
	cur->stripe_vis = DBE_STRIPE_NONE;
	/* La máscara vive en stripe_cxt, que se acaba de resetear. */
	cur->gran_mask = NULL;
	cur->gran_count = 0;
}

/*
 * Primera fila >= `row` que cae en un gránulo que el filtro no descartó.
 *
 * Devuelve `nrows` cuando ya no queda ninguna, que es la condición de fin
 * que los bucles de scan ya usan.  Sin máscara devuelve `row` tal cual: es
 * la ruta normal y no cuesta nada.
 */
uint64
dbe_cursor_next_live_row(DbeRowCursor *cur, uint64 row)
{
	uint64		nrows;
	uint32		g;

	if (cur->gran_mask == NULL || cur->stripe == NULL)
		return row;

	nrows = cur->stripe->row_count;
	g = (uint32) (row / DBE_GRANULE_ROWS);

	while (g < cur->gran_count &&
		   !(cur->gran_mask[g >> 3] & (1 << (g & 7))))
		g++;

	if (g >= cur->gran_count)
		return nrows;

	/* Si `row` ya estaba en un gránulo vivo no se mueve. */
	return Max(row, (uint64) g * DBE_GRANULE_ROWS);
}

uint64
dbe_cursor_granules_pruned(DbeRowCursor *cur)
{
	return cur->granules_pruned;
}

/*
 * ¿Es visible esta fila del stripe cargado?
 *
 * Solo hay algo que decidir cuando el stripe abarca comandos a un lado y
 * otro del snapshot: ahí se mira el command id guardado por fila.  Es la
 * misma regla que aplica heap con cmin, pero por stripe en vez de por
 * tupla.
 */
static bool
dbe_row_visible(DbeRowCursor *cur, uint64 row)
{
	const void *data;
	uint32		len;
	int32		isnull;
	CommandId	cid;
	DbeStatus	st;

	if (cur->stripe_vis != DBE_STRIPE_PARTIAL || cur->cid_reader == NULL)
		return true;

	st = dbe_segreader_get(cur->cid_reader, (uint32) row, &data, &len, &isnull);
	dbe_error_if_not_ok(&st, "leyendo el command id de una fila");

	if (isnull || len < sizeof(CommandId))
		return true;

	memcpy(&cid, data, sizeof(CommandId));
	return cid < cur->snapshot->curcid;
}

/*
 * Carga los segmentos del stripe `index`.  Solo trae del storage los de las
 * columnas pedidas: eso es el column pruning.
 */
static bool
dbe_cursor_load_stripe(DbeRowCursor *cur, int index)
{
	DbeStripeDesc *stripe;
	DbeSegDesc *segs;
	MemoryContext old;
	int			i;

	dbe_cursor_unload_stripe(cur);

	if (index < 0 || index >= cur->nstripes)
		return false;

	stripe = (DbeStripeDesc *) list_nth(cur->stripes, index);

	old = MemoryContextSwitchTo(cur->stripe_cxt);

	segs = (DbeSegDesc *) palloc0(sizeof(DbeSegDesc) * cur->natts);
	dbe_catalog_load_segments_ext(cur->rel->rd_locator.relNumber,
								  stripe->stripe_id, cur->natts, cur->wanted,
								  cur->want_zone, cur->catalog_snapshot,
								  segs, cur->seg_found);

	/*
	 * Zone maps: se decide ANTES de tocar el storage.  Los descriptores de
	 * segmento ya traen el min/max, así que descartar un stripe cuesta lo
	 * que ya se gastó en leer el catálogo y ahorra toda la lectura.
	 */
	if (cur->filter != NULL &&
		!cur->filter(cur->filter_arg, segs, cur->seg_found, cur->natts))
	{
		MemoryContextSwitchTo(old);
		cur->stripe = stripe;
		cur->stripe_idx = index;
		cur->stripe_vis = DBE_STRIPE_NONE;
		cur->row_in_stripe = stripe->row_count;	/* queda agotado */
		cur->stripes_pruned++;
		return true;
	}

	/*
	 * Y una vuelta más fina dentro del stripe que sí se lee: los gránulos
	 * que el filtro excluye se saltean sin decodificar ni evaluar nada.  No
	 * ahorra I/O —el segmento se lee entero— pero sí todo lo que viene
	 * después de tenerlo en memoria.
	 */
	if (cur->filter != NULL && dbe_enable_granule_pruning &&
		stripe->row_count > DBE_GRANULE_ROWS)
	{
		uint32		ngran = (uint32) ((stripe->row_count + DBE_GRANULE_ROWS - 1) /
									  DBE_GRANULE_ROWS);
		uint8	   *mask = (uint8 *) palloc0((ngran + 7) / 8);

		if (dbe_filterset_granules(cur->filter_arg, segs, cur->seg_found,
								   cur->natts, stripe->row_count, mask))
		{
			uint32		g;

			cur->gran_mask = mask;
			cur->gran_count = ngran;
			for (g = 0; g < ngran; g++)
				if (!(mask[g >> 3] & (1 << (g & 7))))
					cur->granules_pruned++;
		}
		else
			pfree(mask);
	}

	for (i = 0; i < cur->natts; i++)
	{
		DbeStatus	st;

		if (!cur->seg_found[i])
			continue;

		/*
		 * palloc devuelve memoria alineada a MAXALIGN y el segmento
		 * arranca en su byte 0, así que el área de datos queda alineada a 8
		 * como espera el reader.
		 */
		cur->segbufs[i] = (char *) palloc(segs[i].seg_length);
		dbe_storage_read(cur->rel, stripe->data_offset + segs[i].seg_offset,
						 cur->segbufs[i], segs[i].seg_length);

		st = dbe_segreader_open(cur->segbufs[i], (uint32) segs[i].seg_length,
								&cur->readers[i]);
		dbe_error_if_not_ok(&st, "abriendo un segmento de columna");
	}

	MemoryContextSwitchTo(old);

	cur->stripe = stripe;
	cur->stripe_idx = index;
	cur->row_in_stripe = 0;
	cur->stripe_vis = dbe_stripe_visible(stripe, cur->snapshot);

	/*
	 * Si el stripe cae a caballo del snapshot, hace falta el segmento de
	 * command ids para filtrar fila por fila.
	 */
	if (cur->stripe_vis == DBE_STRIPE_PARTIAL)
	{
		DbeSegDesc	cidseg;

		old = MemoryContextSwitchTo(cur->stripe_cxt);

		if (dbe_catalog_load_cid_segment(cur->rel->rd_locator.relNumber,
										 stripe->stripe_id, &cidseg))
		{
			char	   *bytes = (char *) palloc(cidseg.seg_length);
			DbeStatus	st;

			dbe_storage_read(cur->rel, stripe->data_offset + cidseg.seg_offset,
							 bytes, cidseg.seg_length);
			st = dbe_segreader_open(bytes, (uint32) cidseg.seg_length,
									&cur->cid_reader);
			dbe_error_if_not_ok(&st, "abriendo el segmento de command ids");
		}

		MemoryContextSwitchTo(old);
	}

	return true;
}

bool
dbe_cursor_set_stripe(DbeRowCursor *cur, int stripe_index)
{
	return dbe_cursor_load_stripe(cur, stripe_index);
}

/*
 * Contrato de SnapshotDirty, que tableam.h no documenta pero el executor da
 * por sentado.
 *
 * InitDirtySnapshot() SOLO fija snapshot_type: xmin, xmax y speculativeToken
 * quedan como basura del stack.  Es el AM el que tiene que escribirlos en
 * cada fila que devuelve — heap lo hace al principio de
 * HeapTupleSatisfiesDirty().  Significan "el xid que el caller tiene que
 * esperar antes de reintentar", o Invalid si la fila está estable.
 *
 * No cumplirlo cuelga a check_exclusion_or_unique_constraint() para
 * siempre: lee el xmin basura, hace XactLockTableWait() sobre un xid que no
 * existe (vuelve al toque), y hace `goto retry` sin que nada haya cambiado.
 * Lo encontró la suite de regresión de Postgres con un
 * INSERT ... ON CONFLICT DO NOTHING.
 */
static void
dbe_note_dirty_snapshot(DbeRowCursor *cur)
{
	Snapshot	snap = cur->snapshot;
	TransactionId xid;

	if (snap == NULL || snap->snapshot_type != SNAPSHOT_DIRTY)
		return;

	snap->xmin = InvalidTransactionId;
	snap->xmax = InvalidTransactionId;
	snap->speculativeToken = 0;

	/* Append-only: nunca hay un xmax que esperar, solo un inserter. */
	xid = XidFromFullTransactionId(cur->stripe->insert_xid);
	if (!TransactionIdIsCurrentTransactionId(xid) &&
		TransactionIdIsInProgress(xid))
		snap->xmin = xid;
}

/*
 * Llena el slot con la fila `row` del stripe cargado.
 */
static void
dbe_cursor_fill_atts(DbeRowCursor *cur, uint64 row, TupleTableSlot *slot,
					 const int *atts, int natts_list)
{
	/*
	 * Los valores se interpretan con el descriptor DEL SLOT, no con el de
	 * la relación.
	 *
	 * No son siempre el mismo.  ATRewriteTable() arma el slot con
	 * tab->oldDesc — el descriptor de ANTES del ALTER — mientras que
	 * RelationGetDescr() ya devuelve el nuevo, porque el catálogo se
	 * actualiza antes de reescribir los datos.  Interpretando un segmento
	 * de `text` con un descriptor que dice `integer`, fetch_att() lee 4
	 * bytes de una cadena y arma un Datum que no apunta a ningún lado.
	 *
	 * La regla general: quien llama define cómo se interpretan los valores.
	 */
	TupleDesc	desc = slot->tts_tupleDescriptor;
	int			natts = Min(desc->natts, cur->natts);
	int			k;

	for (k = 0; k < natts_list; k++)
	{
		int			i = atts[k];
		Form_pg_attribute att;
		const void *data;
		uint32		len;
		int32		isnull;
		DbeStatus	st;

		if (i >= natts)
			continue;

		att = TupleDescAttr(desc, i);

		if (cur->readers[i] == NULL)
		{
			/*
			 * No hay segmento para esta columna.  Puede ser porque no se
			 * pidió (column pruning) o porque está borrada — ahí NULL está
			 * bien — o porque el stripe es ANTERIOR a la columna.
			 *
			 * Ese último caso es ALTER TABLE ADD COLUMN ... DEFAULT, que
			 * desde PG 11 no reescribe la tabla: guarda el valor en
			 * pg_attribute.attmissingval y espera que el AM lo complete al
			 * reconstruir la fila.  heap lo hace en heap_getattr(); acá hay
			 * que hacerlo a mano.
			 */
			if (att->atthasmissing && !att->attisdropped)
			{
				bool		missing_isnull;

				slot->tts_values[i] = getmissingattr(desc, i + 1, &missing_isnull);
				slot->tts_isnull[i] = missing_isnull;
			}
			continue;
		}

		st = dbe_segreader_get(cur->readers[i], (uint32) row, &data, &len, &isnull);
		dbe_error_if_not_ok(&st, "leyendo un valor del segmento");

		if (isnull)
			continue;

		slot->tts_isnull[i] = false;

		if (att->attlen > 0)
			slot->tts_values[i] = fetch_att(data, att->attbyval, att->attlen);
		else if (att->attlen == -1)
			slot->tts_values[i] = dbe_varlena_datum(data, att->attalign,
													cur->stripe_cxt);
		else
			slot->tts_values[i] = PointerGetDatum(data);
	}

}

/*
 * Deja el slot listo para una fila nueva.
 *
 * Todo arranca en NULL: las columnas que no se pidieron quedan así, y las
 * pedidas se llenan después.  Un memset es más barato que recorrer natts
 * con un if por columna.
 */
static void
dbe_cursor_begin_row(DbeRowCursor *cur, TupleTableSlot *slot)
{
	dbe_note_dirty_snapshot(cur);
	ExecClearTuple(slot);
	memset(slot->tts_isnull, true,
		   sizeof(bool) * slot->tts_tupleDescriptor->natts);
}

static void
dbe_cursor_end_row(DbeRowCursor *cur, uint64 row, TupleTableSlot *slot)
{
	ExecStoreVirtualTuple(slot);
	/*
	 * tableoid lo lee slot_getsysattr() de acá.  El seq scan del Table AM lo
	 * ponía por su cuenta, pero el Custom Scan, el fetch por índice y el
	 * bitmap scan no: `GROUP BY tableoid` sobre una particionada colapsaba
	 * todas las particiones en una sola fila con tableoid 0.
	 */
	slot->tts_tableOid = RelationGetRelid(cur->rel);
	dbe_tid_from_rownum(cur->stripe->first_row_number + row, &slot->tts_tid);
}

/*
 * Llena el slot con la fila `row` del stripe cargado.
 */
static void
dbe_cursor_fill_slot(DbeRowCursor *cur, uint64 row, TupleTableSlot *slot)
{
	dbe_cursor_begin_row(cur, slot);
	dbe_cursor_fill_atts(cur, row, slot, cur->wanted_atts, cur->n_wanted_atts);
	dbe_cursor_end_row(cur, row, slot);
}

/*
 * Late materialization.
 *
 * dbe_cursor_next_keys() avanza a la próxima fila visible y llena SOLO las
 * columnas que mira el predicado; dbe_cursor_fill_rest() completa el resto,
 * y se llama únicamente para las filas que lo pasaron.
 *
 * La ganancia es no decodificar las columnas anchas de filas que se van a
 * tirar.  Con un predicado selectivo sobre una columna angosta y un SELECT
 * que pide texto, es la diferencia entre decodificar todo y decodificar
 * casi nada.
 *
 * El slot queda "almacenado" ya en next_keys(): es virtual, así que
 * completar más tts_values después es válido mientras nadie lo haya
 * materializado, y ExecQual() no materializa.
 */
void
dbe_cursor_set_filter_columns(DbeRowCursor *cur, const bool *cols)
{
	int			k;

	cur->n_filter_atts = 0;
	cur->n_rest_atts = 0;

	for (k = 0; k < cur->n_wanted_atts; k++)
	{
		int			i = cur->wanted_atts[k];

		if (cols != NULL && cols[i])
			cur->filter_atts[cur->n_filter_atts++] = i;
		else
			cur->rest_atts[cur->n_rest_atts++] = i;
	}
}

bool
dbe_cursor_next_keys(DbeRowCursor *cur, TupleTableSlot *slot)
{
	for (;;)
	{
		uint64		row;

		while (!dbe_cursor_take_row(cur, &row))
		{
			if (!dbe_cursor_load_stripe(cur, cur->stripe_idx + 1))
				return false;
		}

		if (!dbe_row_visible(cur, row))
			continue;

		cur->last_row = row;
		dbe_cursor_begin_row(cur, slot);
		dbe_cursor_fill_atts(cur, row, slot, cur->filter_atts,
							 cur->n_filter_atts);
		dbe_cursor_end_row(cur, row, slot);
		return true;
	}
}


/*
 * Toma la próxima fila del stripe cargado, salteando de una los gránulos
 * que el filtro descartó.  false = el stripe se agotó.
 *
 * Es el único lugar que avanza row_in_stripe, justamente para que el salto
 * valga para los cuatro recorridos y no haya que acordarse en cada uno.
 */
static bool
dbe_cursor_take_row(DbeRowCursor *cur, uint64 *row)
{
	uint64		r;

	if (cur->stripe == NULL)
		return false;

	r = dbe_cursor_next_live_row(cur, cur->row_in_stripe);
	if (r >= cur->stripe->row_count)
	{
		cur->row_in_stripe = cur->stripe->row_count;
		return false;
	}

	*row = r;
	cur->row_in_stripe = r + 1;
	return true;
}

/* Igual que next_keys() pero sin cambiar de stripe: lo usa el scan paralelo. */
bool
dbe_cursor_next_keys_in_stripe(DbeRowCursor *cur, TupleTableSlot *slot)
{
	for (;;)
	{
		uint64		row;

		if (!dbe_cursor_take_row(cur, &row))
			return false;

		if (!dbe_row_visible(cur, row))
			continue;

		cur->last_row = row;
		dbe_cursor_begin_row(cur, slot);
		dbe_cursor_fill_atts(cur, row, slot, cur->filter_atts,
							 cur->n_filter_atts);
		dbe_cursor_end_row(cur, row, slot);
		return true;
	}
}

void
dbe_cursor_fill_rest(DbeRowCursor *cur, TupleTableSlot *slot)
{
	dbe_cursor_fill_atts(cur, cur->last_row, slot, cur->rest_atts,
						 cur->n_rest_atts);
}

bool
dbe_cursor_next(DbeRowCursor *cur, TupleTableSlot *slot)
{
	for (;;)
	{
		uint64		row;

		while (!dbe_cursor_take_row(cur, &row))
		{
			if (!dbe_cursor_load_stripe(cur, cur->stripe_idx + 1))
				return false;
		}

		if (!dbe_row_visible(cur, row))
			continue;

		dbe_cursor_fill_slot(cur, row, slot);
		return true;
	}
}

/*
 * Trae una fila por su número lógico.  Lo usan las lecturas por TID
 * (index scan, fetch de versión de fila).
 */
bool
dbe_cursor_fetch_rownum(DbeRowCursor *cur, uint64 rownum, TupleTableSlot *slot)
{
	int			i;

	/* Si ya está cargado el stripe correcto, no se recarga. */
	if (cur->stripe != NULL &&
		rownum >= cur->stripe->first_row_number &&
		rownum < cur->stripe->first_row_number + cur->stripe->row_count)
	{
		uint64		row = rownum - cur->stripe->first_row_number;

		if (!dbe_row_visible(cur, row))
			return false;
		dbe_cursor_fill_slot(cur, row, slot);
		return true;
	}

	for (i = 0; i < cur->nstripes; i++)
	{
		DbeStripeDesc *s = (DbeStripeDesc *) list_nth(cur->stripes, i);

		if (rownum >= s->first_row_number &&
			rownum < s->first_row_number + s->row_count)
		{
			if (!dbe_cursor_load_stripe(cur, i))
				return false;
			if (!dbe_row_visible(cur, rownum - s->first_row_number))
				return false;
			dbe_cursor_fill_slot(cur, rownum - s->first_row_number, slot);
			return true;
		}
	}

	return false;				/* no existe o no es visible */
}

void
dbe_cursor_end(DbeRowCursor *cur)
{
	bool		own = cur->own_snapshot;
	Snapshot	snap = cur->snapshot;
	bool		own_catalog = cur->own_catalog_snapshot;
	Snapshot	catalog_snap = cur->catalog_snapshot;

	dbe_cursor_unload_stripe(cur);
	MemoryContextDelete(cur->cxt);

	if (own)
		UnregisterSnapshot(snap);
	if (own_catalog)
		UnregisterSnapshot(catalog_snap);
}

/*
 * Igual que dbe_cursor_next() pero sin avanzar de stripe: devuelve false al
 * agotar el stripe cargado.  Lo usa el scan paralelo, que decide él mismo
 * qué stripe le toca a cada worker.
 */
bool
dbe_cursor_next_in_stripe(DbeRowCursor *cur, TupleTableSlot *slot)
{
	for (;;)
	{
		uint64		row;

		if (!dbe_cursor_take_row(cur, &row))
			return false;

		if (!dbe_row_visible(cur, row))
			continue;

		dbe_cursor_fill_slot(cur, row, slot);
		return true;
	}
}
