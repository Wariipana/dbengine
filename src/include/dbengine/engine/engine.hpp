//===----------------------------------------------------------------------===//
//
// engine.hpp
//   API interna de la CAPA C++.  Solo la incluyen src/engine/ y bridge.cpp.
//
// Nada de acá para adentro puede incluir postgres.h ni llamar a ereport.
// El único contacto con Postgres es a través de dbengine/pgshim.h.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdexcept>
#include <string>

#include "dbengine/status.h"

namespace dbe {

/// Excepción canónica del motor.  Transporta un DbeStatus intacto para que
/// el airlock lo devuelva sin perder el SQLSTATE original cuando el error
/// nació del lado de Postgres.
class Error : public std::runtime_error
{
public:
	explicit Error(const DbeStatus &st)
		: std::runtime_error(st.message[0] != '\0' ? st.message : "error del motor"),
		  status_(st)
	{
	}

	Error(int32_t code, const std::string &msg)
		: std::runtime_error(msg), status_(dbe_status_make(code, msg.c_str()))
	{
	}

	const DbeStatus &status() const noexcept { return status_; }

private:
	DbeStatus	status_;
};

/// Lanza si el estado trae error.  Es la forma en que un DBE_ERR_PG
/// devuelto por un shim se convierte en un unwind ordenado de C++.
inline void
throw_if_not_ok(const DbeStatus &st)
{
	if (st.code != DBE_OK)
		throw Error(st);
}

/// RAII de prueba: cuenta destructores ejecutados.  Es el instrumento que
/// prueba que un longjmp no atravesó el frame (ver test/regress/bridge).
class DtorProbe
{
public:
	explicit DtorProbe(int32_t *counter) noexcept : counter_(counter) {}
	~DtorProbe()
	{
		if (counter_ != nullptr)
			(*counter_)++;
	}

	DtorProbe(const DtorProbe &) = delete;
	DtorProbe &operator=(const DtorProbe &) = delete;

private:
	int32_t    *counter_;
};

void		startup();
void		shutdown();
const char *version() noexcept;

namespace selftest {

void		throw_kind(int32_t kind, int32_t *dtors_run);
void		pg_error(int32_t *dtors_run);
void		roundtrip(int32_t *dtors_run);

}	// namespace selftest
}	// namespace dbe
