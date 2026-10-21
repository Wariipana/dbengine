/*-------------------------------------------------------------------------
 *
 * errors.c
 *	  Conversión DbeStatus -> ereport.  Este es el ÚNICO lugar donde un
 *	  error que nació en C++ se convierte en un error de Postgres, y ocurre
 *	  siempre en un frame de C con todos los frames de C++ ya desenrollados.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "dbengine/dbengine.h"

const char *
dbe_code_name(int32 code)
{
	switch ((DbeCode) code)
	{
		case DBE_OK:
			return "OK";
		case DBE_ERR_EXCEPTION:
			return "EXCEPTION";
		case DBE_ERR_BAD_ALLOC:
			return "BAD_ALLOC";
		case DBE_ERR_UNKNOWN_EXCEPTION:
			return "UNKNOWN_EXCEPTION";
		case DBE_ERR_PG:
			return "PG";
		case DBE_ERR_INVALID_ARGUMENT:
			return "INVALID_ARGUMENT";
		case DBE_ERR_NOT_IMPLEMENTED:
			return "NOT_IMPLEMENTED";
		case DBE_ERR_INTERNAL:
			return "INTERNAL";
	}
	return "UNRECOGNIZED";
}

static int
dbe_sqlerrcode_for(const DbeStatus *st)
{
	switch ((DbeCode) st->code)
	{
		case DBE_ERR_BAD_ALLOC:
			return ERRCODE_OUT_OF_MEMORY;
		case DBE_ERR_INVALID_ARGUMENT:
			return ERRCODE_INVALID_PARAMETER_VALUE;
		case DBE_ERR_NOT_IMPLEMENTED:
			return ERRCODE_FEATURE_NOT_SUPPORTED;
		case DBE_ERR_PG:
			/* Preservamos el SQLSTATE original del error de Postgres. */
			return st->pg_sqlerrcode != 0 ? st->pg_sqlerrcode : ERRCODE_INTERNAL_ERROR;
		default:
			return ERRCODE_INTERNAL_ERROR;
	}
}

void
dbe_error_if_not_ok(const DbeStatus *st, const char *context)
{
	if (st == NULL || st->code == DBE_OK)
		return;

	if (context == NULL)
		context = "motor dbengine";

	ereport(ERROR,
			(errcode(dbe_sqlerrcode_for(st)),
			 errmsg("dbengine: %s",
					st->message[0] != '\0' ? st->message : dbe_code_name(st->code)),
			 errdetail_internal("dbe_code=%s, contexto=%s",
								dbe_code_name(st->code), context)));
}
