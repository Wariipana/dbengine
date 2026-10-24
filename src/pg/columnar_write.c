/*-------------------------------------------------------------------------
 *
 * columnar_write.c
 *	  Buffer de escritura orientado a filas (R8) y flush a stripe columnar.
 *
 * Postgres llama a tuple_insert() una vez por fila.  Escribir un stripe por
 * fila sería catastrófico, así que las filas se juntan acá y se escriben de
 * a un stripe.
 *
 * El buffer puede cruzar comandos y hasta scans: la visibilidad no depende
 * de CUÁNDO se escribe el stripe sino del (xid, cid) que se registra en él.
 * Ver doc/adr-002-visibilidad-de-stripes.md.
 *
 * Un buffer agrupa filas de un solo par (xid, cid).  Si llega una fila con
 * otro cid, se flushea primero.  Consecuencia conocida: N INSERT de una
 * fila en una transacción producen N stripes.  El delta store persistente
 * que lo resuelve es el punto que queda abierto de R8.
 *
 * ATRIBUCIÓN DE SUBTRANSACCIÓN
 *
 * Flushear tarde tiene una trampa.  La fila de metadatos del stripe se
 * inserta en la (sub)transacción ACTUAL, que no es necesariamente la que
 * insertó las filas:
 *
 *     BEGIN;
 *     INSERT INTO t VALUES (4);   -- queda en el buffer, dueño: top-level
 *     SAVEPOINT sp;
 *     INSERT INTO t VALUES (5);   -- cambia el xid -> flushea la 4 ACÁ ADENTRO
 *     ROLLBACK TO sp;             -- y se lleva la fila de metadatos de la 4
 *
 * La visibilidad (ADR-002) no alcanza para arreglarlo: el problema no es
 * que la fila de metadatos se vea, es que deja de EXISTIR.  Tampoco se
 * puede flushear antes: SUBXACT_EVENT_START_SUB ya corre adentro de la
 * subtransacción nueva.
 *
 * Solución: si el flush cae en una subtransacción distinta de la dueña de
 * las filas, el buffer NO se libera, se RETIENE.  Si esa subtransacción
 * aborta, se lo vuelve a escribir; si commitea, la fila de metadatos pasó a
 * un nivel más externo y recién ahí se libera.  Los bytes del stripe
 * perdido quedan como basura en el storage hasta el próximo VACUUM.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/datum.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "dbengine/bridge.h"
#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/workload.h"
#include "dbengine/dbengine.h"
#include "dbengine/storage.h"

/*
 * Zone map en construcción (R7).
 *
 * El mínimo y el máximo se calculan con la función de comparación btree
 * del tipo, no comparando bytes: para `text` con collation, para `numeric`
 * o para cualquier tipo con orden propio, comparar bytes daría un rango
 * incorrecto y el pruning descartaría bloques que sí hacían falta.
 *
 * Por eso esto vive en la capa C y no en el motor: es conocimiento de
 * tipos, y el motor no tiene ninguno.
 */
typedef struct DbeZoneMapAcc
{
	bool		usable;			/* el tipo tiene orden btree */
	bool		seen;			/* hubo al menos un valor no nulo */
	FmgrInfo	cmp;
	Oid			collation;
	int16		typlen;
	bool		typbyval;
	Datum		min_value;
	Datum		max_value;

	/*
	 * Zone map por gránulo (ver DBE_GRANULE_ROWS en catalog.h).  Solo para
	 * tipos por valor de 1, 2, 4 u 8 bytes: ahí el Datum ES el valor, así
	 * que guardar un min/max por gránulo no cuesta ninguna copia, y son los
	 * tipos donde el descarte por rango sirve.
	 */
	bool		gran_usable;
	uint64		nrows;			/* filas vistas, nulos incluidos */
	uint32		gran_count;
	uint32		gran_capacity;
	Datum	   *gran_min;
	Datum	   *gran_max;
	bool	   *gran_seen;
} DbeZoneMapAcc;

/*
 * Acumulador del filtro de Bloom del segmento (F5).
 *
 * Los hashes se juntan y el filtro se arma en el flush, cuando ya se sabe
 * cuántas filas tiene el stripe: dimensionarlo antes obligaría a usar el
 * máximo y un stripe corto pagaría un filtro de 64 kB para cien valores.
 *
 * El hash es el de la familia hash del tipo, no uno propio.  Eso es lo que
 * garantiza que el valor de la consulta —que puede ser de otro tipo, como
 * el integer de `id = 5000` sobre un bigint— hashee igual: las familias
 * hash de Postgres exigen exactamente esa propiedad entre tipos.
 */
typedef struct DbeBloomAcc
{
	bool		enabled;
	FmgrInfo	hash;
	Oid			collation;
	uint64	   *hashes;
	uint64		nhashes;
	uint64		capacity;
} DbeBloomAcc;

typedef struct DbeStagingBuffer
{
	Oid			relid;			/* clave del hash */
	int			natts;
	DbeSegBuilder **builders;
	uint64	   *null_counts;
	DbeZoneMapAcc *zonemaps;
	DbeBloomAcc *blooms;

	/*
	 * Command id de cada fila.  Es lo que permite que un stripe abarque
	 * varios comandos: sin esto el buffer tenía que cerrarse en cada
	 * INSERT, y N INSERT de una fila daban N stripes.
	 */
	DbeSegBuilder *cid_builder;
	CommandId	cid_min;
	CommandId	cid_max;
	uint64		nrows;
	uint64		base_rownum;	/* números de fila reservados al abrir */
	uint64		reserved_rows;
	FullTransactionId insert_xid;
	CommandId	insert_cid;
	MemoryContext cxt;

	/* Atribución de subtransacción; ver el comentario de arriba. */
	SubTransactionId owner_subxid;	/* dónde se insertaron las filas */
	SubTransactionId flush_subxid;	/* dónde se insertó la fila de metadatos */
	bool		needs_reflush;
} DbeStagingBuffer;

static HTAB *dbe_staging_tab = NULL;
static MemoryContext dbe_staging_cxt = NULL;
static bool dbe_staging_callbacks_done = false;

/* Buffers ya flusheados cuya fila de metadatos todavía puede desaparecer. */
static List *dbe_retained = NIL;

/*
 * Reservas de números de fila escritas por esta transacción (ver
 * dbengine.row_reservation).  Se borran en el pre-commit; hasta entonces
 * son las que le avisan al chequeo de unicidad de otra sesión que hay filas
 * en curso.  Vive en TopTransactionContext.
 */
typedef struct DbeReservation
{
	RelFileNumber relnumber;
	uint64		first_row;
	SubTransactionId subxid;	/* dónde se insertó la fila de la reserva */
} DbeReservation;

static List *dbe_reservations = NIL;

static void dbe_staging_xact_callback(XactEvent event, void *arg);
static void dbe_staging_subxact_callback(SubXactEvent event,
										 SubTransactionId mySubid,
										 SubTransactionId parentSubid,
										 void *arg);
static void dbe_staging_flush_buffer(DbeStagingBuffer *buf);
static void dbe_staging_free_buffer(DbeStagingBuffer *buf);
static void dbe_staging_close_buffer(DbeStagingBuffer *buf);
static void dbe_staging_reflush_pending(void);

/*
 * Ancho de slot para un tipo de ancho fijo.
 *
 * Los anchos 1/2/4/8 se guardan tal cual: como el arranque del área de
 * datos está alineado a 8 y el stride divide a 8, cada valor cae alineado.
 * Un ancho raro (macaddr son 6 bytes) rompería esa propiedad, así que se
 * redondea a MAXALIGN.  Cuesta espacio solo en tipos poco frecuentes.
 */
static uint32
dbe_fixed_stride(int16 attlen)
{
	if (attlen == 1 || attlen == 2 || attlen == 4 || attlen == 8)
		return (uint32) attlen;
	return (uint32) MAXALIGN(attlen);
}

static void
dbe_check_supported_type(Form_pg_attribute att, const char *relname)
{
	if (att->attlen == -2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: la columna \"%s\" de \"%s\" es de tipo "
						"cstring, que el storage columnar no soporta",
						NameStr(att->attname), relname)));
	if (att->attlen < -2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dbengine: attlen %d no soportado en la columna \"%s\"",
						att->attlen, NameStr(att->attname))));
}

/*
 * Prepara el acumulador de zone map de una columna.  Si el tipo no tiene
 * operador de comparación btree, la columna simplemente no tiene zone map:
 * no es un error, solo se pierde la posibilidad de podar por ella.
 */
static void
dbe_zonemap_init(DbeZoneMapAcc *zm, Form_pg_attribute att, MemoryContext cxt)
{
	TypeCacheEntry *typentry;

	memset(zm, 0, sizeof(*zm));
	zm->typlen = att->attlen;
	zm->typbyval = att->attbyval;
	zm->collation = att->attcollation;

	typentry = lookup_type_cache(att->atttypid, TYPECACHE_CMP_PROC_FINFO);
	if (typentry->cmp_proc_finfo.fn_oid == InvalidOid)
		return;

	fmgr_info_cxt(typentry->cmp_proc_finfo.fn_oid, &zm->cmp, cxt);
	zm->usable = true;

	zm->gran_usable = att->attbyval &&
		(att->attlen == 1 || att->attlen == 2 ||
		 att->attlen == 4 || att->attlen == 8);
}

/*
 * ¿Se construye filtro para esta columna?
 *
 * Por defecto solo si el canal de feedback vio predicados de igualdad sobre
 * ella: pagar el filtro en todas las columnas de todas las tablas para
 * servirle a unas pocas consultas es exactamente lo que la medición de F5
 * desaconseja.  La GUC lo fuerza para poder medir.
 */
static void
dbe_bloom_acc_init(DbeBloomAcc *acc, Form_pg_attribute att,
				   RelFileNumber relnumber, MemoryContext cxt)
{
	TypeCacheEntry *typentry;

	memset(acc, 0, sizeof(*acc));

	if (att->attisdropped)
		return;
	if (dbe_bloom_bits_per_row == 0 &&
		!dbe_workload_wants_bloom(relnumber, att->attnum))
		return;

	typentry = lookup_type_cache(att->atttypid, TYPECACHE_HASH_PROC_FINFO);
	if (typentry->hash_proc_finfo.fn_oid == InvalidOid)
		return;					/* el tipo no tiene hash: no hay filtro */

	fmgr_info_cxt(typentry->hash_proc_finfo.fn_oid, &acc->hash, cxt);
	acc->collation = att->attcollation;
	acc->capacity = 1024;
	acc->hashes = (uint64 *)
		MemoryContextAlloc(cxt, sizeof(uint64) * acc->capacity);
	acc->nhashes = 0;
	acc->enabled = true;
}

static void
dbe_bloom_acc_add(DbeBloomAcc *acc, Datum value, MemoryContext cxt)
{
	uint32		h;

	if (!acc->enabled)
		return;

	if (acc->nhashes == acc->capacity)
	{
		MemoryContext old = MemoryContextSwitchTo(cxt);

		acc->capacity *= 2;
		acc->hashes = (uint64 *) repalloc(acc->hashes,
										  sizeof(uint64) * acc->capacity);
		MemoryContextSwitchTo(old);
	}

	h = DatumGetUInt32(FunctionCall1Coll(&acc->hash, acc->collation, value));

	/*
	 * El hash del tipo es de 32 bits y el filtro trabaja con 64.  Se
	 * ensancha con una mezcla, no con un cast: los bits altos en cero
	 * degradarían las k posiciones a una sola familia.
	 */
	acc->hashes[acc->nhashes++] =
		((uint64) h) | (((uint64) (h * 0x9e3779b9u)) << 32);
}

static void
dbe_bloom_acc_finish(DbeBloomAcc *acc, DbeSegDesc *seg)
{
	DbeBloomBuilder *b = NULL;
	const void *bytes = NULL;
	uint64		len = 0;
	DbeStatus	st;
	uint32		bits;
	uint64		i;

	seg->bloom = NULL;
	seg->bloom_length = 0;

	if (!acc->enabled || acc->nhashes == 0)
		return;

	bits = dbe_bloom_bits_per_row > 0 ? (uint32) dbe_bloom_bits_per_row : 8;

	st = dbe_bloom_builder_create(acc->nhashes, bits, &b);
	dbe_error_if_not_ok(&st, "creando el filtro de Bloom del segmento");

	PG_TRY();
	{
		for (i = 0; i < acc->nhashes; i++)
		{
			st = dbe_bloom_builder_add(b, acc->hashes[i]);
			dbe_error_if_not_ok(&st, "llenando el filtro de Bloom");
		}
		st = dbe_bloom_builder_finish(b, &bytes, &len);
		dbe_error_if_not_ok(&st, "serializando el filtro de Bloom");

		seg->bloom = (const char *) memcpy(palloc(len), bytes, len);
		seg->bloom_length = (uint32) len;
	}
	PG_FINALLY();
	{
		dbe_bloom_destroy(b);
	}
	PG_END_TRY();
}

/*
 * Cuenta la fila en el gránulo que le toca y, si no es nula, ajusta su
 * min/max.  El gránulo se elige por número de fila —nulos incluidos—
 * porque el lector saltea rangos de filas, no de valores.
 */
static void
dbe_zonemap_granule_add(DbeZoneMapAcc *zm, Datum value, bool isnull,
						MemoryContext cxt)
{
	uint32		g;

	zm->nrows++;
	if (!zm->gran_usable || !zm->usable)
		return;

	g = (uint32) ((zm->nrows - 1) / DBE_GRANULE_ROWS);

	if (g >= zm->gran_capacity)
	{
		MemoryContext old = MemoryContextSwitchTo(cxt);
		uint32		want = Max(zm->gran_capacity * 2, g + 8);

		if (zm->gran_min == NULL)
		{
			zm->gran_min = (Datum *) palloc(sizeof(Datum) * want);
			zm->gran_max = (Datum *) palloc(sizeof(Datum) * want);
			zm->gran_seen = (bool *) palloc0(sizeof(bool) * want);
		}
		else
		{
			zm->gran_min = (Datum *) repalloc(zm->gran_min,
											  sizeof(Datum) * want);
			zm->gran_max = (Datum *) repalloc(zm->gran_max,
											  sizeof(Datum) * want);
			zm->gran_seen = (bool *) repalloc(zm->gran_seen,
											  sizeof(bool) * want);
			memset(zm->gran_seen + zm->gran_capacity, 0,
				   sizeof(bool) * (want - zm->gran_capacity));
		}
		zm->gran_capacity = want;
		MemoryContextSwitchTo(old);
	}

	if (g >= zm->gran_count)
		zm->gran_count = g + 1;

	if (isnull)
		return;

	if (!zm->gran_seen[g])
	{
		zm->gran_seen[g] = true;
		zm->gran_min[g] = value;
		zm->gran_max[g] = value;
		return;
	}

	if (DatumGetInt32(FunctionCall2Coll(&zm->cmp, zm->collation,
										value, zm->gran_min[g])) < 0)
		zm->gran_min[g] = value;
	else if (DatumGetInt32(FunctionCall2Coll(&zm->cmp, zm->collation,
											 value, zm->gran_max[g])) > 0)
		zm->gran_max[g] = value;
}

static void
dbe_zonemap_add(DbeZoneMapAcc *zm, Datum value, bool isnull, MemoryContext cxt)
{
	MemoryContext old;

	dbe_zonemap_granule_add(zm, value, isnull, cxt);

	if (!zm->usable || isnull)
		return;

	if (!zm->seen)
	{
		old = MemoryContextSwitchTo(cxt);
		zm->min_value = datumCopy(value, zm->typbyval, zm->typlen);
		zm->max_value = datumCopy(value, zm->typbyval, zm->typlen);
		MemoryContextSwitchTo(old);
		zm->seen = true;
		return;
	}

	if (DatumGetInt32(FunctionCall2Coll(&zm->cmp, zm->collation,
										value, zm->min_value)) < 0)
	{
		old = MemoryContextSwitchTo(cxt);
		if (!zm->typbyval)
			pfree(DatumGetPointer(zm->min_value));
		zm->min_value = datumCopy(value, zm->typbyval, zm->typlen);
		MemoryContextSwitchTo(old);
	}
	else if (DatumGetInt32(FunctionCall2Coll(&zm->cmp, zm->collation,
											 value, zm->max_value)) > 0)
	{
		old = MemoryContextSwitchTo(cxt);
		if (!zm->typbyval)
			pfree(DatumGetPointer(zm->max_value));
		zm->max_value = datumCopy(value, zm->typbyval, zm->typlen);
		MemoryContextSwitchTo(old);
	}
}

/*
 * Vuelca el mínimo y el máximo acumulados a la representación binaria del
 * tipo, que es la que va al catálogo.
 */
static void
dbe_zonemap_value_bytes(const DbeZoneMapAcc *zm, Datum value,
						const char **out, uint32 *len)
{
	if (zm->typlen > 0)
	{
		char	   *buf = (char *) palloc0(zm->typlen);

		if (zm->typbyval)
			store_att_byval(buf, value, zm->typlen);
		else
			memcpy(buf, DatumGetPointer(value), zm->typlen);

		*out = buf;
		*len = (uint32) zm->typlen;
	}
	else
	{
		struct varlena *v =
			pg_detoast_datum_packed((struct varlena *) DatumGetPointer(value));

		*out = (const char *) v;
		*len = (uint32) VARSIZE_ANY(v);
	}
}

static void
dbe_zonemap_finish(DbeZoneMapAcc *zm, DbeSegDesc *seg)
{
	seg->has_minmax = false;

	if (!zm->usable || !zm->seen)
		return;

	dbe_zonemap_value_bytes(zm, zm->min_value, &seg->min_value, &seg->min_length);
	dbe_zonemap_value_bytes(zm, zm->max_value, &seg->max_value, &seg->max_length);

	/*
	 * Un zone map sirve para descartar bloques, no para guardar datos: si
	 * los valores no entran en el presupuesto, el zone map cuesta más de lo
	 * que puede ahorrar y se descarta.
	 */
	if (seg->min_length > DBE_ZONEMAP_MAX_BYTES ||
		seg->max_length > DBE_ZONEMAP_MAX_BYTES)
		return;

	seg->has_minmax = true;
}

/*
 * Serializa el zone map por gránulo al layout que documenta catalog.h.
 *
 * Con un solo gránulo no se guarda nada: sería repetir el min/max del
 * stripe y cobrarle catálogo a todas las tablas chicas.
 */
static void
dbe_zonemap_granules_finish(DbeZoneMapAcc *zm, DbeSegDesc *seg)
{
	char	   *buf;
	Size		len;
	uint32		n = zm->gran_count;
	int			w = zm->typlen;
	uint32		i;

	seg->granules = NULL;
	seg->granules_length = 0;

	if (!zm->gran_usable || !zm->usable || n < 2)
		return;

	len = DBE_GRANULE_BYTES(n, w);
	buf = (char *) palloc0(len);

	buf[0] = DBE_GRANULE_VERSION;
	buf[1] = (char) w;
	{
		uint16		rows = DBE_GRANULE_ROWS;
		uint16		count = (uint16) n;

		memcpy(buf + 2, &rows, sizeof(rows));
		memcpy(buf + 4, &count, sizeof(count));
	}

	for (i = 0; i < n; i++)
	{
		if (!zm->gran_seen[i])
			continue;			/* gránulo todo nulo: flags queda en cero */

		buf[DBE_GRANULE_FLAGS_OFF + i] = DBE_GRANULE_FLAG_SEEN;
		store_att_byval(buf + DBE_GRANULE_MIN_OFF(n) + (Size) i * w,
						zm->gran_min[i], w);
		store_att_byval(buf + DBE_GRANULE_MAX_OFF(n, w) + (Size) i * w,
						zm->gran_max[i], w);
	}

	seg->granules = buf;
	seg->granules_length = (uint32) len;
}

static void
dbe_staging_ensure_tab(void)
{
	HASHCTL		ctl;

	if (dbe_staging_tab != NULL)
		return;

	dbe_staging_cxt = AllocSetContextCreate(TopMemoryContext,
											"dbengine staging",
											ALLOCSET_DEFAULT_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(DbeStagingBuffer);
	ctl.hcxt = dbe_staging_cxt;

	dbe_staging_tab = hash_create("dbengine staging buffers", 8, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

void
dbe_staging_register_callbacks(void)
{
	if (dbe_staging_callbacks_done)
		return;

	RegisterXactCallback(dbe_staging_xact_callback, NULL);
	RegisterSubXactCallback(dbe_staging_subxact_callback, NULL);
	dbe_staging_callbacks_done = true;
}

/*
 * ¿El buffer se armó para esta forma de la tabla?
 *
 * Los builders se crean por columna al abrir el buffer.  Si desde entonces
 * se agregó una columna, las filas nuevas traen un valor que el buffer no
 * tiene dónde guardar; si se borró una, hay un builder de más.  En los dos
 * casos hay que cerrar el stripe y abrir otro con la forma actual.  Las
 * filas que ya estaban quedan en un stripe sin segmento para la columna
 * nueva, y el lector las completa con attmissingval, que es exactamente la
 * semántica de ADD COLUMN.
 */
static bool
dbe_staging_desc_matches(DbeStagingBuffer *buf, TupleDesc desc)
{
	int			i;

	if (buf->natts != desc->natts)
		return false;

	for (i = 0; i < desc->natts; i++)
		if ((buf->builders[i] == NULL) != TupleDescAttr(desc, i)->attisdropped)
			return false;

	return true;
}

/*
 * ¿Hace falta publicar la reserva?  Solo si algún índice puede rechazar una
 * fila por otra: único o de exclusión.  Sin eso, nadie de afuera necesita
 * saber de filas que todavía no están en el storage.
 */
static bool
dbe_relation_needs_reservation(Relation rel)
{
	List	   *indexes;
	ListCell   *lc;
	bool		needed = false;

	if (!rel->rd_rel->relhasindex)
		return false;

	indexes = RelationGetIndexList(rel);
	foreach(lc, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID,
										  ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index idx;

		if (!HeapTupleIsValid(tup))
			continue;
		idx = (Form_pg_index) GETSTRUCT(tup);
		needed = idx->indisunique || idx->indisexclusion;
		ReleaseSysCache(tup);
		if (needed)
			break;
	}
	list_free(indexes);

	return needed;
}

static void
dbe_publish_reservation(Relation rel, uint64 first_row, uint64 row_count)
{
	DbeReservation *r;
	MemoryContext old;

	dbe_catalog_insert_reservation(rel->rd_locator.relNumber, first_row,
								   row_count);

	old = MemoryContextSwitchTo(TopTransactionContext);
	r = (DbeReservation *) palloc(sizeof(DbeReservation));
	r->relnumber = rel->rd_locator.relNumber;
	r->first_row = first_row;
	r->subxid = GetCurrentSubTransactionId();
	dbe_reservations = lappend(dbe_reservations, r);
	MemoryContextSwitchTo(old);
}

/*
 * Borra las reservas propias.  Va DESPUÉS de vaciar los buffers: mientras
 * la transacción no commitee, la fila borrada sigue siendo visible para
 * SnapshotDirty con el xmin en curso, así que nadie deja de esperar antes
 * de tiempo.
 *
 * El CommandCounterIncrement no es opcional.  En un INSERT o un COPY en
 * autocommit la reserva se insertó en el mismo comando que está terminando,
 * y heap no deja borrar una tupla insertada por el comando actual.
 */
static void
dbe_drop_own_reservations(void)
{
	ListCell   *lc;

	if (dbe_reservations == NIL)
		return;

	CommandCounterIncrement();
	foreach(lc, dbe_reservations)
	{
		DbeReservation *r = (DbeReservation *) lfirst(lc);

		dbe_catalog_delete_reservation(r->relnumber, r->first_row);
	}
	dbe_reservations = NIL;
}

static DbeStagingBuffer *
dbe_staging_lookup(Oid relid, bool create, Relation rel, CommandId cid)
{
	DbeStagingBuffer *buf;
	bool		found;

	dbe_staging_ensure_tab();
	dbe_staging_register_callbacks();

	buf = (DbeStagingBuffer *) hash_search(dbe_staging_tab, &relid,
										   create ? HASH_ENTER : HASH_FIND,
										   &found);
	if (buf == NULL)
		return NULL;

	if (found)
	{
		if (!create)
			return buf;

		/*
		 * La entrada puede seguir en el hash con los builders ya liberados:
		 * pasa cuando el buffer se flusheó por superar el umbral y siguen
		 * llegando filas del mismo comando.  En ese caso hay que rearmarlo.
		 */
		if (buf->builders != NULL)
		{
			/*
			 * El buffer agrupa filas de UNA transacción; el comando puede
			 * cambiar libremente porque el cid se guarda por fila.  Un xid
			 * distinto (otra transacción, o una subtransacción nueva) sí
			 * obliga a cerrar.
			 *
			 * También lo obliga un cambio de forma de la tabla en el medio
			 * de la transacción.  Un ALTER TABLE ADD COLUMN no reescribe ni
			 * pasa por el Table AM: el buffer seguía con los builders de
			 * antes y el valor de la columna nueva se tiraba en silencio.
			 */
			if (FullTransactionIdEquals(buf->insert_xid,
										GetCurrentFullTransactionId()) &&
				dbe_staging_desc_matches(buf, RelationGetDescr(rel)))
				return buf;

			dbe_staging_flush_buffer(buf);
			dbe_staging_close_buffer(buf);
		}
	}

	/* Inicializar (entrada nueva, o reciclada después del flush). */
	{
		TupleDesc	desc = RelationGetDescr(rel);
		MemoryContext old;
		int			i;

		buf->relid = relid;
		buf->natts = desc->natts;
		buf->nrows = 0;

		/*
		 * Se reservan de una todos los números de fila que el buffer podría
		 * llegar a usar, para poder darle a cada INSERT su TID definitivo.
		 * Los que sobren quedan como huecos entre stripes.  El umbral se
		 * congela acá: es una GUC y puede cambiar en el medio.
		 */
		buf->reserved_rows = (uint64) dbe_staging_threshold_rows;
		buf->base_rownum = dbe_storage_reserve_rows(rel, buf->reserved_rows);

		/*
		 * Antes de devolver el primer TID: el executor lo mete en el índice
		 * apenas volvemos, y desde ese momento otra sesión puede tropezar
		 * con él.
		 */
		if (dbe_relation_needs_reservation(rel))
			dbe_publish_reservation(rel, buf->base_rownum, buf->reserved_rows);
		buf->insert_xid = GetCurrentFullTransactionId();
		buf->insert_cid = cid;
		buf->cid_min = cid;
		buf->cid_max = cid;
		buf->owner_subxid = GetCurrentSubTransactionId();
		buf->flush_subxid = InvalidSubTransactionId;
		buf->needs_reflush = false;
		buf->cxt = AllocSetContextCreate(dbe_staging_cxt,
										 "dbengine staging buffer",
										 ALLOCSET_DEFAULT_SIZES);

		old = MemoryContextSwitchTo(buf->cxt);
		buf->builders = (DbeSegBuilder **) palloc0(sizeof(DbeSegBuilder *) * buf->natts);
		buf->null_counts = (uint64 *) palloc0(sizeof(uint64) * buf->natts);
		buf->zonemaps = (DbeZoneMapAcc *) palloc0(sizeof(DbeZoneMapAcc) * buf->natts);
		buf->blooms = (DbeBloomAcc *) palloc0(sizeof(DbeBloomAcc) * buf->natts);
		MemoryContextSwitchTo(old);

		{
			DbeStatus	st = dbe_segbuilder_create(sizeof(CommandId), 1,
												   &buf->cid_builder);

			dbe_error_if_not_ok(&st, "creando el segmento de command ids");
		}

		for (i = 0; i < buf->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);
			DbeStatus	st;
			uint32		width;

			if (att->attisdropped)
				continue;

			dbe_check_supported_type(att, RelationGetRelationName(rel));

			width = (att->attlen > 0) ? dbe_fixed_stride(att->attlen) : 0;
			st = dbe_segbuilder_create(width, dbe_enable_compression ? 1 : 0,
									   &buf->builders[i]);
			dbe_error_if_not_ok(&st, "creando el builder de segmento");

			dbe_zonemap_init(&buf->zonemaps[i], att, buf->cxt);
			dbe_bloom_acc_init(&buf->blooms[i], att,
							   rel->rd_locator.relNumber, buf->cxt);
		}
	}

	return buf;
}

/*
 * Serializa un valor no nulo al builder del segmento.
 *
 * Ancho fijo: se copia con el stride del tipo, alineado.  Largo variable:
 * se guarda el varlena YA materializado, con su header, para que al leerlo
 * vuelva a ser un Datum válido sin copiar.  No hay tabla TOAST propia; los
 * valores grandes van inline y el archivo lógico los reparte entre páginas.
 */
static void
dbe_append_value(DbeSegBuilder *builder, Form_pg_attribute att, Datum value)
{
	DbeStatus	st;

	if (att->attlen > 0)
	{
		uint32		stride = dbe_fixed_stride(att->attlen);
		char		tmp[MAXIMUM_ALIGNOF * 8];
		char	   *p = tmp;

		if (stride > sizeof(tmp))
			p = (char *) palloc0(stride);
		else
			memset(tmp, 0, stride);

		if (att->attbyval)
			store_att_byval(p, value, att->attlen);
		else
			memcpy(p, DatumGetPointer(value), att->attlen);

		st = dbe_segbuilder_append(builder, p, stride, 0);

		if (p != tmp)
			pfree(p);
	}
	else
	{
		struct varlena *v =
			pg_detoast_datum_packed((struct varlena *) DatumGetPointer(value));

		/*
		 * Header corto cuando se puede, igual que heap_fill_tuple(): ocupa 3
		 * bytes menos y, sobre todo, se puede leer desde cualquier
		 * dirección.  Uno de 4 bytes desalineado obliga a copiarlo al leer
		 * (dbe_varlena_datum).  Los tipos con storage plain esperan siempre
		 * el header largo.
		 */
		if (VARATT_IS_4B_U(v) && att->attstorage != TYPSTORAGE_PLAIN &&
			VARATT_CAN_MAKE_SHORT(v))
		{
			char		buf[VARATT_SHORT_MAX];
			Size		len = VARATT_CONVERTED_SHORT_SIZE(v);

			SET_VARSIZE_SHORT(buf, len);
			memcpy(buf + 1, VARDATA(v), len - 1);
			st = dbe_segbuilder_append(builder, buf, (uint32) len, 0);
		}
		else
			st = dbe_segbuilder_append(builder, v, VARSIZE_ANY(v), 0);

		if ((Pointer) v != DatumGetPointer(value))
			pfree(v);
	}

	dbe_error_if_not_ok(&st, "agregando un valor al segmento");
}

void
dbe_staging_add(Relation rel, TupleTableSlot *slot, CommandId cid,
				uint64 *rownum_out)
{
	DbeStagingBuffer *buf;
	TupleDesc	desc = RelationGetDescr(rel);
	int			i;

	slot_getallattrs(slot);

	dbe_staging_reflush_pending();
	buf = dbe_staging_lookup(RelationGetRelid(rel), true, rel, cid);

	for (i = 0; i < buf->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		DbeStatus	st;
		bool		isnull = slot->tts_isnull[i];
		Datum		value = slot->tts_values[i];

		if (buf->builders[i] == NULL)	/* columna borrada */
			continue;

		if (isnull)
		{
			/* El nulo ocupa una fila: el gránulo se cuenta igual. */
			dbe_zonemap_add(&buf->zonemaps[i], (Datum) 0, true, buf->cxt);
			st = dbe_segbuilder_append(buf->builders[i], NULL, 0, 1);
			buf->null_counts[i]++;
		}
		else
		{
			dbe_zonemap_add(&buf->zonemaps[i], value, false, buf->cxt);
			dbe_bloom_acc_add(&buf->blooms[i], value, buf->cxt);
			dbe_append_value(buf->builders[i], att, value);
			continue;
		}

		dbe_error_if_not_ok(&st, "agregando un valor al segmento");
	}

	{
		DbeStatus	st = dbe_segbuilder_append(buf->cid_builder, &cid,
											   sizeof(CommandId), 0);

		dbe_error_if_not_ok(&st, "registrando el command id de la fila");
	}
	if (cid < buf->cid_min)
		buf->cid_min = cid;
	if (cid > buf->cid_max)
		buf->cid_max = cid;

	if (rownum_out != NULL)
		*rownum_out = buf->base_rownum + buf->nrows;
	buf->nrows++;

	if (buf->nrows >= buf->reserved_rows)
	{
		dbe_staging_flush_buffer(buf);
		dbe_staging_close_buffer(buf);
	}
}

/*
 * Escribe el buffer como un stripe.  El (xid, cid) que se registra es el
 * que tenía el buffer, no el del momento del flush: eso es lo que hace que
 * flushear tarde siga siendo visible temprano.
 */
/*
 * Escribe un stripe a partir de builders ya llenos y mete sus metadatos.
 *
 * Lo comparten el flush del buffer de escritura y la fusión de stripes de
 * F6.  Es el punto donde se decide el layout —los segmentos arrancan
 * alineados y el stripe entero se reserva de una— y donde se calculan los
 * zone maps y los filtros; tenerlo dos veces sería tener dos formatos.
 */
static uint64
dbe_emit_stripe(Relation rel, DbeSegBuilder **builders, int natts,
				const uint64 *null_counts, DbeZoneMapAcc *zonemaps,
				DbeBloomAcc *blooms, DbeSegBuilder *cid_builder,
				bool write_cids, uint64 nrows, uint64 first_row_number,
				FullTransactionId xid, CommandId cid_min, CommandId cid_max,
				bool has_commit_ts, TimestampTz commit_ts)
{
	DbeStripeDesc stripe;
	uint64	   *seg_len;
	uint64	   *seg_off;
	const void **seg_bytes;
	uint64		total = 0;
	uint64		base;
	int			i;

	/* Una entrada extra al final para el segmento de command ids. */
	seg_len = (uint64 *) palloc0(sizeof(uint64) * (natts + 1));
	seg_off = (uint64 *) palloc0(sizeof(uint64) * (natts + 1));
	seg_bytes = (const void **) palloc0(sizeof(void *) * (natts + 1));

	for (i = 0; i < natts; i++)
	{
		DbeStatus	st;
		uint32		len;

		if (builders[i] == NULL)
			continue;

		st = dbe_segbuilder_finish(builders[i], &seg_bytes[i], &len);
		dbe_error_if_not_ok(&st, "serializando un segmento");

		seg_off[i] = total;
		seg_len[i] = len;
		total += MAXALIGN(len);		/* los segmentos arrancan alineados */
	}

	if (write_cids)
	{
		DbeStatus	st;
		uint32		len;

		st = dbe_segbuilder_finish(cid_builder, &seg_bytes[natts], &len);
		dbe_error_if_not_ok(&st, "serializando el segmento de command ids");

		seg_off[natts] = total;
		seg_len[natts] = len;
		total += MAXALIGN(len);
	}

	base = dbe_storage_reserve_bytes(rel, total, &stripe.stripe_id);
	stripe.first_row_number = first_row_number;

	for (i = 0; i < natts; i++)
	{
		if (builders[i] == NULL)
			continue;
		dbe_storage_write(rel, base + seg_off[i],
						  (const char *) seg_bytes[i], seg_len[i]);
	}
	if (seg_len[natts] > 0)
		dbe_storage_write(rel, base + seg_off[natts],
						  (const char *) seg_bytes[natts], seg_len[natts]);

	stripe.row_count = nrows;
	stripe.data_offset = base;
	stripe.data_length = total;
	stripe.insert_xid = xid;
	stripe.insert_cid = cid_min;
	stripe.insert_cid_max = cid_max;
	stripe.has_commit_ts = has_commit_ts;
	stripe.commit_ts = commit_ts;

	dbe_catalog_insert_stripe(rel->rd_locator.relNumber, &stripe);

	if (seg_len[natts] > 0)
	{
		DbeSegDesc	seg;
		int32		encoding = 0;
		DbeStatus	st;

		/*
		 * memset y no campo por campo: has_minmax con basura del stack hacía
		 * que se leyera un puntero inventado como si fuera el zone map.
		 */
		memset(&seg, 0, sizeof(seg));
		seg.attnum = DBE_ATTNUM_CID;
		seg.seg_offset = seg_off[natts];
		seg.seg_length = seg_len[natts];

		st = dbe_segbuilder_encoding(cid_builder, &encoding, NULL, NULL);
		dbe_error_if_not_ok(&st, "consultando la codificación de los cids");
		seg.encoding = (int16) encoding;

		dbe_catalog_insert_segment(rel->rd_locator.relNumber,
								   stripe.stripe_id, &seg);
	}

	for (i = 0; i < natts; i++)
	{
		DbeSegDesc	seg;
		int32		encoding = 0;
		DbeStatus	st;

		if (builders[i] == NULL)
			continue;

		memset(&seg, 0, sizeof(seg));
		seg.attnum = (AttrNumber) (i + 1);
		seg.seg_offset = seg_off[i];
		seg.seg_length = seg_len[i];
		seg.null_count = null_counts[i];

		st = dbe_segbuilder_encoding(builders[i], &encoding, NULL, NULL);
		dbe_error_if_not_ok(&st, "consultando la codificación del segmento");
		seg.encoding = (int16) encoding;

		dbe_zonemap_finish(&zonemaps[i], &seg);
		dbe_zonemap_granules_finish(&zonemaps[i], &seg);
		dbe_bloom_acc_finish(&blooms[i], &seg);

		dbe_catalog_insert_segment(rel->rd_locator.relNumber, stripe.stripe_id,
								   &seg);
	}

	pfree(seg_len);
	pfree(seg_off);
	pfree(seg_bytes);

	return stripe.stripe_id;
}

static void
dbe_staging_flush_buffer(DbeStagingBuffer *buf)
{
	Relation	rel;
	bool		pushed_snapshot = false;

	if (buf->nrows == 0 || buf->builders == NULL)
		return;

	/* La tabla puede haberse borrado en esta misma transacción. */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(buf->relid)))
		return;

	/*
	 * El flush escribe en dbengine.stripe_column, que tiene columnas bytea
	 * y por lo tanto tabla TOAST — y tocar TOAST necesita un snapshot.
	 *
	 * Casi siempre hay uno activo, pero no en XACT_EVENT_PRE_COMMIT: para
	 * ese momento Postgres ya sacó el snapshot del portal.  Justo ahí es
	 * donde se vacía lo que quedó pendiente al terminar la transacción.
	 */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_snapshot = true;
	}

	rel = table_open(buf->relid, RowExclusiveLock);

	(void) dbe_emit_stripe(rel, buf->builders, buf->natts, buf->null_counts,
						   buf->zonemaps, buf->blooms, buf->cid_builder,
						   buf->cid_min != buf->cid_max, buf->nrows,
						   buf->base_rownum, buf->insert_xid,
						   buf->cid_min, buf->cid_max, false, 0);

	/*
	 * Un stripe nuevo va al final, así que el orden físico que hubiera
	 * dejado un CLUSTER deja de valer.  CLUSTER pasa por acá también, pero
	 * escribe su marca DESPUÉS de vaciar, así que no se pisa a sí mismo.
	 * La fusión de stripes no pasa por acá: respeta el orden y conserva la
	 * marca.
	 */
	dbe_catalog_clear_order(rel->rd_locator.relNumber);

	/*
	 * Devolver los números de fila reservados de más mantiene el espacio
	 * de TIDs denso.  Hace falta para que más adelante se puedan fusionar
	 * stripes contiguos sin mover TIDs.
	 */
	dbe_storage_release_rows(rel, buf->base_rownum, buf->reserved_rows,
							 buf->nrows);

	buf->flush_subxid = GetCurrentSubTransactionId();

	table_close(rel, RowExclusiveLock);

	if (pushed_snapshot)
		PopActiveSnapshot();
}

/*
 * Cierra un buffer ya flusheado.
 *
 * Si la fila de metadatos quedó en la misma subtransacción que insertó las
 * filas, no hay riesgo y se libera.  Si quedó en una más profunda, se
 * retiene: esa subtransacción puede abortar y llevarse la fila de
 * metadatos, y entonces hay que volver a escribir el stripe.
 */
static void
dbe_staging_close_buffer(DbeStagingBuffer *buf)
{
	DbeStagingBuffer *retained;
	MemoryContext old;

	if (buf->flush_subxid == buf->owner_subxid ||
		buf->flush_subxid == InvalidSubTransactionId)
	{
		dbe_staging_free_buffer(buf);
		return;
	}

	retained = (DbeStagingBuffer *) MemoryContextAlloc(dbe_staging_cxt,
													  sizeof(DbeStagingBuffer));
	*retained = *buf;			/* se lleva los builders y su contexto */

	old = MemoryContextSwitchTo(dbe_staging_cxt);
	dbe_retained = lappend(dbe_retained, retained);
	MemoryContextSwitchTo(old);

	/* La entrada del hash queda vacía, sin liberar lo que se llevó el retenido. */
	buf->builders = NULL;
	buf->null_counts = NULL;
	buf->cxt = NULL;
	buf->nrows = 0;
}

/*
 * Vuelve a escribir los stripes cuya fila de metadatos se perdió en una
 * subtransacción abortada.  Se llama de forma perezosa desde los puntos de
 * entrada normales: adentro del callback de abort no se puede escribir en
 * catálogos.
 */
static void
dbe_staging_reflush_pending(void)
{
	List	   *keep = NIL;
	ListCell   *lc;
	MemoryContext old;

	if (dbe_retained == NIL)
		return;

	foreach(lc, dbe_retained)
	{
		DbeStagingBuffer *buf = (DbeStagingBuffer *) lfirst(lc);

		if (buf->needs_reflush)
		{
			buf->needs_reflush = false;
			dbe_staging_flush_buffer(buf);
		}

		if (buf->flush_subxid == buf->owner_subxid)
			dbe_staging_free_buffer(buf);
		else
		{
			old = MemoryContextSwitchTo(dbe_staging_cxt);
			keep = lappend(keep, buf);
			MemoryContextSwitchTo(old);
		}
	}

	list_free(dbe_retained);
	dbe_retained = keep;
}

static void
dbe_staging_free_buffer(DbeStagingBuffer *buf)
{
	int			i;

	/*
	 * Puede llamarse sobre un buffer ya cerrado: la entrada sigue en el
	 * hash después de un flush por umbral, y el abort de la transacción
	 * recorre todas.
	 */
	if (buf->builders != NULL)
	{
		for (i = 0; i < buf->natts; i++)
		{
			if (buf->builders[i] != NULL)
			{
				dbe_segbuilder_destroy(buf->builders[i]);
				buf->builders[i] = NULL;
			}
		}
	}
	if (buf->cid_builder != NULL)
	{
		dbe_segbuilder_destroy(buf->cid_builder);
		buf->cid_builder = NULL;
	}
	if (buf->cxt != NULL)
	{
		MemoryContextDelete(buf->cxt);
		buf->cxt = NULL;
	}
	buf->nrows = 0;
	buf->builders = NULL;
	buf->null_counts = NULL;
}


/* ------------------------------------------------------------------------
 * Escritor de stripes suelto (F6)
 *
 * El buffer de escritura junta filas de una transacción y les asigna
 * números de fila nuevos.  La fusión de stripes necesita lo contrario:
 * escribir un stripe con números de fila YA DADOS, los de las filas que
 * está reemplazando.  Conservarlos es lo que hace que fusionar no mueva
 * ningún TID y por lo tanto no toque ningún índice.
 * ------------------------------------------------------------------------ */

struct DbeStripeWriter
{
	Relation	rel;
	int			natts;
	DbeSegBuilder **builders;
	uint64	   *null_counts;
	DbeZoneMapAcc *zonemaps;
	DbeBloomAcc *blooms;
	uint64		first_row_number;
	uint64		nrows;
	bool		has_commit_ts;
	TimestampTz commit_ts;
	MemoryContext cxt;
};

void
dbe_stripe_writer_set_commit_ts(DbeStripeWriter *w, TimestampTz ts)
{
	w->has_commit_ts = true;
	w->commit_ts = ts;
}

DbeStripeWriter *
dbe_stripe_writer_begin(Relation rel, uint64 first_row_number)
{
	TupleDesc	desc = RelationGetDescr(rel);
	DbeStripeWriter *w;
	MemoryContext cxt;
	MemoryContext old;
	int			i;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "dbengine stripe writer",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);

	w = (DbeStripeWriter *) palloc0(sizeof(DbeStripeWriter));
	w->rel = rel;
	w->natts = desc->natts;
	w->cxt = cxt;
	w->first_row_number = first_row_number;
	w->builders = (DbeSegBuilder **) palloc0(sizeof(DbeSegBuilder *) * w->natts);
	w->null_counts = (uint64 *) palloc0(sizeof(uint64) * w->natts);
	w->zonemaps = (DbeZoneMapAcc *) palloc0(sizeof(DbeZoneMapAcc) * w->natts);
	w->blooms = (DbeBloomAcc *) palloc0(sizeof(DbeBloomAcc) * w->natts);

	MemoryContextSwitchTo(old);

	for (i = 0; i < w->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		DbeStatus	st;
		uint32		width;

		if (att->attisdropped)
			continue;

		width = (att->attlen > 0) ? dbe_fixed_stride(att->attlen) : 0;
		st = dbe_segbuilder_create(width, dbe_enable_compression ? 1 : 0,
								   &w->builders[i]);
		dbe_error_if_not_ok(&st, "creando el builder de segmento");

		dbe_zonemap_init(&w->zonemaps[i], att, w->cxt);
		dbe_bloom_acc_init(&w->blooms[i], att, rel->rd_locator.relNumber,
						   w->cxt);
	}

	return w;
}

void
dbe_stripe_writer_add(DbeStripeWriter *w, TupleTableSlot *slot)
{
	int			i;

	slot_getallattrs(slot);

	for (i = 0; i < w->natts; i++)
	{
		DbeStatus	st;

		if (w->builders[i] == NULL)
			continue;

		if (slot->tts_isnull[i])
		{
			dbe_zonemap_add(&w->zonemaps[i], (Datum) 0, true, w->cxt);
			st = dbe_segbuilder_append(w->builders[i], NULL, 0, 1);
			dbe_error_if_not_ok(&st, "agregando un nulo al segmento");
			w->null_counts[i]++;
		}
		else
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(w->rel), i);
			Datum		value = slot->tts_values[i];

			dbe_zonemap_add(&w->zonemaps[i], value, false, w->cxt);
			dbe_bloom_acc_add(&w->blooms[i], value, w->cxt);
			dbe_append_value(w->builders[i], att, value);
		}
	}

	w->nrows++;
}

/*
 * Cierra el stripe.  Va con FrozenTransactionId: la fusión solo toma
 * stripes que ya son visibles para todos, así que el resultado también lo
 * es, y de paso deja de depender del clog para siempre.
 */
uint64
dbe_stripe_writer_finish(DbeStripeWriter *w)
{
	uint64		stripe_id;

	stripe_id = dbe_emit_stripe(w->rel, w->builders, w->natts, w->null_counts,
								w->zonemaps, w->blooms, NULL, false, w->nrows,
								w->first_row_number,
								FullTransactionIdFromEpochAndXid(0, FrozenTransactionId),
								InvalidCommandId, InvalidCommandId,
								w->has_commit_ts, w->commit_ts);
	dbe_stripe_writer_end(w);
	return stripe_id;
}

void
dbe_stripe_writer_end(DbeStripeWriter *w)
{
	int			i;

	for (i = 0; i < w->natts; i++)
		if (w->builders[i] != NULL)
		{
			dbe_segbuilder_destroy(w->builders[i]);
			w->builders[i] = NULL;
		}
	MemoryContextDelete(w->cxt);
}

uint64
dbe_stripe_writer_rows(DbeStripeWriter *w)
{
	return w->nrows;
}

void
dbe_staging_flush_relation(Oid relid)
{
	DbeStagingBuffer *buf;

	dbe_staging_reflush_pending();

	if (dbe_staging_tab == NULL)
		return;

	buf = (DbeStagingBuffer *) hash_search(dbe_staging_tab, &relid,
										   HASH_FIND, NULL);
	if (buf == NULL || buf->nrows == 0)
		return;

	dbe_staging_flush_buffer(buf);
	dbe_staging_close_buffer(buf);
	hash_search(dbe_staging_tab, &relid, HASH_REMOVE, NULL);
}

void
dbe_staging_flush_all(void)
{
	HASH_SEQ_STATUS status;
	
	DbeStagingBuffer *buf;
	List	   *relids = NIL;
	ListCell   *lc;

	dbe_staging_reflush_pending();

	if (dbe_staging_tab == NULL)
		return;

	/* No se puede modificar el hash mientras se lo recorre. */
	hash_seq_init(&status, dbe_staging_tab);
	while ((buf = (DbeStagingBuffer *) hash_seq_search(&status)) != NULL)
		relids = lappend_oid(relids, buf->relid);

	foreach(lc, relids)
		dbe_staging_flush_relation(lfirst_oid(lc));

	list_free(relids);
}

void
dbe_staging_forget(Oid relid)
{
	DbeStagingBuffer *buf;

	if (dbe_staging_tab == NULL)
		return;

	buf = (DbeStagingBuffer *) hash_search(dbe_staging_tab, &relid,
										   HASH_FIND, NULL);
	if (buf == NULL)
		return;

	dbe_staging_free_buffer(buf);
	hash_search(dbe_staging_tab, &relid, HASH_REMOVE, NULL);
}

/* Tira todo: solo para el abort de la transacción de más afuera. */
static void
dbe_staging_discard_all(void)
{
	if (dbe_staging_tab == NULL)
		return;

	/*
	 * Después de un abort no vale la pena recorrer estructura por
	 * estructura: se tira el contexto entero y se rearma en el próximo
	 * insert.  Los builders de C++ viven en malloc, así que hay que
	 * destruirlos a mano antes.
	 */
	{
		HASH_SEQ_STATUS status;
		DbeStagingBuffer *buf;
		ListCell   *lc;

		hash_seq_init(&status, dbe_staging_tab);
		while ((buf = (DbeStagingBuffer *) hash_seq_search(&status)) != NULL)
			dbe_staging_free_buffer(buf);

		foreach(lc, dbe_retained)
			dbe_staging_free_buffer((DbeStagingBuffer *) lfirst(lc));
	}

	dbe_retained = NIL;
	hash_destroy(dbe_staging_tab);
	dbe_staging_tab = NULL;
	MemoryContextDelete(dbe_staging_cxt);
	dbe_staging_cxt = NULL;
}

/* Descarta solo lo que pertenece a la subtransacción que aborta. */
static void
dbe_staging_discard_subxact(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS status;
	DbeStagingBuffer *buf;
	List	   *doomed = NIL;
	List	   *keep = NIL;
	ListCell   *lc;
	MemoryContext old;

	if (dbe_staging_tab != NULL)
	{
		hash_seq_init(&status, dbe_staging_tab);
		while ((buf = (DbeStagingBuffer *) hash_seq_search(&status)) != NULL)
		{
			if (buf->owner_subxid == mySubid)
				doomed = lappend_oid(doomed, buf->relid);
		}

		foreach(lc, doomed)
			dbe_staging_forget(lfirst_oid(lc));
		list_free(doomed);
	}

	/*
	 * Los retenidos cuya fila de metadatos se insertó en esta
	 * subtransacción acaban de perderla: hay que reescribir el stripe.  Si
	 * además las filas eran de esta subtransacción, no hay nada que
	 * rescatar.
	 */
	foreach(lc, dbe_retained)
	{
		DbeStagingBuffer *r = (DbeStagingBuffer *) lfirst(lc);

		if (r->owner_subxid == mySubid)
		{
			dbe_staging_free_buffer(r);
			continue;
		}
		if (r->flush_subxid == mySubid)
			r->needs_reflush = true;

		old = MemoryContextSwitchTo(dbe_staging_cxt);
		keep = lappend(keep, r);
		MemoryContextSwitchTo(old);
	}

	list_free(dbe_retained);
	dbe_retained = keep;

	/* Las reservas insertadas en esta subtransacción murieron con ella. */
	{
		List	   *alive = NIL;

		foreach(lc, dbe_reservations)
		{
			DbeReservation *r = (DbeReservation *) lfirst(lc);

			if (r->subxid != mySubid)
			{
				old = MemoryContextSwitchTo(TopTransactionContext);
				alive = lappend(alive, r);
				MemoryContextSwitchTo(old);
			}
		}
		dbe_reservations = alive;
	}
}

/*
 * La subtransacción commiteó: lo que estaba anotado a su nombre pasa al
 * padre.  Si con eso la fila de metadatos queda al mismo nivel que las
 * filas, el buffer retenido ya no hace falta.
 */
static void
dbe_staging_promote_subxact(SubTransactionId mySubid,
							SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS status;
	DbeStagingBuffer *buf;
	List	   *keep = NIL;
	ListCell   *lc;
	MemoryContext old;

	if (dbe_staging_tab != NULL)
	{
		hash_seq_init(&status, dbe_staging_tab);
		while ((buf = (DbeStagingBuffer *) hash_seq_search(&status)) != NULL)
		{
			if (buf->owner_subxid == mySubid)
				buf->owner_subxid = parentSubid;
		}
	}

	foreach(lc, dbe_retained)
	{
		DbeStagingBuffer *r = (DbeStagingBuffer *) lfirst(lc);

		if (r->flush_subxid == mySubid)
			r->flush_subxid = parentSubid;
		if (r->owner_subxid == mySubid)
			r->owner_subxid = parentSubid;

		if (r->flush_subxid == r->owner_subxid)
		{
			dbe_staging_free_buffer(r);
			continue;
		}

		old = MemoryContextSwitchTo(dbe_staging_cxt);
		keep = lappend(keep, r);
		MemoryContextSwitchTo(old);
	}

	list_free(dbe_retained);
	dbe_retained = keep;

	foreach(lc, dbe_reservations)
	{
		DbeReservation *r = (DbeReservation *) lfirst(lc);

		if (r->subxid == mySubid)
			r->subxid = parentSubid;
	}
}

static void
dbe_staging_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			dbe_staging_flush_all();

			/*
			 * Al commitear la transacción de más afuera ya no hay
			 * subtransacción que pueda llevarse una fila de metadatos.
			 */
			{
				ListCell   *lc;

				foreach(lc, dbe_retained)
					dbe_staging_free_buffer((DbeStagingBuffer *) lfirst(lc));
				list_free(dbe_retained);
				dbe_retained = NIL;
			}

			dbe_drop_own_reservations();
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			dbe_staging_discard_all();
			/* Las filas de reserva mueren con la transacción. */
			dbe_reservations = NIL;
			break;

		default:
			break;
	}
}

static void
dbe_staging_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
							 SubTransactionId parentSubid, void *arg)
{
	/*
	 * Un stripe escrito por una subtransacción que aborta queda invisible
	 * solo: su insert_xid nunca commitea.  Acá hay que ocuparse de dos
	 * cosas distintas: lo que quedó sin flushear (se tira) y lo que se
	 * flusheó a nombre de esta subtransacción pero pertenece a una más
	 * externa (hay que reescribirlo).
	 */
	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			dbe_staging_discard_subxact(mySubid);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			dbe_staging_promote_subxact(mySubid, parentSubid);
			break;
		default:
			break;
	}
}
