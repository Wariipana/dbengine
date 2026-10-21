/*-------------------------------------------------------------------------
 *
 * funcs.c
 *	  Funciones SQL de la extensión.
 *
 * En F0: identificación, estado de carga y la batería de autotest de la
 * frontera C/C++ que exige R2.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/tuplestore.h"

#include "dbengine/dbengine.h"
#include "dbengine/bridge.h"

PG_FUNCTION_INFO_V1(dbengine_version);
PG_FUNCTION_INFO_V1(dbengine_preloaded);
PG_FUNCTION_INFO_V1(dbengine_bridge_selftest);
PG_FUNCTION_INFO_V1(dbengine_raise_from_engine);

Datum
dbengine_version(PG_FUNCTION_ARGS)
{
	char		buf[64];
	DbeStatus	st;

	st = dbe_engine_version(buf, sizeof(buf));
	dbe_error_if_not_ok(&st, "consultando la versión del motor");

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

Datum
dbengine_preloaded(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(dbe_preloaded);
}

/*
 * Deja que el error del motor se propague hasta SQL.  Sirve para verificar
 * a nivel de query que la conversión DbeStatus -> ereport produce un error
 * normal de Postgres, con SQLSTATE correcto y sin romper la sesión.
 */
Datum
dbengine_raise_from_engine(PG_FUNCTION_ARGS)
{
	int32		kind = PG_GETARG_INT32(0);
	int32		dtors = 0;
	DbeStatus	st;

	st = dbe_selftest_throw(kind, &dtors);
	dbe_error_if_not_ok(&st, "dbengine_raise_from_engine");

	PG_RETURN_INT32(dtors);
}

/* --- Batería de autotest de la frontera --------------------------------- */

typedef struct SelftestSink
{
	ReturnSetInfo *rsinfo;
	int			failures;
} SelftestSink;

static void
emit(SelftestSink *sink, const char *name, bool passed, const char *fmt,...)
	pg_attribute_printf(4, 5);

static void
emit(SelftestSink *sink, const char *name, bool passed, const char *fmt,...)
{
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	StringInfoData detail;
	va_list		ap;

	initStringInfo(&detail);
	va_start(ap, fmt);
	appendStringInfoVA(&detail, fmt, ap);
	va_end(ap);

	values[0] = CStringGetTextDatum(name);
	values[1] = BoolGetDatum(passed);
	values[2] = CStringGetTextDatum(detail.data);

	tuplestore_putvalues(sink->rsinfo->setResult, sink->rsinfo->setDesc,
						 values, nulls);

	if (!passed)
		sink->failures++;

	pfree(detail.data);
}

/*
 * Cada caso comprueba dos cosas a la vez:
 *   - el código de error que llegó del otro lado del airlock;
 *   - cuántos destructores de C++ corrieron.
 *
 * El contador de destructores es la parte que importa: si un longjmp de
 * Postgres hubiera atravesado los frames de C++, o si una excepción hubiera
 * escapado sin unwind, ese número no daría.
 */
static void
check_throw(SelftestSink *sink, const char *name, int32 kind,
			int32 expect_code, int32 expect_dtors)
{
	int32		dtors = -1;
	DbeStatus	st = dbe_selftest_throw(kind, &dtors);
	bool		ok = (st.code == expect_code && dtors == expect_dtors);

	emit(sink, name, ok,
		 "code=%s (esperado %s), destructores=%d (esperado %d), msg=\"%s\"",
		 dbe_code_name(st.code), dbe_code_name(expect_code),
		 dtors, expect_dtors, st.message);
}

Datum
dbengine_bridge_selftest(PG_FUNCTION_ARGS)
{
	SelftestSink sink;
	int32		dtors;
	DbeStatus	st;
	char		vbuf[64];

	InitMaterializedSRF(fcinfo, 0);
	sink.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	sink.failures = 0;

	/* --- C -> C++: ninguna excepción puede volver a un frame de C ------ */

	check_throw(&sink, "cxx/sin_excepcion", DBE_THROW_NONE, DBE_OK, 0);
	check_throw(&sink, "cxx/runtime_error", DBE_THROW_RUNTIME_ERROR,
				DBE_ERR_EXCEPTION, 1);
	check_throw(&sink, "cxx/bad_alloc", DBE_THROW_BAD_ALLOC,
				DBE_ERR_BAD_ALLOC, 1);
	check_throw(&sink, "cxx/no_estandar", DBE_THROW_INT,
				DBE_ERR_UNKNOWN_EXCEPTION, 1);
	check_throw(&sink, "cxx/anidada_3_frames", DBE_THROW_NESTED,
				DBE_ERR_EXCEPTION, 3);

	/* --- C++ -> C: ningún longjmp puede atravesar un frame de C++ ------ */

	dtors = -1;
	st = dbe_selftest_pg_error(&dtors);
	emit(&sink, "pg/ereport_no_hace_longjmp",
		 st.code == DBE_ERR_PG && dtors == 1 &&
		 st.pg_sqlerrcode == ERRCODE_RAISE_EXCEPTION,
		 "code=%s (esperado PG), destructores=%d (esperado 1), sqlstate=%s",
		 dbe_code_name(st.code), dtors,
		 unpack_sql_state(st.pg_sqlerrcode));

	dtors = -1;
	st = dbe_selftest_roundtrip(&dtors);
	emit(&sink, "pg/ida_y_vuelta_sin_error",
		 st.code == DBE_OK && dtors == 1,
		 "code=%s (esperado OK), destructores=%d (esperado 1)",
		 dbe_code_name(st.code), dtors);

	/* --- La sesión sigue viva y usable después de todo lo anterior ----- */

	st = dbe_engine_version(vbuf, sizeof(vbuf));
	emit(&sink, "motor/sigue_respondiendo",
		 st.code == DBE_OK && strcmp(vbuf, DBENGINE_VERSION) == 0,
		 "version=\"%s\" (esperado \"%s\")", vbuf, DBENGINE_VERSION);

	return (Datum) 0;
}
