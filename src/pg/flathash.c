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
 * direccionamiento abierto encima, de al menos el doble de las entradas.
 * Una clave de ancho fijo es su Datum, con 0 si es nula: dos son iguales si
 * son iguales sus palabras, que para los tipos admitidos es la igualdad del
 * operador.  Una de texto es un puntero a una copia en el arena de la
 * tabla, y se compara por largo y bytes: con collation determinista texteq
 * es exactamente eso.
 *
 * La memoria de la tabla —entradas, directorio y arena— no pasa del
 * presupuesto menos los buffers de las particiones.  Una vez que no puede
 * crecer más, una fila de un grupo que no está va a una de `nparts`
 * particiones, según un hash del hash mezclado con el nivel.  Una partición
 * es un archivo temporal de registros —máscaras, claves y argumentos con su
 * ancho real, los textos con su largo adelante— y se procesa entera después
 * de emitir la tabla, con el nivel siguiente: los grupos de una partición no
 * pueden estar en ninguna otra, así que cada uno se emite una sola vez.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "dbengine/flathash.h"

/* Más profundo que esto es un hash que no reparte, no un dato grande. */
#define DBE_FLAT_MAX_LEVEL	16

/*
 * Marca en la máscara de nulos de un registro: en vez de los argumentos de
 * una fila trae los estados ya agregados de varias (dbe_flat_spill_states).
 * Las claves son a lo sumo 8, así que el bit 31 está libre.
 */
#define DBE_FLAT_REC_STATES	((uint32) 1 << 31)

/* Bloques del arena de textos. */
#define DBE_FLAT_ARENA_BLOCK	(32 * 1024)

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
	int16		keylen[DBE_FLAT_MAX_KEYS];	/* -1: texto */
	bool		anyvar;			/* alguna clave es texto */
	int			naggs;
	DbeFlatAggDef *defs;
	int		   *stateoff;		/* palabra donde empieza cada estado */
	int			words;			/* palabras por entrada */

	Size		budget;
	Size		table_budget;	/* el presupuesto menos los buffers */
	uint32		max_cap;
	int			max_parts;

	MemoryContext cxt;			/* tabla, directorio, particiones */
	MemoryContext arenacxt;		/* textos de las claves */
	MemoryContext outcxt;		/* valores por referencia de la salida */

	uint64	   *entries;
	uint32		cap;
	uint32		nentries;
	uint32	   *dir;
	uint32		dirmask;

	char	   *arena_ptr;
	Size		arena_left;
	Size		arena_bytes;	/* reservado en bloques */

	/* Volcado del nivel que se está procesando. */
	int			level;
	int			nparts;
	DbeFlatPart *parts;
	List	   *pending;		/* DbeFlatPart * por procesar */

	char	   *rec;			/* un registro, crece si hace falta */
	Size		rec_cap;
	Size		fixed_rowlen;	/* sin claves de texto: largo de un registro */
	Size		fixed_statelen;	/* ... y de uno con estados */
	char	   *keybuf[DBE_FLAT_MAX_KEYS];	/* textos leídos de un registro */
	Size		keybuf_cap[DBE_FLAT_MAX_KEYS];

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
 * Los bytes de un texto.  Lo que llega del lector del segmento está sin
 * comprimir (columnar_write.c), pero un valor por omisión de una columna
 * agregada después podría no estarlo.
 */
static inline void
dbe_flat_text(Datum d, const char **data, Size *len)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTENDED(v) && !VARATT_IS_SHORT(v))
		v = pg_detoast_datum_packed(v);
	*data = VARDATA_ANY(v);
	*len = VARSIZE_ANY_EXHDR(v);
}

/* Bytes de la tabla con `cap` entradas: entradas más directorio. */
static Size
dbe_flat_table_bytes(DbeFlatAgg *fa, uint32 cap)
{
	return sizeof(uint64) * fa->words * (Size) cap +
		sizeof(uint32) * (Size) pg_nextpower2_32(2 * cap);
}

static void
dbe_flat_alloc(DbeFlatAgg *fa, uint32 cap)
{
	uint64	   *old = fa->entries;
	uint32		n = fa->nentries;
	uint32		nslots = pg_nextpower2_32(2 * cap);

	fa->entries = (uint64 *) MemoryContextAllocHuge(fa->cxt,
													sizeof(uint64) * fa->words * (Size) cap);
	if (old != NULL)
	{
		memcpy(fa->entries, old, sizeof(uint64) * fa->words * (Size) n);
		pfree(old);
	}
	if (fa->dir != NULL)
		pfree(fa->dir);
	fa->dir = (uint32 *) MemoryContextAllocHuge(fa->cxt,
												sizeof(uint32) * (Size) nslots);
	memset(fa->dir, 0, sizeof(uint32) * (Size) nslots);
	fa->dirmask = nslots - 1;
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

	fa->peak = Max(fa->peak, dbe_flat_table_bytes(fa, cap) + fa->arena_bytes);
}

DbeFlatAgg *
dbe_flat_create(int nkeys, const int16 *keylen, int naggs,
				const DbeFlatAggDef *defs, Size budget, double est_groups)
{
	DbeFlatAgg *fa;
	int			w;
	double		cap;
	Size		per_entry;

	Assert(nkeys >= 1 && nkeys <= DBE_FLAT_MAX_KEYS);

	fa = (DbeFlatAgg *) palloc0(sizeof(DbeFlatAgg));
	fa->nkeys = nkeys;
	memcpy(fa->keylen, keylen, sizeof(int16) * nkeys);
	for (int i = 0; i < nkeys; i++)
		if (keylen[i] < 0)
			fa->anyvar = true;
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

	/*
	 * Un octavo del presupuesto para los buffers de las particiones (un
	 * bloque cada una), el resto para la tabla.
	 */
	fa->budget = budget;
	fa->max_parts = (int) Min(256, Max(4, budget / 8 / BLCKSZ));
	fa->max_parts = (int) pg_prevpower2_32((uint32) fa->max_parts);
	fa->table_budget = budget - (Size) fa->max_parts * BLCKSZ;

	/*
	 * Las entradas no tienen que ser potencia de dos, solo el directorio.
	 * Redondear para abajo dejaba sin usar hasta la mitad del presupuesto,
	 * y con eso cada partición de q33 quedaba apenas más grande que la
	 * tabla y volcaba un nivel más entero.  Con claves de texto el arena
	 * comparte el presupuesto, y se reserva lugar para un texto de 64 bytes
	 * por entrada: si son más largos, la tabla deja de crecer antes.
	 */
	per_entry = sizeof(uint64) * fa->words + 4 * sizeof(uint32) +
		(fa->anyvar ? 64 : 0);
	fa->max_cap = (uint32) Min((Size) PG_INT32_MAX / 8,
							   Max((Size) 64, fa->table_budget / per_entry));

	fa->cxt = AllocSetContextCreate(CurrentMemoryContext, "dbengine flat hash",
									ALLOCSET_DEFAULT_SIZES);
	fa->arenacxt = AllocSetContextCreate(CurrentMemoryContext,
										 "dbengine flat hash textos",
										 ALLOCSET_DEFAULT_SIZES);
	fa->outcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "dbengine flat hash salida",
									   ALLOCSET_SMALL_SIZES);
	if (!fa->anyvar)
	{
		Size		base = 3 * sizeof(uint32);

		for (int i = 0; i < nkeys; i++)
			base += keylen[i];
		fa->fixed_rowlen = base;
		for (int i = 0; i < naggs; i++)
			fa->fixed_rowlen += defs[i].vallen;
		fa->fixed_statelen = base + sizeof(uint64) * (w - 1 - nkeys);
	}
	fa->rec_cap = Max((Size) 256, fa->fixed_statelen);
	fa->rec = MemoryContextAlloc(fa->cxt, fa->rec_cap);

	{
		Datum		zero[2] = {Int64GetDatum(0), Int64GetDatum(0)};
		MemoryContext old = MemoryContextSwitchTo(fa->cxt);

		fa->avgarr = construct_array_builtin(zero, 2, INT8OID);
		MemoryContextSwitchTo(old);
	}

	/* Arranca chica si se esperan pocos grupos; crece de a doble. */
	cap = Min((double) fa->max_cap, Max(256.0, est_groups));
	dbe_flat_alloc(fa, (uint32) cap);
	fa->phase = DBE_FLAT_FILL;
	return fa;
}

static pg_attribute_always_inline uint64
dbe_flat_hash(DbeFlatAgg *fa, const uint64 *keyw, uint32 nullmask)
{
	uint64		h = 0;

	if (!fa->anyvar)
	{
		for (int i = 0; i < fa->nkeys; i++)
			h = hash_combine64(h, murmurhash64(keyw[i]));
		return hash_combine64(h, nullmask);
	}
	for (int i = 0; i < fa->nkeys; i++)
	{
		if (fa->keylen[i] < 0 && (nullmask & ((uint32) 1 << i)) == 0)
		{
			const char *p;
			Size		len;

			dbe_flat_text((Datum) keyw[i], &p, &len);
			h = hash_combine64(h, hash_bytes_extended((const unsigned char *) p,
													  (int) len, 0));
		}
		else
			h = hash_combine64(h, murmurhash64(keyw[i]));
	}
	return hash_combine64(h, nullmask);
}

/* ¿La entrada tiene estas claves?  La cabecera (hash y nulos) ya coincide. */
static pg_attribute_always_inline bool
dbe_flat_keys_equal(DbeFlatAgg *fa, const uint64 *e, const uint64 *keyw,
					uint32 nullmask)
{
	if (!fa->anyvar)
		return memcmp(e + 1, keyw, sizeof(uint64) * fa->nkeys) == 0;

	for (int i = 0; i < fa->nkeys; i++)
	{
		if (fa->keylen[i] < 0 && (nullmask & ((uint32) 1 << i)) == 0)
		{
			const char *a;
			const char *b;
			Size		la;
			Size		lb;

			dbe_flat_text((Datum) e[1 + i], &a, &la);
			dbe_flat_text((Datum) keyw[i], &b, &lb);
			if (la != lb || memcmp(a, b, la) != 0)
				return false;
		}
		else if (e[1 + i] != keyw[i])
			return false;
	}
	return true;
}

/* Suma una fila a los estados de un grupo (`st`: el primero). */
static pg_attribute_always_inline void
dbe_flat_update(DbeFlatAgg *fa, uint64 *st, const Datum *vals,
				const bool *valnulls)
{
	for (int i = 0; i < fa->naggs; i++)
	{
		uint64	   *s = st + (fa->stateoff[i] - 1 - fa->nkeys);
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

/* Combina estados ya agregados: lo que hace un Finalize. */
static void
dbe_flat_merge(DbeFlatAgg *fa, uint64 *e, const uint64 *states)
{
	for (int i = 0; i < fa->naggs; i++)
	{
		uint64	   *s = e + fa->stateoff[i];
		const uint64 *r = states + (fa->stateoff[i] - 1 - fa->nkeys);

		switch (fa->defs[i].kind)
		{
			case DBE_FLAT_COUNT_STAR:
			case DBE_FLAT_COUNT:
				s[0] += r[0];
				break;
			case DBE_FLAT_SUM:
			case DBE_FLAT_AVG:
				s[0] = (uint64) ((int64) s[0] + (int64) r[0]);
				s[1] += r[1];
				break;
			case DBE_FLAT_MIN:
			case DBE_FLAT_MAX:
				if (r[1] == 0)
					break;
				if (s[1] == 0 ||
					(fa->defs[i].kind == DBE_FLAT_MIN ? (int64) r[0] < (int64) s[0]
					 : (int64) r[0] > (int64) s[0]))
					s[0] = r[0];
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

static inline void
dbe_flat_rec_reserve(DbeFlatAgg *fa, Size need)
{
	if (need > fa->rec_cap)
	{
		fa->rec_cap = Max(need, fa->rec_cap * 2);
		fa->rec = repalloc(fa->rec, fa->rec_cap);
	}
}

/*
 * Un registro: [largo][nulos de las claves][nulos de los argumentos][hash]
 * [claves][argumentos o estados].  El hash va solo con claves de texto,
 * donde recalcularlo al leer es la mitad de la búsqueda; con claves enteras
 * es más barato recalcularlo que escribir 8 bytes más por fila.  Una clave de ancho fijo va con su ancho; una de
 * texto, con su largo en 4 bytes y los bytes, o nada si es nula.
 */
static void
dbe_flat_spill(DbeFlatAgg *fa, uint64 h, uint32 nullmask, const uint64 *keyw,
			   const Datum *vals, const bool *valnulls, const uint64 *states)
{
	uint32		valnullmask = 0;
	Size		len = 3 * sizeof(uint32) + (fa->anyvar ? sizeof(uint64) : 0);
	char	   *p;
	uint32		total;
	int			part;
	uint32		recmask = nullmask | (states != NULL ? DBE_FLAT_REC_STATES : 0);
	int			nstates = fa->words - 1 - fa->nkeys;

	if (fa->parts == NULL)
		dbe_flat_open_parts(fa);

	/* Otros bits que los que repartieron el nivel anterior. */
	part = (int) (murmurhash64(h ^ (UINT64CONST(0x9E3779B97F4A7C15) *
									(uint64) (fa->level + 1))) &
				  (uint64) (fa->nparts - 1));

	if (!fa->anyvar)
		len = states != NULL ? fa->fixed_statelen : fa->fixed_rowlen;
	else
	{
		for (int i = 0; i < fa->nkeys; i++)
		{
			if (fa->keylen[i] >= 0)
				len += fa->keylen[i];
			else if ((nullmask & ((uint32) 1 << i)) == 0)
			{
				const char *d;
				Size		l;

				dbe_flat_text((Datum) keyw[i], &d, &l);
				len += sizeof(uint32) + l;
			}
		}
		if (states != NULL)
			len += sizeof(uint64) * nstates;
		else
			for (int i = 0; i < fa->naggs; i++)
				len += fa->defs[i].vallen;
		dbe_flat_rec_reserve(fa, len);
	}

	/* count(col) no guarda el valor pero sí si es nulo. */
	if (states == NULL)
		for (int i = 0; i < fa->naggs; i++)
			if (fa->defs[i].kind != DBE_FLAT_COUNT_STAR && valnulls[i])
				valnullmask |= (uint32) 1 << i;

	p = fa->rec;
	total = (uint32) len;
	memcpy(p, &total, sizeof(uint32));
	p += sizeof(uint32);
	memcpy(p, &recmask, sizeof(uint32));
	p += sizeof(uint32);
	memcpy(p, &valnullmask, sizeof(uint32));
	p += sizeof(uint32);
	if (fa->anyvar)
	{
		memcpy(p, &h, sizeof(uint64));
		p += sizeof(uint64);
	}
	for (int i = 0; i < fa->nkeys; i++)
	{
		union
		{
			int64		align;
			char		bytes[8];
		}			tmp;

		if (fa->keylen[i] < 0)
		{
			const char *d;
			Size		l;
			uint32		l32;

			if (nullmask & ((uint32) 1 << i))
				continue;
			dbe_flat_text((Datum) keyw[i], &d, &l);
			l32 = (uint32) l;
			memcpy(p, &l32, sizeof(uint32));
			p += sizeof(uint32);
			memcpy(p, d, l);
			p += l;
			continue;
		}
		store_att_byval(tmp.bytes, (Datum) keyw[i], fa->keylen[i]);
		memcpy(p, tmp.bytes, fa->keylen[i]);
		p += fa->keylen[i];
	}
	if (states != NULL)
	{
		memcpy(p, states, sizeof(uint64) * nstates);
		p += sizeof(uint64) * nstates;
	}
	else
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
	Assert((Size) (p - fa->rec) == len);

	BufFileWrite(fa->parts[part].file, fa->rec, len);
	fa->parts[part].rows++;
	fa->spilled_rows++;
}

/*
 * Copia un texto al arena, con cabecera de 4 bytes.  false si no entra en
 * el presupuesto; con la tabla vacía entra igual, porque si no un texto más
 * grande que el presupuesto se volcaría para siempre.
 */
static bool
dbe_flat_arena_copy(DbeFlatAgg *fa, Datum src, Datum *dst)
{
	const char *d;
	Size		len;
	Size		need;
	struct varlena *v;

	dbe_flat_text(src, &d, &len);
	need = MAXALIGN(len + VARHDRSZ);
	if (need > fa->arena_left)
	{
		Size		block = Max((Size) DBE_FLAT_ARENA_BLOCK, need);

		if (fa->nentries > 0 &&
			dbe_flat_table_bytes(fa, fa->cap) + fa->arena_bytes + block >
			fa->table_budget)
			return false;
		fa->arena_ptr = MemoryContextAlloc(fa->arenacxt, block);
		fa->arena_left = block;
		fa->arena_bytes += block;
		fa->peak = Max(fa->peak, dbe_flat_table_bytes(fa, fa->cap) +
					   fa->arena_bytes);
	}
	v = (struct varlena *) fa->arena_ptr;
	SET_VARSIZE(v, len + VARHDRSZ);
	memcpy(VARDATA(v), d, len);
	fa->arena_ptr += need;
	fa->arena_left -= need;
	*dst = PointerGetDatum(v);
	return true;
}

/* ¿Puede la tabla duplicarse sin pasarse del presupuesto? */
static bool
dbe_flat_can_grow(DbeFlatAgg *fa, uint32 *newcap)
{
	uint32		c;

	if (fa->cap >= fa->max_cap)
		return false;
	c = (uint32) Min((uint64) fa->max_cap, (uint64) fa->cap * 2);
	if (dbe_flat_table_bytes(fa, c) + fa->arena_bytes > fa->table_budget)
		return false;
	*newcap = c;
	return true;
}

/*
 * Busca el grupo y lo crea si no está.  Devuelve el índice de la entrada, o
 * -1 si no está y no hay lugar; *hash queda con el hash, para volcar.  El
 * índice vale hasta que la tabla se vacíe para procesar una partición:
 * crecer no mueve las entradas de lugar.
 */
static pg_attribute_always_inline int64
dbe_flat_lookup_hashed(DbeFlatAgg *fa, const uint64 *keyw, uint32 nullmask,
					   uint64 h)
{
	uint64		head = (h << 32) | nullmask;
	uint32		pos;
	uint64	   *e;
	uint32		newcap;

	for (pos = (uint32) h & fa->dirmask;; pos = (pos + 1) & fa->dirmask)
	{
		uint32		k = fa->dir[pos];

		if (k == 0)
			break;
		e = fa->entries + (Size) (k - 1) * fa->words;
		if (e[0] == head && dbe_flat_keys_equal(fa, e, keyw, nullmask))
			return (int64) (k - 1);
	}

	if (fa->nentries >= fa->cap)
	{
		if (!dbe_flat_can_grow(fa, &newcap))
			return -1;
		dbe_flat_alloc(fa, newcap);
		/* El directorio cambió de tamaño: buscar la ranura de nuevo. */
		for (pos = (uint32) h & fa->dirmask; fa->dir[pos] != 0;
			 pos = (pos + 1) & fa->dirmask)
			;
	}

	e = fa->entries + (Size) fa->nentries * fa->words;
	e[0] = head;
	for (int i = 0; i < fa->nkeys; i++)
	{
		if (fa->keylen[i] < 0 && (nullmask & ((uint32) 1 << i)) == 0)
		{
			Datum		copy;

			if (!dbe_flat_arena_copy(fa, (Datum) keyw[i], &copy))
				return -1;
			e[1 + i] = (uint64) copy;
		}
		else
			e[1 + i] = keyw[i];
	}
	memset(e + 1 + fa->nkeys, 0,
		   sizeof(uint64) * (fa->words - 1 - fa->nkeys));
	fa->nentries++;
	fa->dir[pos] = fa->nentries;
	return (int64) (fa->nentries - 1);
}

static int64
dbe_flat_lookup(DbeFlatAgg *fa, const uint64 *keyw, uint32 nullmask,
				uint64 *hash)
{
	*hash = dbe_flat_hash(fa, keyw, nullmask);
	return dbe_flat_lookup_hashed(fa, keyw, nullmask, *hash);
}

static inline void
dbe_flat_keywords(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls,
				  uint64 *keyw, uint32 *nullmask)
{
	*nullmask = 0;
	for (int i = 0; i < fa->nkeys; i++)
	{
		if (keynulls[i])
		{
			*nullmask |= (uint32) 1 << i;
			keyw[i] = 0;
		}
		else
			keyw[i] = (uint64) keys[i];
	}
}

uint32
dbe_flat_bucket(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls,
				uint32 nbuckets)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	uint32		nullmask;

	dbe_flat_keywords(fa, keys, keynulls, keyw, &nullmask);

	/*
	 * Otra mezcla que la del directorio y la de las particiones: si fuera la
	 * misma, todos los grupos de un balde caerían en las mismas ranuras.
	 */
	return (uint32) (murmurhash64(dbe_flat_hash(fa, keyw, nullmask) ^
								  UINT64CONST(0xC2B2AE3D27D4EB4F)) % nbuckets);
}

int64
dbe_flat_find(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	uint32		nullmask;
	uint64		h;

	Assert(fa->phase == DBE_FLAT_FILL);
	dbe_flat_keywords(fa, keys, keynulls, keyw, &nullmask);
	return dbe_flat_lookup(fa, keyw, nullmask, &h);
}

int
dbe_flat_nstates(DbeFlatAgg *fa)
{
	return fa->words - 1 - fa->nkeys;
}

void
dbe_flat_accum(DbeFlatAgg *fa, uint64 *states, const Datum *vals,
			   const bool *valnulls)
{
	dbe_flat_update(fa, states, vals, valnulls);
}

void
dbe_flat_spill_states(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls,
					  const uint64 *states)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	uint32		nullmask;
	uint64		h;
	int64		idx;

	dbe_flat_keywords(fa, keys, keynulls, keyw, &nullmask);
	idx = dbe_flat_lookup(fa, keyw, nullmask, &h);
	if (idx >= 0)
		dbe_flat_merge(fa, fa->entries + (Size) idx * fa->words, states);
	else
		dbe_flat_spill(fa, h, nullmask, keyw, NULL, NULL, states);
}

void
dbe_flat_add_to(DbeFlatAgg *fa, int64 idx, const Datum *vals,
				const bool *valnulls)
{
	Assert(fa->phase == DBE_FLAT_FILL && idx >= 0 && idx < fa->nentries);
	dbe_flat_update(fa, fa->entries + (Size) idx * fa->words + 1 + fa->nkeys,
					vals, valnulls);
}

void
dbe_flat_add(DbeFlatAgg *fa, const Datum *keys, const bool *keynulls,
			 const Datum *vals, const bool *valnulls)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	uint32		nullmask;
	uint64		h;
	int64		idx;

	Assert(fa->phase == DBE_FLAT_FILL);
	dbe_flat_keywords(fa, keys, keynulls, keyw, &nullmask);
	idx = dbe_flat_lookup(fa, keyw, nullmask, &h);
	if (idx >= 0)
		dbe_flat_update(fa, fa->entries + (Size) idx * fa->words + 1 + fa->nkeys,
						vals, valnulls);
	else
		dbe_flat_spill(fa, h, nullmask, keyw, vals, valnulls, NULL);
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
	memset(fa->dir, 0, sizeof(uint32) * (fa->dirmask + 1));
	MemoryContextReset(fa->arenacxt);
	fa->arena_ptr = NULL;
	fa->arena_left = 0;
	fa->arena_bytes = 0;
}

/* Un texto leído de un registro, como varlena en un buffer de la clave. */
static Datum
dbe_flat_read_text(DbeFlatAgg *fa, int i, const char **pp)
{
	uint32		l;
	struct varlena *v;

	memcpy(&l, *pp, sizeof(uint32));
	*pp += sizeof(uint32);
	if (fa->keybuf_cap[i] < l + VARHDRSZ)
	{
		if (fa->keybuf[i] != NULL)
			pfree(fa->keybuf[i]);
		fa->keybuf_cap[i] = Max((Size) 64, (Size) (l + VARHDRSZ) * 2);
		fa->keybuf[i] = MemoryContextAlloc(fa->cxt, fa->keybuf_cap[i]);
	}
	v = (struct varlena *) fa->keybuf[i];
	SET_VARSIZE(v, l + VARHDRSZ);
	memcpy(VARDATA(v), *pp, l);
	*pp += l;
	return PointerGetDatum(v);
}

/* Procesa una partición pendiente entera: queda lista para emitir. */
static void
dbe_flat_load_part(DbeFlatAgg *fa, DbeFlatPart *part)
{
	uint64		keyw[DBE_FLAT_MAX_KEYS];
	Datum	   *vals = (Datum *) palloc(sizeof(Datum) * Max(fa->naggs, 1));
	bool	   *valnulls = (bool *) palloc(sizeof(bool) * Max(fa->naggs, 1));
	int			nstates = fa->words - 1 - fa->nkeys;
	uint64	   *statebuf = (uint64 *) palloc(sizeof(uint64) * Max(nstates, 1));

	if (part->level > DBE_FLAT_MAX_LEVEL)
		elog(ERROR, "dbengine: la agregación volcó más de %d niveles",
			 DBE_FLAT_MAX_LEVEL);

	dbe_flat_clear_table(fa);
	fa->level = part->level;
	fa->phase = DBE_FLAT_FILL;

	for (uint64 r = 0; r < part->rows; r++)
	{
		const char *p;
		uint32		total;
		uint32		nullmask;
		uint32		valnullmask;
		bool		has_states;
		const uint64 *states = NULL;
		uint64		h = 0;
		int64		idx;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Sin claves de texto todos los registros son de filas y del mismo
		 * largo: los de estados salen de la caché por código de diccionario,
		 * que exige una clave de texto.  Se leen de una vez.
		 */
		if (!fa->anyvar)
			BufFileReadExact(part->file, fa->rec, fa->fixed_rowlen);
		else
		{
			BufFileReadExact(part->file, &total, sizeof(uint32));
			dbe_flat_rec_reserve(fa, total);
			BufFileReadExact(part->file, fa->rec + sizeof(uint32),
							 total - sizeof(uint32));
		}
		p = fa->rec + sizeof(uint32);
		memcpy(&nullmask, p, sizeof(uint32));
		p += sizeof(uint32);
		has_states = (nullmask & DBE_FLAT_REC_STATES) != 0;
		nullmask &= ~DBE_FLAT_REC_STATES;
		memcpy(&valnullmask, p, sizeof(uint32));
		p += sizeof(uint32);
		if (fa->anyvar)
		{
			memcpy(&h, p, sizeof(uint64));
			p += sizeof(uint64);
		}
		for (int i = 0; i < fa->nkeys; i++)
		{
			union
			{
				int64		align;
				char		bytes[8];
			}			tmp;

			if (fa->keylen[i] < 0)
			{
				keyw[i] = (nullmask & ((uint32) 1 << i)) ? 0
					: (uint64) dbe_flat_read_text(fa, i, &p);
				continue;
			}
			memcpy(tmp.bytes, p, fa->keylen[i]);
			keyw[i] = (uint64) fetch_att(tmp.bytes, true, fa->keylen[i]);
			p += fa->keylen[i];
		}

		if (!fa->anyvar)
			h = dbe_flat_hash(fa, keyw, nullmask);

		/* Los estados se leen a un buffer alineado: p no lo está. */
		if (has_states)
		{
			memcpy(statebuf, p, sizeof(uint64) * nstates);
			states = statebuf;
			idx = dbe_flat_lookup_hashed(fa, keyw, nullmask, h);
			if (idx >= 0)
				dbe_flat_merge(fa, fa->entries + (Size) idx * fa->words, states);
			else
				dbe_flat_spill(fa, h, nullmask, keyw, NULL, NULL, states);
			continue;
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
		idx = dbe_flat_lookup_hashed(fa, keyw, nullmask, h);
		if (idx >= 0)
			dbe_flat_update(fa, fa->entries + (Size) idx * fa->words + 1 + fa->nkeys,
							vals, valnulls);
		else
			dbe_flat_spill(fa, h, nullmask, keyw, vals, valnulls, NULL);
	}
	BufFileClose(part->file);
	pfree(part);
	pfree(vals);
	pfree(valnulls);
	pfree(statebuf);
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

	/*
	 * Las cifras del EXPLAIN no se tocan: en paralelo esto se llama entre un
	 * balde y el siguiente, y son la suma de todos.  Con un rescan se
	 * acumulan, como las demás cifras con loops > 1.
	 */
	dbe_flat_clear_table(fa);
	fa->level = 0;
	fa->phase = DBE_FLAT_FILL;
	fa->emit_pos = 0;
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
