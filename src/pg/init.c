/*-------------------------------------------------------------------------
 *
 * init.c
 *	  _PG_init, GUCs, detección de shared_preload_libraries y arranque del
 *	  motor.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"
#include "dbengine/workload.h"
#include "dbengine/bridge.h"
#include "dbengine/customscan.h"

/*
 * PG_MODULE_MAGIC_EXT existe desde PG 18 y agrega nombre/versión al bloque
 * mágico, así que \dx y los logs de error identifican el módulo.
 */
PG_MODULE_MAGIC_EXT(
					.name = "dbengine",
					.version = DBENGINE_VERSION
);

bool		dbe_enable_vectorized = true;
bool		dbe_enable_agg_pushdown = true;
bool		dbe_enable_compression = true;
bool		dbe_enable_zonemap_pruning = true;
bool		dbe_enable_granule_pruning = true;
bool		dbe_enable_dict_quals = true;
bool		dbe_enable_learned_zonemaps = false;	/* F5, experimental */
bool		dbe_enable_adaptive_clustering = false; /* F6 */
int			dbe_retention_window = 86400;
int			dbe_block_rows = 65536;
int			dbe_batch_size = 1024;
int			dbe_staging_threshold_rows = 65536;
int			dbe_max_blocks_per_compaction = 64;
int			dbe_worker_naptime = 60;

bool		dbe_preloaded = false;
int			dbe_bloom_bits_per_row = 0;

static void dbe_define_gucs(void);
static void dbe_on_proc_exit(int code, Datum arg);

void
_PG_init(void)
{
	DbeStatus	st;

	/*
	 * process_shared_preload_libraries_in_progress solo es true cuando el
	 * postmaster está procesando shared_preload_libraries.  Si llegamos
	 * acá por un LOAD o por CREATE EXTENSION, es false y las features que
	 * necesitan hooks globales no están disponibles.
	 */
	dbe_preloaded = process_shared_preload_libraries_in_progress;

	dbe_define_gucs();

	dbe_tableam_init();
	dbe_catalog_drop_hook_init();
	dbe_debug_backtrace_install();

	MarkGUCPrefixReserved("dbengine");

	/*
	 * Arranque del motor C++.  Cruza el airlock: si tira, vuelve como
	 * DbeStatus y recién acá lo convertimos en ereport.
	 */
	st = dbe_engine_startup();
	dbe_error_if_not_ok(&st, "inicializando el motor");

	/*
	 * _PG_fini no existe desde PG 17, así que el apagado ordenado del motor
	 * (a futuro: flush del staging buffer) va por on_proc_exit.  La lista se
	 * hereda al fork, así que cada backend lo corre al terminar.
	 */
	on_proc_exit(dbe_on_proc_exit, (Datum) 0);

	/*
	 * El hook del planner se instala siempre.  Con la biblioteca precargada
	 * está desde la primera query; si se cargó por CREATE EXTENSION, desde
	 * la primera que toque la extensión.  En los dos casos es correcto: sin
	 * hook simplemente se usa el seq scan del Table AM.
	 */
	dbe_customscan_init();
	dbe_vectoragg_init();
	dbe_scancols_init();

	/*
	 * El canal de feedback pide memoria compartida, así que solo se puede
	 * armar desde el postmaster: los hooks de shmem no corren para una
	 * biblioteca cargada por CREATE EXTENSION.  Sin precarga el canal queda
	 * apagado y anotar no hace nada.
	 */
	dbe_workload_init();

	if (dbe_preloaded)
		dbe_register_bgworker();
	else
		ereport(DEBUG1,
				(errmsg("dbengine cargado sin shared_preload_libraries: "
						"background worker y hooks del planner deshabilitados")));
}

/*
 * Callback de salida del proceso.  No puede hacer ereport(ERROR): estamos
 * en pleno shutdown y un longjmp acá no tiene a dónde ir.  Se loguea y se
 * sigue.
 */
static void
dbe_on_proc_exit(int code, Datum arg)
{
	DbeStatus	st = dbe_engine_shutdown();

	if (st.code != DBE_OK)
		ereport(LOG,
				(errmsg("dbengine: el motor no se apagó limpiamente: %s",
						st.message[0] != '\0' ? st.message : dbe_code_name(st.code))));
}

static void
dbe_define_gucs(void)
{
	DefineCustomBoolVariable("dbengine.enable_vectorized",
							 "Habilita el Custom Scan vectorizado.",
							 "Si está apagado, todos los planes usan el executor nativo.",
							 &dbe_enable_vectorized,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_agg_pushdown",
							 "Empuja las agregaciones simples adentro del scan columnar.",
							 "Evita construir una tupla por fila para después tirarla. "
							 "Requiere dbengine.enable_vectorized.",
							 &dbe_enable_agg_pushdown,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.debug_backtrace",
							 "Imprime un backtrace en el log si el backend recibe SIGSEGV.",
							 "Diagnóstico. Solo tiene efecto si se lee al arrancar el "
							 "proceso, o sea con la biblioteca precargada.",
							 &dbe_debug_backtrace,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.bloom_bits_per_row",
							"Bits por fila del filtro de Bloom de cada segmento.",
							"Descarta stripes con predicados de igualdad sobre "
							"columnas que el zone map min/max no puede acotar. "
							"En 0 el filtro solo se construye para las columnas "
							"que el canal de feedback vio con predicados de "
							"igualdad; con un valor mayor se construye siempre. "
							"A 8 bits la tasa de falsos positivos es ~2%.",
							&dbe_bloom_bits_per_row,
							0, 0, 32,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_compression",
							 "Habilita la compresión adaptativa por segmento.",
							 "Apagarlo guarda los valores crudos. Sirve para medir "
							 "la ganancia de la compresión sin cambiar nada más.",
							 &dbe_enable_compression,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_zonemap_pruning",
							 "Habilita el descarte de bloques por zone maps min/max.",
							 NULL,
							 &dbe_enable_zonemap_pruning,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_granule_pruning",
							 "Habilita el descarte por gránulo dentro de un stripe.",
							 "El stripe es la unidad de escritura; el gránulo, de "
							 "descarte. Un stripe que pasa el filtro puede igual "
							 "saltearse la mayoría de sus filas. No ahorra I/O: "
							 "ahorra decodificar y evaluar el predicado.",
							 &dbe_enable_granule_pruning,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_dict_quals",
							 "Evalúa los predicados una vez por entrada del diccionario.",
							 "Una cláusula que mira una sola columna con diccionario "
							 "se evalúa la primera vez que aparece cada valor y el "
							 "resultado se reusa en las demás filas del stripe que "
							 "tienen el mismo código.",
							 &dbe_enable_dict_quals,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_learned_zonemaps",
							 "Habilita la capa experimental de zone maps aprendidos.",
							 "Refinamiento opcional sobre los min/max. Un modelo degradado "
							 "solo puede costar performance, nunca corrección.",
							 &dbe_enable_learned_zonemaps,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("dbengine.enable_adaptive_clustering",
							 "Habilita el reordenamiento de bloques guiado por el workload.",
							 "El reordenamiento ocurre solo durante la compactación, "
							 "nunca durante una query.",
							 &dbe_enable_adaptive_clustering,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.retention_window",
							"Hasta dónde atrás tiene que seguir siendo exacto el time travel.",
							"La fusión de stripes coarsa la granularidad del time "
							"travel, así que no toca nada más nuevo que esto. "
							"En 0 fusiona todo y el time travel queda con la "
							"granularidad que dejen los stripes fusionados.",
							&dbe_retention_window,
							86400,
							0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.block_rows",
							"Filas por bloque columnar.",
							NULL,
							&dbe_block_rows,
							65536,
							1024, 1048576,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.batch_size",
							"Tamaño del batch de los operadores vectorizados.",
							NULL,
							&dbe_batch_size,
							1024,
							64, 65536,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.staging_threshold_rows",
							"Filas acumuladas en el staging buffer antes de compactar.",
							NULL,
							&dbe_staging_threshold_rows,
							65536,
							1, 16777216,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.max_blocks_per_compaction",
							"Presupuesto de bloques que un ciclo de compactación puede reordenar.",
							"Acota la duración de cada ciclo.",
							&dbe_max_blocks_per_compaction,
							64,
							0, 1048576,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("dbengine.worker_naptime",
							"Intervalo entre ciclos del background worker, en segundos.",
							NULL,
							&dbe_worker_naptime,
							60,
							1, 86400,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);
}

void
dbe_require_preload(const char *feature)
{
	if (!dbe_preloaded)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s necesita que dbengine esté en shared_preload_libraries",
						feature),
				 errhint("Agregá 'dbengine' a shared_preload_libraries y reiniciá el servidor.")));
}
