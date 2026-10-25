//===----------------------------------------------------------------------===//
//
// engine.cpp
//   Ciclo de vida del motor y batería de autotest de la frontera.
//
// Este archivo NO incluye postgres.h.  Cualquier servicio de Postgres que
// necesite pasa por dbengine/pgshim.h.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/engine.hpp"
#include "dbengine/pgshim.h"
#include "dbengine/bridge.h"	/* solo por el enum DbeThrowKind; no arrastra Postgres */

#include <new>
#include <string>

namespace dbe {

namespace {
bool		g_started = false;
}

void
startup()
{
	if (g_started)
		return;
	g_started = true;
}

void
shutdown()
{
	g_started = false;
}

const char *
version() noexcept
{
	return "0.2.0";
}

namespace selftest {

namespace {

/// Tres frames anidados, cada uno con un objeto con destructor.  Si la
/// excepción se propaga bien, el contador queda en 3.
void
nested_level3(int32_t *dtors_run)
{
	DtorProbe	probe(dtors_run);

	throw Error(DBE_ERR_EXCEPTION, "excepción desde el frame más profundo");
}

void
nested_level2(int32_t *dtors_run)
{
	DtorProbe	probe(dtors_run);

	nested_level3(dtors_run);
}

void
nested_level1(int32_t *dtors_run)
{
	DtorProbe	probe(dtors_run);

	nested_level2(dtors_run);
}

}	// namespace

void
throw_kind(int32_t kind, int32_t *dtors_run)
{
	if (dtors_run != nullptr)
		*dtors_run = 0;

	switch (kind)
	{
		case DBE_THROW_NONE:
			return;

		case DBE_THROW_RUNTIME_ERROR:
		{
			DtorProbe	probe(dtors_run);

			throw std::runtime_error("std::runtime_error desde el motor");
		}

		case DBE_THROW_BAD_ALLOC:
		{
			DtorProbe	probe(dtors_run);

			throw std::bad_alloc();
		}

		case DBE_THROW_INT:
		{
			DtorProbe	probe(dtors_run);

			throw 42;
		}

		case DBE_THROW_NESTED:
			nested_level1(dtors_run);
			return;

		default:
			throw Error(DBE_ERR_INVALID_ARGUMENT, "kind desconocido en selftest_throw");
	}
}

void
pg_error(int32_t *dtors_run)
{
	if (dtors_run != nullptr)
		*dtors_run = 0;

	{
		DtorProbe	probe(dtors_run);

		//
		// Acá está el cruce que importa: llamamos a un shim de C que hace
		// ereport(ERROR).  Si el longjmp de Postgres atravesara este frame,
		// `probe` no se destruiría y el contador quedaría en 0.
		//
		// El shim lo detiene y nos devuelve el error como valor; nosotros
		// desenrollamos con una excepción de C++, que sí corre destructores.
		//
		DbeStatus	st = dbe_shim_selftest_raise("error provocado desde C++");

		throw_if_not_ok(st);
	}

	throw Error(DBE_ERR_INTERNAL, "el shim debía haber devuelto error y no lo hizo");
}

void
roundtrip(int32_t *dtors_run)
{
	if (dtors_run != nullptr)
		*dtors_run = 0;

	DtorProbe	probe(dtors_run);
	DbeStatus	st = dbe_shim_selftest_noop();

	throw_if_not_ok(st);
}

}	// namespace selftest
}	// namespace dbe
