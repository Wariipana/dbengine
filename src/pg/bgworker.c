/*-------------------------------------------------------------------------
 *
 * bgworker.c
 *	  Background worker de mantenimiento: compactación del staging buffer
 *	  (F1/R8), reordenamiento en compactación (F6) y purga del version
 *	  store según retention policy (F7).
 *
 * En F0 solo arranca, respeta las señales y loguea cada ciclo.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "dbengine/dbengine.h"

void
dbe_register_bgworker(void)
{
	BackgroundWorker worker;

	MemSet(&worker, 0, sizeof(worker));

	/*
	 * Por ahora solo BGWORKER_SHMEM_ACCESS.  Cuando F7 necesite tocar
	 * catálogos para la purga se agrega BGWORKER_BACKEND_DATABASE_CONNECTION
	 * y la conexión correspondiente.
	 */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 10;
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	snprintf(worker.bgw_library_name, BGW_MAXLEN, "dbengine");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "dbe_bgworker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "dbengine maintenance");
	snprintf(worker.bgw_type, BGW_MAXLEN, "dbengine maintenance");

	RegisterBackgroundWorker(&worker);
}

void
dbe_bgworker_main(Datum main_arg)
{
	uint32		wait_event;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	/*
	 * PG 18 sacó WAIT_EVENT_EXTENSION: ahora las extensiones registran su
	 * propio wait event, así pg_stat_activity muestra un nombre útil en vez
	 * de un genérico "Extension".
	 */
	wait_event = WaitEventExtensionNew("DbengineMaintenanceMain");

	ereport(LOG,
			(errmsg("dbengine: background worker de mantenimiento iniciado "
					"(naptime=%ds, retention_window=%ds)",
					dbe_worker_naptime, dbe_retention_window)));

	while (!ShutdownRequestPending)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 (long) dbe_worker_naptime * 1000L,
						 wait_event);
		ResetLatch(MyLatch);

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		CHECK_FOR_INTERRUPTS();

		/*
		 * F1/R8: compactar staging buffers que pasaron el umbral.
		 * F6: reordenar hasta dbe_max_blocks_per_compaction bloques.
		 * F7: purgar versiones fuera de la ventana de retención.
		 */
		elog(DEBUG1, "dbengine: ciclo de mantenimiento (todavía sin trabajo en F0)");
	}

	ereport(LOG, (errmsg("dbengine: background worker de mantenimiento detenido")));
	proc_exit(0);
}
