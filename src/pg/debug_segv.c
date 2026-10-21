/*
 * debug_segv.c
 *	  Backtrace propio ante SIGSEGV, para entornos sin core dumps.
 *
 * Cuando ptrace está restringido a descendientes y /proc/sys no se puede
 * escribir para redirigir los core dumps, no hay forma de sacarle un
 * backtrace a un backend que se cayó: ni attach ni core.  La única que
 * queda es que el propio backend lo imprima antes de morir.
 *
 * Está apagado por defecto.  El handler no es async-signal-safe en sentido
 * estricto — backtrace() puede pedir memoria — pero el proceso ya está
 * muerto y la información vale más que la pureza.  No usarlo en producción.
 *
 * Con esto se encontró el último crash de F1: dbe_stripe_visible() aplicaba
 * la regla MVCC a un SnapshotNonVacuumable, cuyos campos xmin/xip ni
 * siquiera están inicializados.
 */
#include "postgres.h"

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "utils/guc.h"

#include "dbengine/dbengine.h"

bool		dbe_debug_backtrace = false;

static void
dbe_segv_handler(int sig)
{
	void	   *frames[64];
	int			n = backtrace(frames, 64);
	char		head[64];
	int			len;

	len = snprintf(head, sizeof(head), "\nDBENGINE BACKTRACE pid=%d sig=%d\n",
				   (int) getpid(), sig);
	if (write(STDERR_FILENO, head, len) < 0)
		 /* nada que hacer en un handler */ ;
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	_exit(128 + sig);
}

void
dbe_debug_backtrace_install(void)
{
	if (!dbe_debug_backtrace)
		return;
	signal(SIGSEGV, dbe_segv_handler);
	signal(SIGBUS, dbe_segv_handler);
	signal(SIGILL, dbe_segv_handler);
}
