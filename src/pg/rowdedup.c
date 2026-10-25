/*-------------------------------------------------------------------------
 *
 * rowdedup.c
 *	  Filas repetidas descartadas dentro de un stripe.  Ver rowdedup.h.
 *
 * Una tabla de hash con direccionamiento abierto sobre claves de ancho
 * fijo: un uint64 por columna —el Datum o el código del diccionario— más
 * una máscara de nulos.  Se dimensiona para las filas del stripe, con el
 * doble de ranuras, así que nunca se llena y nunca crece a mitad de uno.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "dbengine/dbengine.h"
#include "dbengine/rowdedup.h"

typedef struct DbeDedupCol
{
	AttrNumber	attnum;
	bool		byval;
	DbeSegReader *reader;		/* con diccionario en este stripe, o NULL */
} DbeDedupCol;

struct DbeRowDedup
{
	int			ncols;
	DbeDedupCol cols[DBE_DEDUP_MAX_COLS];

	bool		have_stripe;	/* `generation` es la del stripe actual */
	uint64		generation;
	bool		active;			/* false: este stripe pasa entero */

	uint32		mask;			/* ranuras - 1 */
	uint32	   *slots;			/* 0 = vacía, si no índice de la clave + 1 */
	uint32		capacity;		/* claves que entran en `keys` */
	uint32		nkeys;
	uint64	   *keys;			/* nkeys * ncols */
	uint32	   *nulls;			/* máscara de nulos por clave */
	uint32	   *hashes;

	MemoryContext cxt;
};

DbeRowDedup *
dbe_dedup_create(TupleDesc desc, List *attnums)
{
	DbeRowDedup *dd;
	ListCell   *lc;

	Assert(list_length(attnums) >= 1 &&
		   list_length(attnums) <= DBE_DEDUP_MAX_COLS);

	dd = (DbeRowDedup *) palloc0(sizeof(DbeRowDedup));
	foreach(lc, attnums)
	{
		AttrNumber	a = lfirst_int(lc);

		dd->cols[dd->ncols].attnum = a;
		dd->cols[dd->ncols].byval = TupleDescAttr(desc, a - 1)->attbyval;
		dd->ncols++;
	}
	dd->cxt = AllocSetContextCreate(CurrentMemoryContext, "dbengine dedup",
									ALLOCSET_DEFAULT_SIZES);
	return dd;
}

/*
 * Stripe nuevo: decidir si se deduplica y vaciar la tabla.  Las columnas por
 * referencia entran solo con diccionario, porque el código identifica el
 * valor dentro del stripe sin tener que copiarlo ni compararlo.
 */
static void
dbe_dedup_start_stripe(DbeRowDedup *dd, DbeRowCursor *cur, uint64 generation)
{
	uint64		nrows = dbe_cursor_stripe_rows(cur);
	uint32		nslots;
	int			i;

	dd->have_stripe = true;
	dd->generation = generation;
	dd->nkeys = 0;
	dd->active = (nrows > 0 && nrows <= PG_UINT32_MAX / 4);

	for (i = 0; i < dd->ncols && dd->active; i++)
	{
		DbeDedupCol *c = &dd->cols[i];
		uint64		g;

		c->reader = NULL;
		if (c->byval)
			continue;
		c->reader = dbe_cursor_reader(cur, c->attnum, &g);
		if (c->reader == NULL)
			dd->active = false;
		else
		{
			int32		is_dict = 0;
			uint32		n = 0;
			DbeStatus	st = dbe_segreader_dict(c->reader, &is_dict, &n);

			dbe_error_if_not_ok(&st, "consultando el diccionario del segmento");
			if (!is_dict)
				dd->active = false;
		}
	}
	if (!dd->active)
		return;

	if (nrows > dd->capacity)
	{
		MemoryContextReset(dd->cxt);
		dd->capacity = (uint32) nrows;
		nslots = pg_nextpower2_32(dd->capacity * 2);
		dd->mask = nslots - 1;
		dd->slots = MemoryContextAlloc(dd->cxt, sizeof(uint32) * nslots);
		dd->keys = MemoryContextAlloc(dd->cxt,
									  sizeof(uint64) * dd->ncols * dd->capacity);
		dd->nulls = MemoryContextAlloc(dd->cxt, sizeof(uint32) * dd->capacity);
		dd->hashes = MemoryContextAlloc(dd->cxt, sizeof(uint32) * dd->capacity);
	}
	memset(dd->slots, 0, sizeof(uint32) * (dd->mask + 1));
}

bool
dbe_dedup_repeated(DbeRowDedup *dd, DbeRowCursor *cur, TupleTableSlot *slot)
{
	uint64		key[DBE_DEDUP_MAX_COLS];
	uint32		nullmask = 0;
	uint64		h = 0;
	uint64		generation;
	uint32		pos;
	int			i;

	(void) dbe_cursor_reader(cur, dd->cols[0].attnum, &generation);
	if (!dd->have_stripe || generation != dd->generation)
		dbe_dedup_start_stripe(dd, cur, generation);
	if (!dd->active)
		return false;

	for (i = 0; i < dd->ncols; i++)
	{
		DbeDedupCol *c = &dd->cols[i];
		bool		isnull;

		if (c->reader != NULL)
		{
			uint32		code = 0;
			int32		code_null = 0;
			DbeStatus	st = dbe_segreader_code(c->reader,
												(uint32) dbe_cursor_current_row(cur),
												&code, &code_null);

			dbe_error_if_not_ok(&st, "leyendo un código de diccionario");
			isnull = (code_null != 0);
			key[i] = isnull ? 0 : code;
		}
		else
		{
			Datum		d = slot_getattr(slot, c->attnum, &isnull);

			key[i] = isnull ? 0 : (uint64) d;
		}
		if (isnull)
			nullmask |= (uint32) 1 << i;
		h = hash_combine64(h, murmurhash64(key[i]));
	}
	h = hash_combine64(h, nullmask);

	for (pos = (uint32) h & dd->mask;; pos = (pos + 1) & dd->mask)
	{
		uint32		k = dd->slots[pos];
		const uint64 *other;

		if (k == 0)
			break;
		k--;
		if (dd->hashes[k] != (uint32) h || dd->nulls[k] != nullmask)
			continue;
		other = &dd->keys[(Size) k * dd->ncols];
		if (memcmp(other, key, sizeof(uint64) * dd->ncols) == 0)
			return true;
	}

	/*
	 * Una fila por clave como mucho, y las filas del stripe son capacity: no
	 * se puede llenar.  Igual se comprueba, porque si el cursor alguna vez
	 * entregara más filas de las que dice, la tabla se quedaría sin ranura
	 * libre y el bucle de arriba no terminaría.
	 */
	if (dd->nkeys >= dd->capacity)
		return false;
	memcpy(&dd->keys[(Size) dd->nkeys * dd->ncols], key,
		   sizeof(uint64) * dd->ncols);
	dd->nulls[dd->nkeys] = nullmask;
	dd->hashes[dd->nkeys] = (uint32) h;
	dd->nkeys++;
	dd->slots[pos] = dd->nkeys;
	return false;
}

void
dbe_dedup_reset(DbeRowDedup *dd)
{
	dd->have_stripe = false;
}
