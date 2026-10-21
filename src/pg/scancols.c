/*-------------------------------------------------------------------------
 *
 * scancols.c
 *	  Qué columnas necesita cada scan, para que el acceso por TID no cargue
 *	  la tabla entera.
 *
 * El Table AM no recibe la lista de columnas: `index_fetch_tuple` y
 * `scan_bitmap_next_tuple` solo ven la Relation.  Sin esa lista, traer UNA
 * fila obliga a cargar los segmentos de TODAS las columnas del stripe —
 * medido sobre ClickBench, 4.932 buffers (~38 MB) para devolver una fila de
 * 105 columnas, contra 1 buffer que costó encontrarla en el índice.  Ahí se
 * iban los 63 ms de un lookup que en heap cuesta 1,3 ms.
 *
 * La información existe, solo que en el plan y no en la API del AM.  Se la
 * saca con ExecutorStart_hook: al arrancar la query se recorre el árbol de
 * PlanState, y por cada scan sobre una tabla columnar se anotan los attnum
 * que ese nodo referencia.  Es la misma técnica que usa Citus columnar.
 *
 * Ante cualquier duda se anota "todas": perder pruning cuesta I/O, pero
 * podar de más devuelve NULL donde había un valor.
 *
 * QUIÉN PUEDE USAR EL MAPA
 *
 * El mapa describe los scans del PLAN, pero el Table AM también recibe
 * scans que no salen de ningún plan: la construcción de un índice, la
 * reescritura de ALTER TABLE, ANALYZE, COPY ... TO.  Esos necesitan todas
 * las columnas.  Antes los separaba solo la profundidad del executor: con
 * un cursor abierto la profundidad no volvía a cero, el mapa sobrevivía, y
 * un CREATE INDEX o un ALTER TABLE ... TYPE posteriores leían las columnas
 * no anotadas como NULL — un índice lleno de NULL, o una columna borrada
 * para siempre.
 *
 * Ahora el mapa solo vale si el contexto más interno es un ExecutorRun.
 * Un comando utilitario, aunque esté anidado adentro de una query (una
 * función que corre DDL), lo apaga para todo lo que ocurra adentro suyo; un
 * executor anidado adentro del utilitario (CREATE TABLE AS, EXPLAIN
 * ANALYZE) lo vuelve a prender, con sus propias columnas ya anotadas.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "tcop/utility.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "dbengine/columnar.h"
#include "dbengine/dbengine.h"

typedef struct DbeScanColsEntry
{
	Oid			relid;			/* clave */
	Bitmapset  *attrs;			/* attnum + FirstLowInvalidHeapAttributeNumber */
	bool		want_all;		/* no se pudo determinar: cargar todo */
} DbeScanColsEntry;

static HTAB *dbe_scan_cols = NULL;
static int	dbe_exec_depth = 0;
static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;
static ProcessUtility_hook_type prev_process_utility = NULL;

/* Contexto más interno en el que se está ejecutando; ver arriba. */
typedef enum DbeScanContext
{
	DBE_CTX_NONE = 0,
	DBE_CTX_EXECUTOR,
	DBE_CTX_UTILITY
} DbeScanContext;

static DbeScanContext dbe_scan_context = DBE_CTX_NONE;

static void
dbe_scan_cols_reset(void)
{
	if (dbe_scan_cols != NULL)
	{
		hash_destroy(dbe_scan_cols);
		dbe_scan_cols = NULL;
	}
}

static DbeScanColsEntry *
dbe_scan_cols_entry(Oid relid)
{
	DbeScanColsEntry *e;
	bool		found;

	if (dbe_scan_cols == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DbeScanColsEntry);
		ctl.hcxt = TopMemoryContext;
		dbe_scan_cols = hash_create("dbengine scan columns", 8, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	e = (DbeScanColsEntry *) hash_search(dbe_scan_cols, &relid, HASH_ENTER,
										 &found);
	if (!found)
	{
		e->attrs = NULL;
		e->want_all = false;
	}
	return e;
}

/*
 * Anota las columnas que este nodo de scan referencia.
 *
 * Después de setrefs, los Var de un nodo de scan que apuntan a la relación
 * llevan varno = scanrelid, así que pull_varattnos() con ese varno junta
 * exactamente lo suyo y descarta lo que viene de otros lados.
 */
static void
dbe_note_scan(PlanState *ps, Index scanrelid, Relation rel)
{
	DbeScanColsEntry *e;
	Bitmapset  *attrs = NULL;
	Plan	   *plan = ps->plan;
	MemoryContext old;
	int			x = -1;

	if (!dbe_is_columnar_relation(rel))
		return;

	e = dbe_scan_cols_entry(RelationGetRelid(rel));
	if (e->want_all)
		return;

	pull_varattnos((Node *) plan->targetlist, scanrelid, &attrs);
	pull_varattnos((Node *) plan->qual, scanrelid, &attrs);

	switch (nodeTag(plan))
	{
		case T_BitmapHeapScan:
			pull_varattnos((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
						   scanrelid, &attrs);
			break;
		case T_IndexScan:
			pull_varattnos((Node *) ((IndexScan *) plan)->indexqualorig,
						   scanrelid, &attrs);
			pull_varattnos((Node *) ((IndexScan *) plan)->indexorderbyorig,
						   scanrelid, &attrs);
			break;
		case T_TidScan:
			pull_varattnos((Node *) ((TidScan *) plan)->tidquals,
						   scanrelid, &attrs);
			break;
		default:
			break;
	}

	/*
	 * Referencia a la fila entera o a una columna de sistema: no se puede
	 * podar nada.
	 */
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	att = x + FirstLowInvalidHeapAttributeNumber;

		if (att <= 0)
		{
			e->want_all = true;
			e->attrs = NULL;
			return;
		}
	}

	old = MemoryContextSwitchTo(TopMemoryContext);
	e->attrs = bms_add_members(e->attrs, attrs);
	MemoryContextSwitchTo(old);
}

static bool
dbe_collect_cols(PlanState *ps, void *context)
{
	EState	   *estate = (EState *) context;

	if (ps == NULL)
		return false;

	if (IsA(ps, SeqScanState) || IsA(ps, IndexScanState) ||
		IsA(ps, BitmapHeapScanState) || IsA(ps, TidScanState) ||
		IsA(ps, IndexOnlyScanState))
	{
		ScanState  *ss = (ScanState *) ps;
		Index		relid = ((Scan *) ps->plan)->scanrelid;

		if (relid > 0 && ss->ss_currentRelation != NULL)
			dbe_note_scan(ps, relid, ss->ss_currentRelation);
	}

	return planstate_tree_walker(ps, dbe_collect_cols, estate);
}

static void
dbe_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * Solo el nivel más externo limpia: una función PL puede abrir queries
	 * anidadas, y tirar la tabla desde adentro dejaría al scan de afuera sin
	 * su lista a mitad de ejecución.
	 */
	if (dbe_exec_depth == 0)
		dbe_scan_cols_reset();
	dbe_exec_depth++;

	if (queryDesc->planstate != NULL)
		(void) dbe_collect_cols(queryDesc->planstate, queryDesc->estate);
}

static void
dbe_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	DbeScanContext saved = dbe_scan_context;

	dbe_scan_context = DBE_CTX_EXECUTOR;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		dbe_scan_context = saved;
	}
	PG_END_TRY();
}

static void
dbe_process_utility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	DbeScanContext saved = dbe_scan_context;

	dbe_scan_context = DBE_CTX_UTILITY;
	PG_TRY();
	{
		if (prev_process_utility)
			prev_process_utility(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		dbe_scan_context = saved;
	}
	PG_END_TRY();
}

/*
 * Al terminar la transacción no queda ningún executor vivo: los cursores
 * WITH HOLD ya se materializaron en el pre-commit.  Si un error se salteó
 * ExecutorEnd, la profundidad quedaba arriba de cero para siempre y el mapa
 * no se volvía a limpiar en todo el resto de la sesión.
 */
static void
dbe_scancols_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			dbe_exec_depth = 0;
			dbe_scan_cols_reset();
			break;
		default:
			break;
	}
}

static void
dbe_executor_end(QueryDesc *queryDesc)
{
	if (dbe_exec_depth > 0)
		dbe_exec_depth--;
	if (dbe_exec_depth == 0)
		dbe_scan_cols_reset();

	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
dbe_scancols_init(void)
{
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = dbe_executor_start;
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = dbe_executor_run;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = dbe_executor_end;
	prev_process_utility = ProcessUtility_hook;
	ProcessUtility_hook = dbe_process_utility;
	RegisterXactCallback(dbe_scancols_xact_callback, NULL);
}

/*
 * Llena `wanted` con las columnas que la query pidió de esta relación.
 * Devuelve false si no se sabe, y ahí el caller tiene que leerlas todas.
 */
bool
dbe_scan_columns_for(Oid relid, int natts, bool *wanted)
{
	DbeScanColsEntry *e;
	int			x = -1;
	bool		any = false;

	if (dbe_scan_cols == NULL)
		return false;

	/* Solo los scans de un plan en ejecución: ver el comentario de arriba. */
	if (dbe_scan_context != DBE_CTX_EXECUTOR)
		return false;

	e = (DbeScanColsEntry *) hash_search(dbe_scan_cols, &relid, HASH_FIND,
										 NULL);
	if (e == NULL || e->want_all || e->attrs == NULL)
		return false;

	memset(wanted, 0, sizeof(bool) * natts);
	while ((x = bms_next_member(e->attrs, x)) >= 0)
	{
		AttrNumber	att = x + FirstLowInvalidHeapAttributeNumber;

		if (att < 1 || att > natts)
			return false;		/* algo no cuadra: leer todo */
		wanted[att - 1] = true;
		any = true;
	}

	return any;
}
