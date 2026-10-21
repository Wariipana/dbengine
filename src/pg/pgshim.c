/*-------------------------------------------------------------------------
 *
 * pgshim.c
 *	  Airlock C++ -> C.  C PURO, a propósito.
 *
 * Este archivo es el único que contiene PG_TRY/PG_CATCH en todo el camino
 * que el motor puede recorrer.  Como se compila con el compilador de C, el
 * setjmp/longjmp nunca puede saltar por encima de un destructor de C++:
 * los frames de C++ quedan siempre por FUERA de este archivo, y el longjmp
 * se detiene acá adentro.
 *
 * Regla de mantenimiento: si alguna vez este archivo se compila como C++,
 * la invariante se rompe.  El Makefile lo pone explícitamente en la lista
 * de fuentes C.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>

#include "utils/elog.h"
#include "utils/memutils.h"

#include "dbengine/pgshim.h"

/* Verificación en tiempo de compilación de la invariante de arriba. */
#ifdef __cplusplus
#error "pgshim.c debe compilarse como C, no como C++ (ver R2)"
#endif

void
dbe_shim_log(const char *msg)
{
	/*
	 * elevel LOG no hace longjmp, así que esta llamada es segura aun si el
	 * caller de C++ tiene objetos vivos en su frame.
	 */
	ereport(LOG, (errmsg("dbengine: %s", msg != NULL ? msg : "(null)")));
}

void *
dbe_shim_alloc(size_t sz)
{
	/*
	 * malloc y no palloc, deliberadamente:
	 *
	 * 1. palloc hace ereport(ERROR) al fallar -> longjmp desde adentro de
	 *    un frame de C++.  Inaceptable.
	 * 2. La memoria del motor no sigue el ciclo de vida de un MemoryContext
	 *    de query; las estructuras persistentes (zone maps, PGM, staging)
	 *    sobreviven a la query que las tocó.
	 *
	 * La capa C sigue usando palloc para todo lo suyo, como pide el plan.
	 */
	return malloc(sz);
}

void
dbe_shim_free(void *p)
{
	free(p);
}

/*
 * Captura un error de Postgres y lo devuelve como DbeStatus.
 *
 * OJO (ver el contrato en pgshim.h): FlushErrorState() limpia el estado de
 * error pero NO libera buffers, locks ni resource owners.  El caller de
 * C++ está obligado a desenrollar hasta el airlock sin volver a llamar a
 * ningún shim; la capa C re-lanza y ahí el abort de transacción limpia.
 */
static DbeStatus
dbe_shim_capture_error(void)
{
	ErrorData  *edata;
	DbeStatus	st;
	MemoryContext ecxt;

	/* CopyErrorData exige no estar en ErrorContext. */
	ecxt = MemoryContextSwitchTo(TopMemoryContext);
	edata = CopyErrorData();

	st.code = DBE_ERR_PG;
	st.pg_sqlerrcode = edata->sqlerrcode;
	dbe_status_set_message(&st, edata->message);

	FreeErrorData(edata);
	FlushErrorState();
	MemoryContextSwitchTo(ecxt);

	return st;
}

DbeStatus
dbe_shim_selftest_raise(const char *msg)
{
	DbeStatus	st = dbe_status_ok();

	PG_TRY();
	{
		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("dbengine selftest: %s", msg != NULL ? msg : "(null)")));
	}
	PG_CATCH();
	{
		st = dbe_shim_capture_error();
	}
	PG_END_TRY();

	return st;
}

DbeStatus
dbe_shim_selftest_noop(void)
{
	DbeStatus	st = dbe_status_ok();

	PG_TRY();
	{
		/* Trabajo trivial que sí toca APIs de Postgres. */
		(void) MemoryContextIsValid(CurrentMemoryContext);
	}
	PG_CATCH();
	{
		st = dbe_shim_capture_error();
	}
	PG_END_TRY();

	return st;
}
