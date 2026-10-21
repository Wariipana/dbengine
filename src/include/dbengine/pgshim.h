/*-------------------------------------------------------------------------
 *
 * pgshim.h
 *	  Airlock C++ -> C.  Servicios de Postgres que el motor necesita,
 *	  envueltos en PG_TRY/PG_CATCH del lado C.
 *
 * Implementado en src/pg/pgshim.c, que es C PURO a propósito: el
 * setjmp/longjmp de PG_TRY nunca aparece en una unidad de traducción de
 * C++, así que no hay riesgo de saltar por encima de destructores.
 *
 * CONTRATO al capturar un error de Postgres:
 *   El shim hace CopyErrorData() + FlushErrorState() y devuelve DBE_ERR_PG.
 *   Eso NO libera los recursos que Postgres tenía tomados (buffers, locks,
 *   ...).  Por eso el motor está obligado a tratar DBE_ERR_PG como
 *   "desenrollar ya mismo": propagarlo hasta el airlock de bridge.cpp sin
 *   volver a llamar a ningún shim.  La capa C re-lanza con ereport(ERROR),
 *   y ahí sí el abort de transacción libera todo por resource owner.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_PGSHIM_H
#define DBENGINE_PGSHIM_H

#include "dbengine/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Log sin riesgo de longjmp: elevel se fuerza a <= LOG. */
void		dbe_shim_log(const char *msg);

/*
 * Memoria del motor.  El motor NO usa palloc: su memoria vive fuera de los
 * MemoryContext de Postgres porque puede sobrevivir a un query context y
 * porque los contenedores de C++ necesitan un allocator que no haga
 * longjmp al quedarse sin memoria.  Estos son malloc/free envueltos para
 * poder contabilizarlos más adelante.
 */
void	   *dbe_shim_alloc(size_t sz);
void		dbe_shim_free(void *p);

/*
 * Shim de prueba (F0): ejecuta ereport(ERROR) adentro de un PG_TRY y
 * devuelve el error como DbeStatus en vez de dejar volar el longjmp.
 */
DbeStatus	dbe_shim_selftest_raise(const char *msg);

/* Igual pero sin error: verifica el camino feliz del ida y vuelta. */
DbeStatus	dbe_shim_selftest_noop(void);

#ifdef __cplusplus
}								/* extern "C" */
#endif

#endif							/* DBENGINE_PGSHIM_H */
