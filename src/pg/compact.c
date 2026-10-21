/*-------------------------------------------------------------------------
 *
 * compact.c
 *	  Fusión de stripes (F6).
 *
 * El buffer de escritura cierra un stripe por transacción.  Con autocommit
 * fila a fila eso deja un stripe por fila: 2000 INSERT sueltos daban 2000
 * stripes y 16 MB para lo que en un solo INSERT ocupa 216 kB.  El problema
 * no es el espacio sino el scan, que tiene que abrir 2000 stripes.
 *
 * La salida es fusionar stripes adyacentes.  Lo que la hace barata es una
 * propiedad del layout: un stripe es dueño de un rango CONTIGUO de números
 * de fila, así que fusionar stripes adyacentes **conservando el orden**
 * conserva el número de fila de cada una.  Y el número de fila es el TID.
 *
 *	 stripe A: filas 0..999      stripe B: filas 1000..1999
 *	 fusionado: filas 0..1999, cada una donde estaba
 *
 * O sea que fusionar no mueve ningún TID y por lo tanto **no toca ningún
 * índice**.  Es la diferencia con reordenar (CLUSTER), que los mueve todos
 * y obliga a reconstruirlos.
 *
 * Solo se fusionan stripes que ya son visibles para todos.  El resultado se
 * escribe con FrozenTransactionId, que es visible para cualquier snapshot
 * presente o futuro — y de paso lo saca de depender del clog.
 *
 * Lo que NO hace: recuperar el espacio de los stripes viejos.  El archivo
 * lógico es append-only, así que el stripe nuevo se agrega al final y los
 * bytes viejos quedan como hueco.  Se reclaman con VACUUM FULL, que desde
 * F6 funciona.  Fusionar arregla el costo del scan ya mismo; el espacio se
 * recupera cuando se pide.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"

PG_FUNCTION_INFO_V1(dbengine_merge_stripes);

/*
 * ¿Este stripe se puede fusionar?
 *
 * El criterio es el TAMAÑO, no si ya se fusionó antes.  Con el presupuesto
 * en 64, dos mil stripes de una fila se fusionan de a 64 y quedan 32: para
 * llegar a uno hace falta otra pasada sobre stripes que ya están
 * congelados.  Saltearlos por estar congelados dejaba la fusión trabada a
 * mitad de camino.
 *
 * El umbral es medio stripe lleno: por encima de eso fusionar casi no baja
 * la cantidad y en cambio vuelve a copiar los datos.  Como cada pasada
 * divide la cantidad de stripes por el presupuesto, converge en
 * log_presupuesto(N) pasadas y el espacio que deja atrás es una fracción
 * chica del original.
 */
static bool
dbe_stripe_mergeable(const DbeStripeDesc *s, uint64 max_rows,
					 TimestampTz horizonte)
{
	TransactionId xid = XidFromFullTransactionId(s->insert_xid);
	TimestampTz ts;

	if (s->row_count == 0 || s->row_count > max_rows / 2)
		return false;

	/*
	 * Ventana de retención (F7).  Fusionar coarsa el time travel: el stripe
	 * resultante se queda con el momento del más nuevo, así que viajar a un
	 * instante intermedio dejaría de ver filas que en ese momento existían.
	 * Por eso solo se fusiona lo que ya salió de la ventana.
	 *
	 * Sin track_commit_timestamp no se puede saber de cuándo es un stripe;
	 * ahí se fusiona igual, porque tampoco había time travel que preservar.
	 */
	if (dbe_stripe_commit_ts(s, &ts) && ts > horizonte)
		return false;
	if (!TransactionIdIsValid(xid))
		return false;
	if (TransactionIdIsCurrentTransactionId(xid))
		return false;			/* recién escrito por nosotros */

	/*
	 * Visible para todos: terminó antes de que cualquier transacción viva
	 * tomara su snapshot.  Solo así el resultado puede ir congelado sin
	 * hacerle ver a un lector viejo filas que no debería.
	 */
	if (!TransactionIdPrecedes(xid, GetOldestNonRemovableTransactionId(NULL)))
		return false;

	return TransactionIdDidCommit(xid);
}

/*
 * Fusiona los stripes [from, to) del cursor en uno solo.
 */
static void
dbe_merge_run(Relation rel, DbeRowCursor *cur, TupleTableSlot *slot,
			  int from, int to)
{
	DbeStripeWriter *w;
	uint64	   *ids;
	int			n = to - from;
	int			i;
	TimestampTz commit_ts = 0;
	bool		tiene_ts = false;

	ids = (uint64 *) palloc(sizeof(uint64) * n);
	for (i = 0; i < n; i++)
		ids[i] = dbe_cursor_stripe_at(cur, from + i)->stripe_id;

	/*
	 * El stripe fusionado se queda con el momento del MÁS NUEVO.  Es la
	 * elección conservadora para el time travel: viajar a un instante
	 * anterior no ve las filas, que es "todavía no existían" — falso para
	 * algunas, pero del lado seguro.  La alternativa, quedarse con el más
	 * viejo, mostraría filas que en ese momento no existían.
	 */
	for (i = 0; i < n; i++)
	{
		const DbeStripeDesc *s = dbe_cursor_stripe_at(cur, from + i);
		TimestampTz ts;

		if (dbe_stripe_commit_ts(s, &ts) && (!tiene_ts || ts > commit_ts))
		{
			commit_ts = ts;
			tiene_ts = true;
		}
	}

	w = dbe_stripe_writer_begin(rel,
								dbe_cursor_stripe_at(cur, from)->first_row_number);
	if (tiene_ts)
		dbe_stripe_writer_set_commit_ts(w, commit_ts);

	PG_TRY();
	{
		for (i = from; i < to; i++)
		{
			if (!dbe_cursor_set_stripe(cur, i))
				continue;
			while (dbe_cursor_next_in_stripe(cur, slot))
			{
				CHECK_FOR_INTERRUPTS();
				dbe_stripe_writer_add(w, slot);
			}
		}

		/*
		 * Los metadatos viejos se borran ANTES de meter los nuevos: los dos
		 * cubren el mismo rango de números de fila, y tenerlos a la vez —
		 * aunque sea solo dentro de esta transacción — sería ver cada fila
		 * dos veces.  Los datos viejos siguen en el archivo, así que un
		 * lector que ya armó su lista de stripes sigue andando.
		 */
		for (i = 0; i < n; i++)
			dbe_catalog_delete_stripe(rel->rd_locator.relNumber, ids[i]);

		(void) dbe_stripe_writer_finish(w);
	}
	PG_CATCH();
	{
		dbe_stripe_writer_end(w);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(ids);
}

/*
 * dbengine.merge_stripes(tabla) -> cuántos stripes se fusionaron.
 */
Datum
dbengine_merge_stripes(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	DbeRowCursor *cur;
	TupleTableSlot *slot;
	int			nstripes;
	int			merged = 0;
	int			budget;
	uint64		max_rows;
	TimestampTz horizonte;
	int			i;
	List	   *runs = NIL;
	ListCell   *lc;

	/*
	 * ShareUpdateExclusive: dos fusiones a la vez sobre la misma tabla se
	 * pisarían, pero las lecturas y las escrituras siguen andando.  No hace
	 * falta más: los TID no se mueven.
	 */
	/*
	 * Fusionar reescribe el storage de la tabla y coarsa su time travel.
	 * table_open() no chequea permisos, así que hay que hacerlo acá: es
	 * mantenimiento, y el permiso de mantenimiento es MAINTAIN o ser dueño.
	 */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK &&
		!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));

	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!dbe_is_columnar_relation(rel))
	{
		table_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("dbengine: \"%s\" no es una tabla columnar",
						RelationGetRelationName(rel))));
	}

	/* Lo que esté pendiente en el buffer tiene que estar en el storage. */
	dbe_staging_flush_relation(relid);

	budget = dbe_max_blocks_per_compaction;
	max_rows = (uint64) dbe_block_rows;
	horizonte = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
											-1000L * dbe_retention_window);

	cur = dbe_cursor_begin(rel, GetActiveSnapshot(), NULL);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	nstripes = dbe_cursor_nstripes(cur);

	/*
	 * Se buscan corridas de stripes adyacentes: contiguos en números de
	 * fila, fusionables, y que juntos no pasen de un stripe lleno.  La
	 * contigüidad no es un detalle — si entre dos stripes quedó un hueco de
	 * números de fila reservados y no usados, fusionarlos correría los TID
	 * de todas las filas del segundo.
	 */
	i = 0;
	while (i < nstripes)
	{
		const DbeStripeDesc *first = dbe_cursor_stripe_at(cur, i);
		uint64		rows;
		int			j;

		if (!dbe_stripe_mergeable(first, max_rows, horizonte))
		{
			i++;
			continue;
		}

		rows = first->row_count;
		for (j = i + 1; j < nstripes && (j - i) < budget; j++)
		{
			const DbeStripeDesc *prev = dbe_cursor_stripe_at(cur, j - 1);
			const DbeStripeDesc *s = dbe_cursor_stripe_at(cur, j);

			if (prev->first_row_number + prev->row_count != s->first_row_number)
				break;			/* hueco: fusionar movería TIDs */
			if (!dbe_stripe_mergeable(s, max_rows, horizonte))
				break;
			if (rows + s->row_count > max_rows)
				break;
			rows += s->row_count;
		}

		if (j - i >= 2)
		{
			runs = lappend(runs, list_make2_int(i, j));
			merged += (j - i);
		}
		i = j > i ? j : i + 1;
	}

	foreach(lc, runs)
	{
		List	   *r = (List *) lfirst(lc);

		dbe_merge_run(rel, cur, slot, linitial_int(r), lsecond_int(r));
	}

	dbe_cursor_end(cur);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, ShareUpdateExclusiveLock);

	PG_RETURN_INT64((int64) merged);
}
