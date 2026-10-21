/*-------------------------------------------------------------------------
 *
 * workload.c
 *	  Canal de feedback del workload.  Ver workload.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "access/relation.h"
#include "catalog/pg_am_d.h"
#include "utils/hsearch.h"
#include "utils/relfilenumbermap.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "dbengine/catalog.h"
#include "dbengine/dbengine.h"
#include "dbengine/workload.h"

PG_FUNCTION_INFO_V1(dbengine_workload_stats);
PG_FUNCTION_INFO_V1(dbengine_workload_reset);

typedef struct DbeWorkloadShared
{
	LWLock	   *lock;
	int			nentries;
	DbeWorkloadEntry entries[FLEXIBLE_ARRAY_MEMBER];
} DbeWorkloadShared;

static DbeWorkloadShared *dbe_workload = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static Size
dbe_workload_size(void)
{
	return MAXALIGN(offsetof(DbeWorkloadShared, entries) +
					sizeof(DbeWorkloadEntry) * DBE_WORKLOAD_ENTRIES);
}

void
dbe_workload_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(dbe_workload_size());
	RequestNamedLWLockTranche("dbengine_workload", 1);
}

void
dbe_workload_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	dbe_workload = (DbeWorkloadShared *)
		ShmemInitStruct("dbengine workload", dbe_workload_size(), &found);
	if (!found)
	{
		memset(dbe_workload, 0, dbe_workload_size());
		dbe_workload->lock = &(GetNamedLWLockTranche("dbengine_workload"))->lock;
		dbe_workload->nentries = 0;
	}
	LWLockRelease(AddinShmemInitLock);
}

void
dbe_workload_init(void)
{
	/*
	 * Sin la biblioteca precargada no hay memoria compartida que pedir: los
	 * hooks de shmem solo corren en el postmaster.  El canal queda apagado y
	 * dbe_workload_note() no hace nada.
	 */
	if (!dbe_preloaded)
		return;

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = dbe_workload_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = dbe_workload_shmem_startup;
}

typedef enum DbeWorkloadKind
{
	DBE_WL_RANGE,
	DBE_WL_EQ,
	DBE_WL_GROUP
} DbeWorkloadKind;

static void dbe_workload_note_kind(RelFileNumber relnumber, AttrNumber attnum,
								   DbeWorkloadKind kind);

void
dbe_workload_note(RelFileNumber relnumber, AttrNumber attnum, bool is_equality)
{
	dbe_workload_note_kind(relnumber, attnum,
						   is_equality ? DBE_WL_EQ : DBE_WL_RANGE);
}

/* Columna por la que se agrupa; ver el comentario de group_refs. */
void
dbe_workload_note_group(RelFileNumber relnumber, AttrNumber attnum)
{
	dbe_workload_note_kind(relnumber, attnum, DBE_WL_GROUP);
}

static void
dbe_workload_note_kind(RelFileNumber relnumber, AttrNumber attnum,
					   DbeWorkloadKind kind)
{
	DbeWorkloadKey key;
	int			i;
	int			free_slot = -1;

	if (dbe_workload == NULL || !RelFileNumberIsValid(relnumber) || attnum <= 0)
		return;

	key.dbid = MyDatabaseId;
	key.relnumber = relnumber;
	key.attnum = attnum;

	/*
	 * Búsqueda lineal.  Con 1024 entradas y una anotación por relación por
	 * planificación, no es el cuello de nada; una tabla de hash en memoria
	 * compartida con expulsión es bastante más código para el mismo efecto.
	 */
	LWLockAcquire(dbe_workload->lock, LW_EXCLUSIVE);

	for (i = 0; i < DBE_WORKLOAD_ENTRIES; i++)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[i];

		if (e->key.relnumber == key.relnumber && e->key.dbid == key.dbid &&
			e->key.attnum == key.attnum)
		{
			if (kind == DBE_WL_EQ)
				e->eq_preds++;
			else if (kind == DBE_WL_GROUP)
				e->group_refs++;
			else
				e->range_preds++;
			e->last_seen = GetCurrentTimestamp();
			LWLockRelease(dbe_workload->lock);
			return;
		}
		if (free_slot < 0 && !RelFileNumberIsValid(e->key.relnumber))
			free_slot = i;
	}

	/*
	 * Llena: se descarta la anotación.  Es estadística de workload — perder
	 * una no cambia ningún resultado, y expulsar a alguien pediría una
	 * política de reemplazo que todavía nadie necesita.
	 */
	if (free_slot >= 0)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[free_slot];

		e->key = key;
		e->range_preds = (kind == DBE_WL_RANGE) ? 1 : 0;
		e->eq_preds = (kind == DBE_WL_EQ) ? 1 : 0;
		e->group_refs = (kind == DBE_WL_GROUP) ? 1 : 0;
		e->last_seen = GetCurrentTimestamp();
		dbe_workload->nentries++;
	}

	LWLockRelease(dbe_workload->lock);
}

/*
 * Lleva las anotaciones de un relfilenode al que lo reemplaza.
 *
 * Sin esto el canal de feedback es inútil justo cuando hace falta.  Una
 * reescritura —CLUSTER, VACUUM FULL— escribe bajo un relfilenode NUEVO, y
 * es el único momento en que se construyen los filtros de Bloom.  Al
 * preguntar por el relfilenode nuevo, que el canal nunca vio, la respuesta
 * era que no para todas las columnas: el Bloom adaptativo no se activaba
 * nunca, y solo funcionaba forzándolo con dbengine.bloom_bits_per_row.
 *
 * Se copian, no se mueven: la reescritura puede abortar y dejar la tabla en
 * el relfilenode viejo.  Las entradas huérfanas se reciclan solas cuando la
 * tabla se llena, que es lo mismo que ya pasaba con las de tablas borradas.
 */
void
dbe_workload_carry(RelFileNumber from, RelFileNumber to)
{
	int			i;
	int			n = 0;
	DbeWorkloadEntry copia[DBE_WORKLOAD_ENTRIES];

	if (dbe_workload == NULL || !RelFileNumberIsValid(from) ||
		!RelFileNumberIsValid(to) || from == to)
		return;

	LWLockAcquire(dbe_workload->lock, LW_EXCLUSIVE);

	for (i = 0; i < DBE_WORKLOAD_ENTRIES; i++)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[i];

		if (e->key.relnumber == from && e->key.dbid == MyDatabaseId)
			copia[n++] = *e;
	}

	for (i = 0; i < DBE_WORKLOAD_ENTRIES && n > 0; i++)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[i];

		if (RelFileNumberIsValid(e->key.relnumber))
		{
			/* Si ya existe la entrada destino, se pisa con la que viene. */
			int			j;

			for (j = 0; j < n; j++)
				if (e->key.relnumber == to && e->key.dbid == MyDatabaseId &&
					e->key.attnum == copia[j].key.attnum)
				{
					*e = copia[j];
					e->key.relnumber = to;
					copia[j] = copia[--n];
					break;
				}
			continue;
		}

		*e = copia[--n];
		e->key.relnumber = to;
	}

	LWLockRelease(dbe_workload->lock);
}

bool
dbe_workload_wants_bloom(RelFileNumber relnumber, AttrNumber attnum)
{
	int			i;
	bool		want = false;

	if (dbe_workload == NULL || !RelFileNumberIsValid(relnumber) || attnum <= 0)
		return false;

	LWLockAcquire(dbe_workload->lock, LW_SHARED);
	for (i = 0; i < DBE_WORKLOAD_ENTRIES; i++)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[i];

		if (e->key.relnumber == relnumber && e->key.dbid == MyDatabaseId &&
			e->key.attnum == attnum)
		{
			want = (e->eq_preds > 0);
			break;
		}
	}
	LWLockRelease(dbe_workload->lock);
	return want;
}

int
dbe_workload_snapshot(DbeWorkloadEntry **out)
{
	DbeWorkloadEntry *copy;
	int			n = 0;
	int			i;

	*out = NULL;
	if (dbe_workload == NULL)
		return 0;

	copy = (DbeWorkloadEntry *)
		palloc(sizeof(DbeWorkloadEntry) * DBE_WORKLOAD_ENTRIES);

	LWLockAcquire(dbe_workload->lock, LW_SHARED);
	for (i = 0; i < DBE_WORKLOAD_ENTRIES; i++)
	{
		DbeWorkloadEntry *e = &dbe_workload->entries[i];

		if (RelFileNumberIsValid(e->key.relnumber))
			copy[n++] = *e;
	}
	LWLockRelease(dbe_workload->lock);

	*out = copy;
	return n;
}

void
dbe_workload_reset(void)
{
	if (dbe_workload == NULL)
		return;

	LWLockAcquire(dbe_workload->lock, LW_EXCLUSIVE);
	memset(dbe_workload->entries, 0,
		   sizeof(DbeWorkloadEntry) * DBE_WORKLOAD_ENTRIES);
	dbe_workload->nentries = 0;
	LWLockRelease(dbe_workload->lock);
}

/* ------------------------------------------------------------------------
 * Superficie SQL
 * ------------------------------------------------------------------------ */

Datum
dbengine_workload_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DbeWorkloadEntry *entries;
	int			n;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	n = dbe_workload_snapshot(&entries);

	for (i = 0; i < n; i++)
	{
		Datum		values[7];
		bool		nulls[7] = {false, false, false, false, false, false, false};

		values[0] = ObjectIdGetDatum(entries[i].key.dbid);
		values[1] = Int64GetDatum((int64) entries[i].key.relnumber);
		values[2] = Int16GetDatum(entries[i].key.attnum);
		values[3] = Int64GetDatum((int64) entries[i].eq_preds);
		values[4] = Int64GetDatum((int64) entries[i].range_preds);
		values[5] = Int64GetDatum((int64) entries[i].group_refs);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	if (entries != NULL)
		pfree(entries);

	return (Datum) 0;
}

Datum
dbengine_workload_reset(PG_FUNCTION_ARGS)
{
	dbe_workload_reset();
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------
 * Consejo de clustering (F6)
 *
 * La decisión de por cuál columna reordenar sale del workload, no de una
 * heurística sobre los datos.  Se pesa la igualdad por encima del rango:
 * agrupar valores iguales aprieta los rangos [min,max] de cada stripe más
 * que ordenar por una columna que se consulta por tramos anchos.
 *
 * Es un CONSEJO, no una acción.  Reescribir una tabla toma ACCESS
 * EXCLUSIVE, y eso no lo decide una estadística: lo decide quien opera la
 * base.  Por eso la función devuelve qué convendría y con qué índice, y el
 * CLUSTER lo corre alguien más.
 * ------------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(dbengine_cluster_advice);

Datum
dbengine_cluster_advice(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DbeWorkloadEntry *entries;
	int			n;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	n = dbe_workload_snapshot(&entries);

	for (i = 0; i < n; i++)
	{
		Datum		values[7];
		bool		nulls[7] = {false, false, false, false, false,
							   false, false};
		Oid			relid;
		Relation	rel;
		int64		score;
		char	   *colname = NULL;
		Oid			indexoid = InvalidOid;
		Oid			ordered_by = InvalidOid;

		if (entries[i].key.dbid != MyDatabaseId)
			continue;

		/*
		 * El canal guarda relfilenode, no OID: el storage vive en el
		 * relfilenode y una reescritura lo cambia.  Para contestar en
		 * términos de tablas hay que traducir.
		 */
		relid = RelidByRelfilenumber(InvalidOid, entries[i].key.relnumber);
		if (!OidIsValid(relid))
			continue;

		rel = try_relation_open(relid, AccessShareLock);
		if (rel == NULL)
			continue;

		if (!dbe_is_columnar_relation(rel) ||
			entries[i].key.attnum > RelationGetDescr(rel)->natts)
		{
			relation_close(rel, AccessShareLock);
			continue;
		}

		colname = NameStr(TupleDescAttr(RelationGetDescr(rel),
										entries[i].key.attnum - 1)->attname);
		ordered_by = dbe_catalog_get_order(rel->rd_locator.relNumber);

		/*
		 * Peso 3 a la igualdad y 1 al rango.  No es una constante ajustada:
		 * es el orden de magnitud correcto — agrupar iguales aprieta los
		 * zone maps mucho más que ordenar por algo que se pide por tramos.
		 */
		/*
		 * Agrupar pesa como la igualdad: las dos quieren que los valores
		 * iguales queden juntos, y desde que la agregación se puede empujar
		 * al scan, agrupar es lo que más gana con el orden físico.
		 */
		score = (int64) entries[i].eq_preds * 3 +
			(int64) entries[i].group_refs * 3 +
			(int64) entries[i].range_preds;

		/* Un CLUSTER necesita un btree sobre esa columna. */
		{
			List	   *indexes = RelationGetIndexList(rel);
			ListCell   *lc;

			foreach(lc, indexes)
			{
				Relation	irel = try_relation_open(lfirst_oid(lc),
													 AccessShareLock);

				if (irel == NULL)
					continue;
				if (irel->rd_rel->relam == BTREE_AM_OID &&
					irel->rd_index->indnatts == 1 &&
					irel->rd_index->indkey.values[0] == entries[i].key.attnum)
					indexoid = RelationGetRelid(irel);
				relation_close(irel, AccessShareLock);
				if (OidIsValid(indexoid))
					break;
			}
			list_free(indexes);
		}

		values[0] = ObjectIdGetDatum(relid);
		values[1] = Int16GetDatum(entries[i].key.attnum);
		values[2] = CStringGetTextDatum(colname);
		values[3] = Int64GetDatum(score);
		if (OidIsValid(indexoid))
			values[4] = ObjectIdGetDatum(indexoid);
		else
			nulls[4] = true;
		values[5] = BoolGetDatum(OidIsValid(indexoid));

		/*
		 * ¿Es esta la columna por la que la tabla YA está ordenada?
		 *
		 * Sin este dato el consejo solo muestra lo que se gana y nunca lo
		 * que se pierde.  Medido sobre ClickBench: reordenar por UserID da
		 * 455x en el GROUP BY con LIMIT y 39x en el lookup, y al mismo
		 * tiempo hunde a 0,17x la query que filtra por rango de EventDate,
		 * que venía gratis porque la tabla estaba ordenada por tiempo desde
		 * la inserción.  Reordenar es una redistribución, no una mejora, y
		 * quien decide necesita ver las dos mitades.
		 */
		values[6] = BoolGetDatum(OidIsValid(indexoid) && indexoid == ordered_by);

		relation_close(rel, AccessShareLock);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	if (entries != NULL)
		pfree(entries);

	return (Datum) 0;
}
