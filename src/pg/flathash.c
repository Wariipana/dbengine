/*-------------------------------------------------------------------------
 *
 * flathash.c
 *	  GROUP BY por hash con tabla propia y volcado a disco.  Ver flathash.h.
 *
 * Una entrada son `words` uint64 seguidos en un arreglo denso:
 *
 *	   [hash << 32 | máscara de nulos] [clave 1] ... [clave k] [estados]
 *
 * con un directorio de ranuras uint32 (0 = vacía, si no índice + 1) de
 * direccionamiento abierto encima, al doble de las entradas.  Las claves
 * son el Datum por valor, con 0 si es nulo: dos claves son iguales si son
 * iguales sus palabras y sus máscaras, que para los tipos admitidos es la
 * misma igualdad que la del operador.
 *
 * Volcado: una vez que la tabla no puede crecer más, una fila de un grupo
 * que no está va a una de `nparts` particiones, según un hash del hash
 * mezclado con el nivel.  Una partición es un archivo temporal de registros
 * empaquetados —máscaras, claves y argumentos con su ancho real— y se
 * procesa entera después de emitir la tabla, con el nivel siguiente: los
 * grupos de una partición no pueden estar en ninguna otra, así que cada
 * uno se emite una sola vez.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"

#include "dbengine/flathash.h"

/* Más profundo que esto es un hash que no reparte, no un dato grande. */
#define DBE_FLAT_MAX_LEVEL	16

typedef struct DbeFlatPart
{
	BufFile    *file;
	int			level;			/* con qué nivel se procesa */
	uint64		rows;
} DbeFlatPart;

typedef enum DbeFlatPhase
{
	DBE_FLAT_FILL,
	DBE_FLAT_EMIT,
	DBE_FLAT_DONE
} DbeFlatPhase;

struct DbeFlatAgg
{
	int			nkeys;
	int16		keylen[DBE_FLAT_MAX_KEYS];
	int			naggs;
	DbeFlatAggDef *defs;
	int		   *stateoff;		/* palabra donde empieza cada estado */
	int			words;			/* palabras por entrada */

	Size		budget;
	double		est_groups;
	uint32		max_cap;
	int			max_parts;

	MemoryContext cxt;			/* tabla, directorio, particiones */
	MemoryContext outcxt;		/* valores por referencia de la salida */

	uint64	   *entries;
	uint32		cap;
	uint32		nentries;
	uint32	   *dir;
	uint32		dirmask;
	bool		full;

	/* Volcado del nivel que se está procesando. */
	int			level;
	int			nparts;
	DbeFlatPart *parts;
	List	   *pending;		/* DbeFlatPart * por procesar */

	int			reclen;
	char	   *rec;

	DbeFlatPhase phase;
	uint32		emit_pos;

	ArrayType  *avgarr;			/* {count, sum} para int8_avg() */

	uint64		groups_out;
	int			parts_total;
	uint64		spilled_rows;
	Size		peak;
};

static int
dbe_flat_state_words(DbeFlatKind kind)
{
	return (kind == DBE_FLAT_COUNT_STAR || kind == DBE_FLAT_COUNT) ? 1 : 2;
}

static inline int64
dbe_flat_int(Datum d, int16 len)
{
	switch (len)
	{
		case 2:
			return DatumGetInt16(d);
		case 4:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static inline Datum
dbe_flat_datum(int64 v, int16 len)
{
	switch (len)
	{
		case 2:
			return Int16GetDatum((int16) v);
		case 4:
			return Int32GetDatum((int32) v);
		default:
			return Int64GetDatum(v);
	}
}

/*
 * Bytes por entrada contando el directorio, que tiene entre 2 y 4 ranuras
 * por entrada: es una potencia de dos de al menos el doble.
 */
static Size
dbe_flat_entry_bytes(DbeFlatAgg *fa)
{
	return sizeof(uint64) * fa->words + 4 * sizeof(uint32);
}

static void
dbe_flat_alloc(DbeFlatAgg *fa, uint32 cap)
{
	uint64	   *old = fa->entries;
	uint32		n = fa->nentries;

	fa->entries = (uint64 *) MemoryContextAllocHuge(fa->cxt,
													sizeof(uint64) * fa->words * (Size) cap);
	if (old != NULL)
	{
		memcpy(fa->entries, old, sizeof(uint64) * fa->words * (Size) n);
		pfree(old);
	}
	if (fa->dir != NULL)
		pfree(fa->dir);
	{
		uint32		nslots = pg_nextpower2_32(2 * cap);

		fa->dir = (uint32 *) MemoryContextAllocHuge(fa->cxt,
													sizeof(uint32) * (Size) nslots);
		memset(fa->dir, 0, sizeof(uint32) * (Size) nslots);
		fa->dirmask = nslots - 1;
	}
	fa->cap = cap;

	/* Directorio de nuevo: las entradas no se mueven de índice. */
	for (uint32 i = 0; i < n; i++)
	{
		uint32		h = (uint32) (fa->entries[(Size) i * fa->words] >> 32);
		uint32		pos = h & fa->dirmask;

		while (fa->dir[pos] != 0)
			pos = (pos + 1) & fa->dirmask;
		fa->dir[pos] = i + 1;
	}

	fa->peak = Max(fa->peak, MemoryContextMemAllocated(fa->cxt, true));
}

DbeFlatAgg *
dbe_flat_create(int nkeys, const int16 *keylen, int naggs,
				const DbeFlatAggDef *defs, Size budget, double est_groups)
{
	DbeFlatAgg *fa;
	int			w;
	uint32		cap;
	Size		table_budget;

	Assert(nkeys >= 1 && nkeys <= DBE_FLAT_MAX_KEYS);

	fa = (DbeFlatAgg *) palloc0(sizeof(DbeFlatAgg));
	fa->nkeys = nkeys;
	memcpy(fa->keylen, keylen, sizeof(int16) * nkeys);
	fa->naggs = naggs;
	fa->defs = (DbeFlatAggDef *) palloc(sizeof(DbeFlatAggDef) * Max(naggs, 1));
	memcpy(fa->defs, defs, sizeof(DbeFlatAggDef) * naggs);
	fa->stateoff = (int *) palloc(sizeof(int) * Max(naggs, 1));

	w = 1 + nkeys;
	for (int i = 0; i < naggs; i++)
	{
		fa->stateoff[i] = w;
		w += dbe_flat_state_words(defs[i].kind);
	}
	fa->words = w;

	fa->reclen = 2 * sizeof(uint32);
	for (int i = 0; i < nkeys; i++)
		fa->reclen += keylen[i];
	for (int i = 0; i < naggs; i++)
		fa->reclen += defs[i].vallen;

	/*
	 * Un octavo del presupuesto para los buffers de las particiones (un
	 * bloque cada una), el resto para la tabla.
	 */
	fa->budget = budget;
	fa->est_groups = est_groups;
	fa->max_parts = (int) Min(256, Max(4, budget / 8 / BLCKSZ));
	fa->max_parts = (int) pg_prevpower2_32((uint32) fa->max_parts);
	table_budget = budget - (Size) fa->max_parts * BLCKSZ;
	/*
	 * Las entradas no tienen que ser potencia de dos, solo el directorio.
	 * Redondear para abajo dejaba sin usar hasta la mitad del presupuesto,
	 * y con eso cada partición de q33 quedaba apenas más grande que la
	 * tabla y volcaba un nivel más entero.
	 */
	fa->max_cap = (uint32) Min((Size) PG_INT32_MAX / 8,
							   Max((Size) 64, table_budget / dbe_flat_entry_bytes(fa)));

	fa->cxt = AllocSetContextCreate(CurrentMemoryContext, "dbengine flat hash",
									ALLOCSET_DEFAULT_SIZES);
	fa->outcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "dbengine flat hash salida",
									   ALLOCSET_SMALL_SIZES);
	fa->rec = MemoryContextAlloc(fa->cxt, fa->reclen);

	{
		Datum		zero[2] = {Int64GetDatum(0), Int64GetDatum(0)};
		MemoryContext old = MemoryContextSwitchTo(fa->cxt);

		fa->avgarr = construct_array_builtin(zero, 2, INT8OID);
		MemoryContextSwitchTo(old);
	}

	/* Arranca chica si se esperan pocos grupos; crece de a doble. */
	cap = (uint32) Min((double) fa->max_cap, Max(256.0, est_groups));
	dbe_flat_alloc(fa, cap);
	fa->phase = DBE_FLAT_FILL;
	return fa;
}

static inline uint64
dbe_flat_hash(const uint64 *keyw, int nkeys, uint32 nullmask)
{
	uint64		h = 0;

	for (int i = 0; i < nkeys; i++)
		h = hash_combine64(h, murmurhash64(keyw[i]));
	return hash_combine64(h, nullmask);
}

static void
dbe_flat_update(DbeFlatAgg *fa, uint64 *e, const Datum *vals,
				const bool *valnulls)
{
	for (int i = 0; i < fa->naggs; i++)
	{
		uint64	   *s = e + fa->stateoff[i];
		const DbeFlatAggDef *d = &fa->defs[i];
		int64		v;

		switch (d->kind)
		{
			case DBE_FLAT_COUNT_STAR:
				s[0]++;
				break;
			case DBE_FLAT_COUNT:
				if (!valnulls[i])
					s[0]++;
				break;
			case DBE_FLAT_SUM:
			case DBE_FLAT_AVG:
				if (valnulls[i])
					break;
				/* int2/int4 en int64: como int4_sum, sin chequeo de overflow. */
				s[0] = (uint64) ((int64) s[0] + dbe_flat_int(vals[i], d->vallen));
				s[1]++;
				break;
			case DBE_FLAT_MIN:
			case DBE_FLAT_MAX:
				if (valnulls[i])
					break;
				v = dbe_flat_int(vals[i], d->vallen);
				if (s[1] == 0 ||
					(d->kind == DBE_FLAT_MIN ? v < (int64) s[0] : v > (int64) s[0]))
					s[0] = (uint64) v;
				s[1] = 1;
				break;
		}
	}
}

/*
 * Abre las particiones del nivel actual: siempre todas las que entran en el
 * presupuesto.  Elegirlas según la estimación de grupos salía caro justo
 * cuando la estimación es mala, que es cuando se vuelca: en q16 el planner
 * dice 39.309 grupos y hay 1,53M, se abrían 4 particiones, ninguna entraba
 * y se volcaban de nuevo enteras —14M filas escritas para 10M de entrada—.
 * Una partición de más cuesta un buffer de un bloque, que ya está
 * descontado del presupuesto de la tabla.
 */
static void
dbe_flat_open_parts(DbeFlatAgg *fa)
{
	int			n = fa->max_parts;

	fa->nparts = n;
	fa->parts = (DbeFlatPart *) MemoryContextAllocZero(fa->cxt,
													   sizeof(DbeFlatPart) * n);
	for (int i = 0; i < n; i++)
	{
		fa->parts[i].file = BufFileCreateTemp(false);
		fa->parts[i].level = fa->level + 1;
	}
	fa->parts_total += n;
}

static void
dbe_flat_spill(DbeFlatAgg *fa, uint64 h, uint32 nullmask, const uint64 *keyw,
			   const Datum *vals, const bool *valnulls)
{
	uint32		valnullmask = 0;
	char	   *p = fa->rec;
	int			part;

	if (fa->parts == NULL)
		dbe_flat_open_parts(fa);

	/* Otros bits que los que repartieron el nivel anterior. */
	part = (int) (murmurhash64(h ^ (UINT64CONST(0x9E3779B97F4A7C15) *
									(uint64) (fa->level + 1))) &
				  (uint64) (fa->nparts - 1));

	/* count(col) no guarda el valor pero sí si es nulo. */
	for (int i = 0; i < fa->naggs; i++)
		if (fa->defs[i].kind != DBE_FLAT_COUNT_STAR && valnulls[i])
			valnullmask |= (uint32) 1 << i;

	memcpy(p, &nullmask, sizeof(uint32));
	p += sizeof(uint32);
	memcpy(p, &valnullmask, sizeof(uint32));
	p += sizeof(uint32);
	for (int i = 0; i < fa->nkeys; i++)
	{
		union
		{
			int64		align;
			char		bytes[8];
		}			tmp;

		store_att_byval(tmp.bytes, (Datum) keyw[i], fa->keylen[i]);
		memcpy(p, tmp.bytes, fa->keylen[i]);
		p += fa->keylen[i];
	}
	for (int i = 0; i < fa->naggs; i++)
	{
		union
		{
			int64		align;
			char		bytes[8];
		}			tmp;

		if (fa->defs[i].vallen == 0)
			continue;
		store_att_byval(tmp.bytes, valnulls[i] ? (Datum) 0 : vals[i],
						fa->defs[i].vallen);
		memcpy(p, tmp.bytes, fa->defs[i].vallen);
		p += fa->defs[i].vallen;
	}

	BufFileWrite(fa->parts[part].file, fa->rec, fa->reclen);
	fa->parts[part].rows++;
	fa->spilled_rows++;
}

static void
dbe_flat_add_words(DbeFlatAgg *fa, const uint64 *keyw, uint32 nullmask,
				   const Datum *vals, const bool *valnulls)
{
	uint64		h = dbe_flat_hash(keyw, fa->nkeys, nullmask);
	uint64		head = (h << 32) | nullmask;
	uint32		pos;
	uint64	   *e;

	for (pos = (uint32) h & fa->dirmask;; pos = (pos + 1) & fa->dirmask)
	{
		uint32		k = fa->dir[pos];

		if (k == 0)
			break;
		e = fa->entries + (Size) (k - 1) * fa->words;
		if (e[0] == head &&
			memcmp(e + 1, keyw, sizeof(uint64) * fa->nkeys) == 0)
		{
			dbe_flat_update(fa, e, vals, valnulls);
			return;
		}
	}

	if (fa->nentries >= fa->cap)
	{
		if (fa->cap < fa->max_cap)
		{
			dbe_flat_alloc(fa, (uint32) Min((uint64) fa->max_cap,
											(uint64) fa->cap * 2));
			/* El directorio cambió de tamaño: buscar la ranura de nuevo. */
			for (pos = (uint32) h & fa->dirmask; fa->dir[pos] != 0;
				 pos = (pos + 1) & fa->dirmask)
				;
		}
		else
		{
			fa->full = true;
			dbe_flat_spill(fa, h, nullmask, keyw, vals, valnulls);
			return;
		}
	}

	e = fa->entries + (Size) fa->nentries * fa->words;
	e[0] = head;
	memcpy(e + 1, keyw, sizeof(uint64) * fa->nkeys);
	memset(e + 1 + fa->nkeys, 0,
		   sizeof(uint64) * (fa->words - 1 - fa->nkeys));
	fa->nentries++;
	fa->dir[pos] = fa->nentries;
	dbe_flat_update(fa, e, vals, valnulls);
}

void
dbe_flat_add(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls,
			 const Datum *vals, const bool *valnulls)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	uint32		nullmask = 0;

	Assert(fa->phase == DBE_FLAT_FILL);
	for (int i = 0; i < fa->nkeys; i++)
	{
		if (keynulls[i])
		{
			nullmask |= (uint32) 1 << i;
			keyw[i] = 0;
		}
		else
			keyw[i] = (uint64) keys[i];
	}
	dbe_flat_add_words(fa, keyw, nullmask, vals, valnulls);
}

/* Las particiones del nivel que terminó pasan a la pila de pendientes. */
static void
dbe_flat_close_level(DbeFlatAgg *fa)
{
	MemoryContext old = MemoryContextSwitchTo(fa->cxt);

	for (int i = 0; i < fa->nparts; i++)
	{
		DbeFlatPart *p;

		if (fa->parts[i].rows == 0)
		{
			BufFileClose(fa->parts[i].file);
			continue;
		}
		if (BufFileSeek(fa->parts[i].file, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("dbengine: no se pudo rebobinar una partición de la agregación")));
		p = (DbeFlatPart *) palloc(sizeof(DbeFlatPart));
		*p = fa->parts[i];
		fa->pending = lappend(fa->pending, p);
	}
	MemoryContextSwitchTo(old);
	if (fa->parts != NULL)
		pfree(fa->parts);
	fa->parts = NULL;
	fa->nparts = 0;
}

void
dbe_flat_input_done(DbeFlatAgg *fa)
{
	dbe_flat_close_level(fa);
	fa->phase = DBE_FLAT_EMIT;
	fa->emit_pos = 0;
}

static void
dbe_flat_clear_table(DbeFlatAgg *fa)
{
	fa->nentries = 0;
	fa->full = false;
	memset(fa->dir, 0, sizeof(uint32) * (fa->dirmask + 1));
}

/* Procesa una partición pendiente entera: queda lista para emitir. */
static void
dbe_flat_load_part(DbeFlatAgg *fa, DbeFlatPart *part)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	Datum	   *vals = (Datum *) palloc(sizeof(Datum) * Max(fa->naggs, 1));
	bool	   *valnulls = (bool *) palloc(sizeof(bool) * Max(fa->naggs, 1));

	if (part->level > DBE_FLAT_MAX_LEVEL)
		elog(ERROR, "dbengine: la agregación volcó más de %d niveles",
			 DBE_FLAT_MAX_LEVEL);

	dbe_flat_clear_table(fa);
	fa->level = part->level;
	fa->phase = DBE_FLAT_FILL;

	for (uint64 r = 0; r < part->rows; r++)
	{
		const char *p = fa->rec;
		uint32		nullmask;
		uint32		valnullmask;

		CHECK_FOR_INTERRUPTS();
		BufFileReadExact(part->file, fa->rec, fa->reclen);
		memcpy(&nullmask, p, sizeof(uint32));
		p += sizeof(uint32);
		memcpy(&valnullmask, p, sizeof(uint32));
		p += sizeof(uint32);
		for (int i = 0; i < fa->nkeys; i++)
		{
			union
			{
				int64		align;
				char		bytes[8];
			}			tmp;

			memcpy(tmp.bytes, p, fa->keylen[i]);
			keyw[i] = (uint64) fetch_att(tmp.bytes, true, fa->keylen[i]);
			p += fa->keylen[i];
		}
		for (int i = 0; i < fa->naggs; i++)
		{
			union
			{
				int64		align;
				char		bytes[8];
			}			tmp;

			valnulls[i] = (valnullmask & ((uint32) 1 << i)) != 0;
			vals[i] = (Datum) 0;
			if (fa->defs[i].vallen == 0)
				continue;
			memcpy(tmp.bytes, p, fa->defs[i].vallen);
			vals[i] = fetch_att(tmp.bytes, true, fa->defs[i].vallen);
			p += fa->defs[i].vallen;
		}
		dbe_flat_add_words(fa, keyw, nullmask, vals, valnulls);
	}
	BufFileClose(part->file);
	pfree(part);
	pfree(vals);
	pfree(valnulls);
	dbe_flat_input_done(fa);
}

bool
dbe_flat_next(DbeFlatAgg *fa, Datum *keys, bool *keynulls, Datum *outs,
			  bool *outnulls)
{
	const uint64 *e;
	uint32		nullmask;

	MemoryContextReset(fa->outcxt);
	for (;;)
	{
		if (fa->phase == DBE_FLAT_DONE)
			return false;
		Assert(fa->phase == DBE_FLAT_EMIT);
		if (fa->emit_pos < fa->nentries)
			break;
		if (fa->pending == NIL)
		{
			fa->phase = DBE_FLAT_DONE;
			return false;
		}
		{
			DbeFlatPart *part = (DbeFlatPart *) llast(fa->pending);

			fa->pending = list_delete_last(fa->pending);
			dbe_flat_load_part(fa, part);
		}
	}

	e = fa->entries + (Size) fa->emit_pos++ * fa->words;
	nullmask = (uint32) e[0];
	for (int i = 0; i < fa->nkeys; i++)
	{
		keynulls[i] = (nullmask & ((uint32) 1 << i)) != 0;
		keys[i] = keynulls[i] ? (Datum) 0 : (Datum) e[1 + i];
	}

	for (int i = 0; i < fa->naggs; i++)
	{
		const uint64 *s = e + fa->stateoff[i];
		const DbeFlatAggDef *d = &fa->defs[i];

		outnulls[i] = false;
		switch (d->kind)
		{
			case DBE_FLAT_COUNT_STAR:
			case DBE_FLAT_COUNT:
				outs[i] = Int64GetDatum((int64) s[0]);
				break;
			case DBE_FLAT_SUM:
				outnulls[i] = (s[1] == 0);
				outs[i] = outnulls[i] ? (Datum) 0 : Int64GetDatum((int64) s[0]);
				break;
			case DBE_FLAT_AVG:
				if (s[1] == 0)
				{
					outnulls[i] = true;
					outs[i] = (Datum) 0;
				}
				else
				{
					/* El mismo cálculo que avg(int4): int8_avg sobre {n, suma}. */
					int64	   *st = (int64 *) ARR_DATA_PTR(fa->avgarr);
					MemoryContext old = MemoryContextSwitchTo(fa->outcxt);

					st[0] = (int64) s[1];
					st[1] = (int64) s[0];
					outs[i] = DirectFunctionCall1(int8_avg,
												  PointerGetDatum(fa->avgarr));
					MemoryContextSwitchTo(old);
				}
				break;
			case DBE_FLAT_MIN:
			case DBE_FLAT_MAX:
				outnulls[i] = (s[1] == 0);
				outs[i] = outnulls[i] ? (Datum) 0
					: dbe_flat_datum((int64) s[0], d->vallen);
				break;
		}
	}
	fa->groups_out++;
	return true;
}

void
dbe_flat_reset(DbeFlatAgg *fa)
{
	ListCell   *lc;

	for (int i = 0; i < fa->nparts; i++)
		BufFileClose(fa->parts[i].file);
	if (fa->parts != NULL)
		pfree(fa->parts);
	fa->parts = NULL;
	fa->nparts = 0;
	foreach(lc, fa->pending)
	{
		DbeFlatPart *p = (DbeFlatPart *) lfirst(lc);

		BufFileClose(p->file);
		pfree(p);
	}
	list_free(fa->pending);
	fa->pending = NIL;

	dbe_flat_clear_table(fa);
	fa->level = 0;
	fa->phase = DBE_FLAT_FILL;
	fa->emit_pos = 0;
	fa->groups_out = 0;
	fa->parts_total = 0;
	fa->spilled_rows = 0;
	MemoryContextReset(fa->outcxt);
}

void
dbe_flat_stats(DbeFlatAgg *fa, uint64 *groups, int *partitions,
			   uint64 *spilled_rows, Size *peak_bytes)
{
	*groups = fa->groups_out;
	*partitions = fa->parts_total;
	*spilled_rows = fa->spilled_rows;
	*peak_bytes = fa->peak;
}
