/*-------------------------------------------------------------------------
 *
 * status.h
 *	  Tipo de estado que cruza la frontera C <-> C++ (R2).
 *
 * Este header lo incluyen LAS DOS capas, por eso no puede depender de
 * postgres.h ni de <exception>.  Es C plano, POD, sin asignación dinámica:
 * un DbeStatus se puede copiar por valor entre mundos sin que ninguno de
 * los dos tenga que liberar nada del otro.
 *
 * REGLA DE ORO (R2):
 *   - Ninguna excepción de C++ escapa a un frame de C.
 *   - Ningún longjmp de ereport/elog atraviesa un frame de C++.
 *   - Todo error viaja como DbeStatus por valor; se convierte en
 *     ereport() o en excepción SOLO dentro de la capa que corresponde.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_STATUS_H
#define DBENGINE_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBE_STATUS_MSG_MAX 512

/* Códigos de error del motor.  No son SQLSTATE; el mapeo a SQLSTATE lo
 * hace la capa C en pg/errors.c. */
typedef enum DbeCode
{
	DBE_OK = 0,
	DBE_ERR_EXCEPTION,			/* excepción de C++ atrapada en el airlock */
	DBE_ERR_BAD_ALLOC,			/* std::bad_alloc */
	DBE_ERR_UNKNOWN_EXCEPTION,	/* catch(...) de algo que no es std::exception */
	DBE_ERR_PG,					/* error de Postgres capturado por un shim */
	DBE_ERR_INVALID_ARGUMENT,
	DBE_ERR_NOT_IMPLEMENTED,
	DBE_ERR_INTERNAL
} DbeCode;

typedef struct DbeStatus
{
	int32_t		code;			/* DbeCode */
	int32_t		pg_sqlerrcode;	/* SQLSTATE original si code == DBE_ERR_PG, si no 0 */
	char		message[DBE_STATUS_MSG_MAX];
} DbeStatus;

/*
 * Helpers inline: sin asignación, sin dependencias.  Deliberadamente no
 * usamos snprintf con formato acá para que el header sea trivialmente
 * auditable; el formateo se hace del lado que corresponda.
 */
static inline DbeStatus
dbe_status_ok(void)
{
	DbeStatus	s;

	s.code = DBE_OK;
	s.pg_sqlerrcode = 0;
	s.message[0] = '\0';
	return s;
}

static inline int
dbe_status_is_ok(const DbeStatus *s)
{
	return s != NULL && s->code == DBE_OK;
}

/* Copia truncada y siempre NUL-terminada.  No usa strncpy para no depender
 * de <string.h> con semánticas distintas entre capas. */
static inline void
dbe_status_set_message(DbeStatus *s, const char *msg)
{
	size_t		i = 0;

	if (msg != NULL)
	{
		for (; i < (size_t) (DBE_STATUS_MSG_MAX - 1) && msg[i] != '\0'; i++)
			s->message[i] = msg[i];
	}
	s->message[i] = '\0';
}

static inline DbeStatus
dbe_status_make(int32_t code, const char *msg)
{
	DbeStatus	s;

	s.code = code;
	s.pg_sqlerrcode = 0;
	dbe_status_set_message(&s, msg);
	return s;
}

const char *dbe_code_name(int32_t code);

#ifdef __cplusplus
}								/* extern "C" */
#endif

#endif							/* DBENGINE_STATUS_H */
