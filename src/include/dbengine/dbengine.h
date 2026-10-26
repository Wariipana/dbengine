/*-------------------------------------------------------------------------
 *
 * dbengine.h
 *	  Header interno de la CAPA C.  Solo lo incluyen archivos de src/pg/.
 *	  src/engine/ nunca debe ver este header (arrastra postgres.h).
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_DBENGINE_H
#define DBENGINE_DBENGINE_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/rel.h"

#include "dbengine/status.h"

#if PG_VERSION_NUM < 180000
#error "dbengine requiere PostgreSQL 18 o superior"
#endif

/*
 * PG 19: un TupleDesc armado a mano tiene que pasar por TupleDescFinalize()
 * antes de usarse (calcula firstNonCachedOffsetAttr; con --enable-cassert
 * deformar una tupla sin eso es un TRAP).  En PG 18 no existe ni hace falta.
 */
#if PG_VERSION_NUM >= 190000
#define dbe_tupdesc_finalize(td) TupleDescFinalize(td)
#else
#define dbe_tupdesc_finalize(td) ((void) 0)
#endif

#define DBENGINE_VERSION "0.2.0"

/* --- GUCs (definidas en init.c) ---------------------------------------- */
extern bool dbe_enable_vectorized;
extern bool dbe_enable_agg_pushdown;
extern bool dbe_enable_compression;
extern bool dbe_enable_zonemap_pruning;
extern bool dbe_enable_granule_pruning;
extern bool dbe_enable_dict_quals;
extern bool dbe_enable_scan_dedup;
extern bool dbe_enable_flat_hash;
extern bool dbe_kernel_readahead;
extern bool dbe_enable_learned_zonemaps;
extern bool dbe_enable_adaptive_clustering;
extern int	dbe_retention_window;	/* segundos */
extern int	dbe_block_rows;
extern int	dbe_batch_size;
extern int	dbe_staging_threshold_rows;
extern int	dbe_max_blocks_per_compaction;
extern int	dbe_worker_naptime;		/* segundos */

/* --- Estado de carga --------------------------------------------------- */

/*
 * True si la biblioteca se cargó vía shared_preload_libraries.  Los hooks
 * del planner y el background worker solo se pueden registrar en ese caso.
 */
extern bool dbe_preloaded;
extern int	dbe_bloom_bits_per_row;

/*
 * Aborta con un mensaje claro si `feature` necesita preload y no lo hay.
 * Definido en init.c.
 */
extern void dbe_require_preload(const char *feature);

/* --- Manejo de errores (errors.c) -------------------------------------- */

/*
 * Convierte un DbeStatus no-OK en ereport(ERROR).  Se llama SIEMPRE desde
 * un frame de C, nunca desde C++: el longjmp resultante no cruza nada.
 * No retorna si el estado es de error.
 */
extern void dbe_error_if_not_ok(const DbeStatus *st, const char *context);

/* --- Table AM (tableam.c) ---------------------------------------------- */

/* True si la relación usa el access method columnar de dbengine. */
extern void dbe_tableam_init(void);
extern bool dbe_debug_backtrace;
extern void dbe_debug_backtrace_install(void);
extern bool dbe_is_columnar_relation(Relation rel);

/* --- Background worker (bgworker.c) ------------------------------------ */
extern void dbe_register_bgworker(void);
PGDLLEXPORT void dbe_bgworker_main(Datum main_arg);

#endif							/* DBENGINE_DBENGINE_H */
