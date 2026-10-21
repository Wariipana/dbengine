/*-------------------------------------------------------------------------
 *
 * timetravel.c
 *	  Time travel (F7).
 *
 * El plan pedía un "version store como estructura lateral del Table AM".
 * En un storage append-only **no hace falta: el version store ya está**.
 * Nada se borra ni se pisa, así que el estado de la tabla en un momento T
 * es exactamente el conjunto de stripes cuya transacción commiteó antes de
 * T.  La estructura lateral que haría falta si hubiera UPDATE y DELETE acá
 * sería una copia de algo que ya existe.
 *
 * El momento del commit lo lleva Postgres en pg_commit_ts, que hay que
 * encender con track_commit_timestamp.  Guardarlo nosotros por stripe sería
 * duplicarlo; solo se guarda cuando la fusión hace perder el xid original.
 *
 * La granularidad es el stripe, no la fila.  Es la correcta: un stripe es
 * un lote de una transacción, y una transacción commitea entera.
 *
 * Lo que NO se puede: viajar más atrás de lo que pg_commit_ts conserva, ni
 * ver dentro de una transacción.  Y la fusión de stripes coarsa la
 * granularidad —el resultado se queda con el momento del más nuevo—, así
 * que solo fusiona lo que ya salió de la ventana de retención.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"

PG_FUNCTION_INFO_V1(dbengine_time_travel);
PG_FUNCTION_INFO_V1(dbengine_stripe_timeline);

/*
 * dbengine.time_travel(tabla, momento) -> las filas que había en ese momento.
 *
 * Devuelve `setof record`, así que quien la llama tiene que decir las
 * columnas:
 *
 *	 SELECT * FROM dbengine.time_travel('t', now() - '1 hour'::interval)
 *		 AS (a int, b text);
 */
Datum
dbengine_time_travel(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz asof = PG_GETARG_TIMESTAMPTZ(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	DbeRowCursor *cur;
	TupleTableSlot *slot;
	TupleDesc	reldesc;
	int			nstripes;
	int			i;

	/*
	 * Devuelve datos de la tabla, así que hace falta poder leerla.
	 * table_open() no chequea permisos.
	 */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));

	rel = table_open(relid, AccessShareLock);

	if (!dbe_is_columnar_relation(rel))
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("dbengine: \"%s\" no es una tabla columnar",
						RelationGetRelationName(rel))));
	}

	reldesc = RelationGetDescr(rel);
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	if (rsinfo->setDesc->natts != reldesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("dbengine: la lista de columnas tiene %d, la tabla %d",
						rsinfo->setDesc->natts, reldesc->natts),
				 errhint("La lista de columnas de la llamada tiene que "
						 "coincidir con la de la tabla.")));

	cur = dbe_cursor_begin(rel, GetActiveSnapshot(), NULL);
	slot = MakeSingleTupleTableSlot(reldesc, &TTSOpsVirtual);
	nstripes = dbe_cursor_nstripes(cur);

	for (i = 0; i < nstripes; i++)
	{
		const DbeStripeDesc *s = dbe_cursor_stripe_at(cur, i);
		TimestampTz ts;

		CHECK_FOR_INTERRUPTS();

		if (!dbe_stripe_commit_ts(s, &ts))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("dbengine: no se sabe cuándo commiteó el stripe %llu",
							(unsigned long long) s->stripe_id),
					 errdetail("Sin ese dato el resultado sería una mezcla de "
							   "momentos, que es peor que no contestar."),
					 errhint("Encendé track_commit_timestamp. Los stripes "
							 "escritos antes de encenderlo no se pueden "
							 "ubicar en el tiempo.")));

		if (ts > asof)
			continue;			/* commiteó después: no existía todavía */

		if (!dbe_cursor_set_stripe(cur, i))
			continue;
		while (dbe_cursor_next_in_stripe(cur, slot))
		{
			CHECK_FOR_INTERRUPTS();
			slot_getallattrs(slot);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 slot->tts_values, slot->tts_isnull);
		}
	}

	dbe_cursor_end(cur);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * dbengine.stripe_timeline(tabla): qué stripes hay y de cuándo son.
 *
 * Es lo que hace explicable un time travel: hasta dónde se puede viajar y
 * con qué granularidad.
 */
Datum
dbengine_stripe_timeline(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	DbeRowCursor *cur;
	int			nstripes;
	int			i;

	/*
	 * Devuelve datos de la tabla, así que hace falta poder leerla.
	 * table_open() no chequea permisos.
	 */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));

	rel = table_open(relid, AccessShareLock);

	if (!dbe_is_columnar_relation(rel))
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("dbengine: \"%s\" no es una tabla columnar",
						RelationGetRelationName(rel))));
	}

	InitMaterializedSRF(fcinfo, 0);

	cur = dbe_cursor_begin(rel, GetActiveSnapshot(), NULL);
	nstripes = dbe_cursor_nstripes(cur);

	for (i = 0; i < nstripes; i++)
	{
		const DbeStripeDesc *s = dbe_cursor_stripe_at(cur, i);
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};
		TimestampTz ts;

		values[0] = Int64GetDatum((int64) s->stripe_id);
		values[1] = Int64GetDatum((int64) s->first_row_number);
		values[2] = Int64GetDatum((int64) s->row_count);
		values[3] = Int64GetDatum((int64) XidFromFullTransactionId(s->insert_xid));

		if (dbe_stripe_commit_ts(s, &ts))
			values[4] = TimestampTzGetDatum(ts);
		else
			nulls[4] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	dbe_cursor_end(cur);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}
