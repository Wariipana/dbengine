/*-------------------------------------------------------------------------
 *
 * vectoragg.c
 *	  Agregación empujada adentro del scan columnar (F3).
 *
 * El nodo Agg de Postgres pide una fila por vez.  Para agregar 100M filas
 * eso significa construir 100M TupleTableSlot, pasarlos por ExecScan,
 * ExecProcNode y la proyección — y después tirarlos.  Medido: 2 s de las
 * que casi nada es leer datos.
 *
 * Este nodo reemplaza el par Agg+Scan cuando la forma de la query lo
 * permite: recorre los segmentos y alimenta las funciones de transición
 * directamente, sin armar tuplas intermedias.  La frontera de conversión
 * es una sola, al final, con el resultado.
 *
 * SEMÁNTICA: se usan las funciones de transición y finalización del propio
 * catálogo (pg_aggregate), con la misma inicialización que nodeAgg.  No se
 * reimplementa ninguna agregación, así que `sum(int4)` desborda igual y
 * `avg(numeric)` redondea igual.  Lo único que cambia es de dónde salen los
 * valores de entrada.
 *
 * Cualquier forma que no encaje exactamente cae al plan normal.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "access/stratnum.h"
#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "utils/ruleutils.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "parser/parse_agg.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/customscan.h"
#include "dbengine/dbengine.h"
#include "dbengine/zonemap.h"

#define DBE_AGG_NAME "DbengineVectorAgg"

static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;

/* Un agregado que este nodo sabe calcular. */
typedef struct DbeAggState
{
	AttrNumber	attnum;			/* columna de entrada; 0 = count(*) */
	bool		is_count_star;

	FmgrInfo	transfn;
	bool		transfn_strict;
	FmgrInfo	finalfn;
	bool		has_finalfn;
	int			finalfn_nargs;

	/*
	 * Modo parcial: en un plan paralelo este nodo hace de Partial Agg, o
	 * sea que no llama a la función de finalización sino que entrega el
	 * estado de transición.  Si ese estado es `internal` hay que
	 * serializarlo — es lo que hace nodeAgg con AGGSPLIT_INITIAL_SERIAL.
	 */
	bool		skip_final;
	bool		serialize;
	FmgrInfo	serialfn;

	Oid			transtype;
	int16		transtyplen;
	bool		transtypbyval;
	Oid			inputtype;
	int16		inputtyplen;
	bool		inputtypbyval;
	Oid			collation;

	/*
	 * MIN/MAX se reconocen por pg_aggregate.aggsortop, que es la misma
	 * marca que usa el planner para el truco de resolverlos con un índice.
	 * Con eso se pueden contestar desde los zone maps, sin leer datos.
	 */
	bool		is_minmax;
	bool		want_min;
	FmgrInfo	minmax_cmp;

	/* Vista de la columna en el stripe que se está recorriendo. */
	bool		has_vector;
	DbeColumnVector vec;

	Datum		transvalue;
	bool		transnull;
	bool		nothing_yet;	/* transfn strict y todavía sin primer valor */
	int64		count;			/* para count(*) */

	/*
	 * Valor inicial guardado, para poder reiniciar el agregado en cada
	 * grupo sin volver a mirar pg_aggregate.  Se copia al reiniciar porque
	 * una función de transición puede modificar su estado in situ, y si
	 * ese estado fuera el inicial compartido lo rompería para los grupos
	 * siguientes.  Es lo mismo que hace nodeAgg.
	 */
	Datum		initvalue;
	bool		initnull;
	bool		init_nothing_yet;

	/*
	 * Las entradas del tlist que son columna de agrupación viajan en este
	 * mismo arreglo, para que quede paralelo a custom_scan_tlist.  No
	 * agregan nada: su valor sale de la clave del grupo.
	 */
	bool		is_group;
	AttrNumber	group_attnum;
	int			group_idx;

	/*
	 * count(DISTINCT col): un conjunto de hash con los valores ya vistos.
	 * El contador cuenta valores nuevos, no filas.
	 */
	bool		is_distinct;
	TupleHashTable dist_table;
	TupleTableSlot *dist_slot;
	MemoryContext dist_cxt;
	MemoryContext dist_tempcxt;
	uint64		dist_since_reset;
} DbeAggState;

typedef struct DbeAggParallelShared
{
	pg_atomic_uint64 next_stripe;

	/* Contadores del EXPLAIN; ver la nota en customscan.c. */
	pg_atomic_uint64 rows_seen;
	pg_atomic_uint64 rows_filtered;
	pg_atomic_uint64 stripes_pruned;
	pg_atomic_uint64 granules_pruned;
} DbeAggParallelShared;

typedef struct DbeVectorAggState
{
	CustomScanState css;
	DbeAggParallelShared *shared;	/* NULL si el nodo corre serial */
	bool		only_count_star;
	bool		only_metadata;	/* todos count(*) o min/max */

	/*
	 * Contexto de agregación para las funciones de transición.
	 *
	 * Varios agregados usan un estado `internal` — sum(bigint), sum(numeric),
	 * avg(...) — y sus funciones llaman a AggCheckCallContext() para saber
	 * en qué MemoryContext dejarlo; sin un AggState arriba fallan con
	 * "aggregate function called in non-aggregate context".
	 *
	 * Se les pasa uno mínimo, con curaggcontext apuntando a un ExprContext
	 * que vive toda la agregación y no se resetea.  Es lo único que
	 * AggCheckCallContext() mira.  Las formas que además necesitan
	 * AggGetAggref() (ordered-set, hypothetical) se descartan antes, en la
	 * planificación.
	 */
	AggState   *agg_context;
	Relation	relation;
	bool	   *wanted;
	DbeRowCursor *cursor;
	TupleTableSlot *rel_slot;	/* filas de la relación; el de scan describe la salida */
	DbeAggState *aggs;
	int			naggs;
	bool		done;
	uint64		rows_seen;

	/*
	 * WHERE empujado adentro del nodo.
	 *
	 * Las cláusulas viajan en custom_private con sus Var originales (varno
	 * = relid de la tabla, varattno = número de columna).  setrefs.c no
	 * toca custom_private, así que siguen siendo evaluables contra un slot
	 * con el descriptor de la relación: ExecInitExpr traduce cualquier
	 * varno que no sea INNER_VAR/OUTER_VAR a EEOP_SCAN_VAR, que lee
	 * ecxt_scantuple por varattno.
	 */
	ExprState  *qual;
	DbeFilterSet *filters;
	int			nqualcols;
	AttrNumber *qualcols;
	DbeColumnVector *qualvecs;
	bool	   *qualvec_ok;
	int16	   *qual_typlen;
	bool	   *qual_typbyval;
	uint64		rows_filtered;
	uint64		stripes_pruned;	/* total del plan, solo si totals_ready */
	uint64		granules_pruned;
	bool		published;		/* ya sumó sus contadores al DSM */
	bool		totals_ready;

	/* ------------------------------------------------------------------
	 * GROUP BY sobre la columna de clustering.
	 *
	 * Solo se arma cuando el planner comprobó que el scan ya entrega las
	 * filas ordenadas por las columnas de agrupación: con eso los grupos
	 * quedan contiguos y se agregan de corrido, sin tabla de hash y sin
	 * guardar más que el grupo que se está armando.
	 * ------------------------------------------------------------------ */
	int			ngroupcols;
	AttrNumber *groupcols;
	DbeColumnVector *groupvecs;
	bool	   *groupvec_ok;
	int16	   *group_typlen;
	bool	   *group_typbyval;
	FmgrInfo   *group_eq;		/* igualdad del tipo, por columna */
	Oid		   *group_collation;

	Datum	   *cur_group;		/* clave del grupo abierto */
	bool	   *cur_group_null;
	bool		have_group;
	MemoryContext group_cxt;	/* donde vive cur_group */

	/* Posición para poder cortar en medio de un stripe y retomar. */
	bool		in_stripe;
	uint64		cur_row;
	uint64		stripe_nrows;
	bool		stripe_all_visible;

	/*
	 * Agrupación por hash: la que NO necesita que la tabla esté ordenada.
	 * Se recorre todo una vez llenando la tabla y después se emite un grupo
	 * por llamada.  A cambio del hash por fila, aplica a cualquier columna.
	 */
	bool		hashed;
	TupleHashTable hashtable;
	/*
	 * Dos slots con el mismo descriptor y distinto tipo: el virtual se llena
	 * a mano para buscar en la tabla, y el de minimal tuple es el único que
	 * puede recibir la clave guardada en la entrada.  Un slot virtual
	 * rechaza ExecStoreMinimalTuple.
	 */
	TupleTableSlot *group_slot;	/* virtual, para buscar */
	TupleTableSlot *group_key_slot;	/* minimal, para leer la clave */
	TupleHashIterator hash_iter;
	bool		hash_filled;
	bool		hash_more;		/* quedó scan sin terminar por memoria */
	Size		hash_budget;
	MemoryContext hash_aggcxt;	/* estados de transición, vive todo el scan */
	MemoryContext hash_tempcxt;
	MemoryContext dictcxt;		/* cache de códigos, se resetea por stripe */
	Oid		   *group_eqops;
} DbeVectorAggState;

/*
 * Lo mutable de un agregado, por grupo.  Va en el "additional" de cada
 * entrada de la tabla de hash: el resto de DbeAggState —funciones, tipos,
 * banderas— es igual para todos los grupos y no hace falta repetirlo.
 */
typedef struct DbeGroupAggs
{
	Datum		transvalue;
	bool		transnull;
	bool		nothing_yet;
	int64		count;
} DbeGroupAggs;

static Plan *dbe_agg_plan_path(PlannerInfo *root, RelOptInfo *rel,
							   struct CustomPath *best_path, List *tlist,
							   List *clauses, List *custom_plans);
static Node *dbe_agg_create_state(CustomScan *cscan);
static void dbe_agg_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *dbe_agg_exec(CustomScanState *node);
static void dbe_agg_end(CustomScanState *node);
static void dbe_agg_rescan(CustomScanState *node);
static void dbe_agg_shutdown(CustomScanState *node);
static void dbe_agg_explain(CustomScanState *node, List *ancestors,
							ExplainState *es);
static Size dbe_agg_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
static void dbe_agg_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								   void *coordinate);
static void dbe_agg_reinitialize_dsm(CustomScanState *node,
									 ParallelContext *pcxt, void *coordinate);
static void dbe_agg_initialize_worker(CustomScanState *node, shm_toc *toc,
									  void *coordinate);
static void dbe_agg_missing_value(DbeVectorAggState *state, AttrNumber attnum,
								  Datum *value, bool *isnull);
typedef enum DbeStripeTake
{
	DBE_TAKE_DONE,			/* no queda nada por hacer */
	DBE_TAKE_STRIPE,			/* el cursor quedó parado en un stripe */
	DBE_TAKE_SKIP				/* vacío o descartado: seguir con el próximo */
} DbeStripeTake;

static DbeStripeTake dbe_agg_take_stripe(DbeVectorAggState *state);
static bool dbe_agg_is_partial(DbeVectorAggState *state);
static void dbe_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel,
								   RelOptInfo *output_rel, void *extra);

static const CustomPathMethods dbe_agg_path_methods = {
	.CustomName = DBE_AGG_NAME,
	.PlanCustomPath = dbe_agg_plan_path,
};

static const CustomScanMethods dbe_agg_scan_methods = {
	.CustomName = DBE_AGG_NAME,
	.CreateCustomScanState = dbe_agg_create_state,
};

static const CustomExecMethods dbe_agg_exec_methods = {
	.CustomName = DBE_AGG_NAME,
	.BeginCustomScan = dbe_agg_begin,
	.ExecCustomScan = dbe_agg_exec,
	.EndCustomScan = dbe_agg_end,
	.ReScanCustomScan = dbe_agg_rescan,
	.ShutdownCustomScan = dbe_agg_shutdown,
	.EstimateDSMCustomScan = dbe_agg_estimate_dsm,
	.InitializeDSMCustomScan = dbe_agg_initialize_dsm,
	.ReInitializeDSMCustomScan = dbe_agg_reinitialize_dsm,
	.InitializeWorkerCustomScan = dbe_agg_initialize_worker,
	.ExplainCustomScan = dbe_agg_explain,
};

void
dbe_vectoragg_init(void)
{
	RegisterCustomScanMethods(&dbe_agg_scan_methods);

	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = dbe_create_upper_paths;
}

/* ------------------------------------------------------------------------
 * Planificación
 * ------------------------------------------------------------------------ */

/*
 * ¿Es un Aggref que este nodo sabe calcular?
 *
 * La lista de exclusiones es a propósito larga y las condiciones se chequean
 * de forma positiva: cualquier forma que no se reconozca cae al plan normal,
 * que siempre está disponible y es correcto.
 */
static bool
dbe_agg_supported(Aggref *ref, Index relid, AttrNumber *attnum,
				  bool grouped, bool *is_distinct)
{
	Node	   *arg;

	*is_distinct = false;

	if (ref->aggorder != NIL ||
		ref->aggfilter != NULL || ref->aggvariadic ||
		ref->aggkind != AGGKIND_NORMAL ||
		ref->aggsplit != AGGSPLIT_SIMPLE)
		return false;

	/*
	 * count(DISTINCT col) se resuelve con un conjunto de hash sobre la
	 * columna: es lo mismo que hace nodeAgg, pero sin armar una tupla por
	 * fila para llegar hasta él.  Solo count(): los demás agregados sobre
	 * DISTINCT necesitarían además el valor, no solo saber si ya se vio.
	 *
	 * Y solo sin GROUP BY: por grupo haría falta un conjunto por grupo, que
	 * es otra estructura y otra decisión de memoria.
	 */
	if (ref->aggdistinct != NIL)
	{
		if (grouped || list_length(ref->args) != 1 ||
			list_length(ref->aggdistinct) != 1)
			return false;
		if (get_func_name(ref->aggfnoid) == NULL ||
			strcmp(get_func_name(ref->aggfnoid), "count") != 0)
			return false;
		*is_distinct = true;
	}

	if (ref->aggstar)
	{
		*attnum = InvalidAttrNumber;	/* count(*) */
		return true;
	}

	if (list_length(ref->args) != 1)
		return false;

	arg = (Node *) ((TargetEntry *) linitial(ref->args))->expr;
	if (!IsA(arg, Var))
		return false;

	{
		Var		   *var = (Var *) arg;

		if (var->varno != relid || var->varattno <= 0 || var->varlevelsup != 0)
			return false;
		*attnum = var->varattno;
	}

	return true;
}

/* ¿Todos los agregados del tlist son MIN o MAX? */
static bool
dbe_all_minmax(List *tlist)
{
	ListCell   *lc;

	foreach(lc, tlist)
	{
		Aggref	   *ref = (Aggref *) ((TargetEntry *) lfirst(lc))->expr;
		HeapTuple	tup;
		bool		has_sortop;

		if (ref->aggstar)
			continue;

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
		if (!HeapTupleIsValid(tup))
			return false;
		has_sortop = OidIsValid(((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop);
		ReleaseSysCache(tup);

		if (!has_sortop)
			return false;
	}

	return true;
}

/*
 * Arma Finalize Agg -> Gather -> este nodo en modo parcial.
 *
 * El target parcial se obtiene marcando cada Aggref con
 * AGGSPLIT_INITIAL_SERIAL, que es exactamente lo que hace el planner para
 * su propio Partial Agg: mark_partial_aggref() ajusta el tipo de salida al
 * del estado de transición, o a bytea si hay que serializarlo.
 */
static void
dbe_add_parallel_agg_path(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *output_rel, GroupPathExtraData *extra,
						  List *tlist, List *custom_private, int natts)
{
	RelOptInfo *partial_rel;
	PathTarget *partial_target;
	CustomPath *ppath;
	Path	   *gather;
	Path	   *final;
	double		rows;
	int			workers;
	ListCell   *lc;
	List	   *partial_tlist = NIL;

	workers = compute_parallel_worker(input_rel, input_rel->pages, -1,
									  max_parallel_workers_per_gather);
	if (workers <= 0)
		return;

	/* Copia del tlist con los agregados marcados como parciales. */
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) copyObject(lfirst(lc));

		/*
		 * Las columnas del GROUP BY viajan tal cual: no son agregados y el
		 * Finalize las necesita enteras para poder juntar por clave.
		 */
		if (IsA(tle->expr, Aggref))
			mark_partial_aggref((Aggref *) tle->expr, AGGSPLIT_INITIAL_SERIAL);
		partial_tlist = lappend(partial_tlist, tle);
	}

	partial_target = make_pathtarget_from_tlist(partial_tlist);

	/*
	 * El rel padre del path parcial es una copia superficial del rel de
	 * salida con otro target.
	 *
	 * Antes se usaba fetch_upper_rel(UPPERREL_PARTIAL_GROUP_AGG) y se le
	 * pisaba el reltarget — pero ese rel lo comparte el planner con SU
	 * propia agregación parcial, y cambiárselo deja sus paths apuntando a
	 * un target que no corresponde.  Mutar estructuras compartidas del
	 * planner es exactamente el tipo de cosa que revienta lejos del lugar
	 * donde se escribió.
	 */
	partial_rel = makeNode(RelOptInfo);
	memcpy(partial_rel, output_rel, sizeof(RelOptInfo));
	partial_rel->reltarget = partial_target;
	partial_rel->pathlist = NIL;
	partial_rel->partial_pathlist = NIL;
	partial_rel->cheapest_startup_path = NULL;
	partial_rel->cheapest_total_path = NULL;
#if PG_VERSION_NUM < 190000
	/* PG 19 sacó cheapest_unique_path de RelOptInfo. */
	partial_rel->cheapest_unique_path = NULL;
#endif
	partial_rel->cheapest_parameterized_paths = NIL;
	partial_rel->consider_parallel = true;
	partial_rel->rows = (root->parse->groupClause != NIL)
		? estimate_num_groups(root,
							  get_sortgrouplist_exprs(root->parse->groupClause,
													  root->parse->targetList),
							  input_rel->rows, NULL, NULL)
		: 1;

	ppath = makeNode(CustomPath);
	ppath->path.pathtype = T_CustomScan;
	ppath->path.parent = partial_rel;
	ppath->path.pathtarget = partial_target;
	ppath->path.param_info = NULL;
	ppath->path.parallel_aware = true;
	ppath->path.parallel_safe = true;
	ppath->path.parallel_workers = workers;
	ppath->path.rows = (root->parse->groupClause != NIL)
		? Max(1.0, partial_rel->rows) : 1;
	ppath->path.startup_cost = 0;

	/*
	 * El trabajo se reparte entre workers.  El costo se expresa como una
	 * fracción del scan porque este nodo recorre los mismos bytes sin armar
	 * tuplas: medido a 100M filas, 20 ns por fila contra 74.
	 */
	ppath->path.total_cost =
		(input_rel->cheapest_total_path->total_cost * 0.35 +
		 cpu_operator_cost * input_rel->rows * list_length(tlist)) /
		(workers + 0.5);

	ppath->methods = &dbe_agg_path_methods;
	ppath->flags = 0;
	ppath->custom_paths = NIL;
	ppath->custom_private = custom_private;

	rows = partial_rel->rows;
	gather = (Path *) create_gather_path(root, partial_rel, (Path *) ppath,
										 partial_target, NULL, &rows);

	if (root->parse->groupClause != NIL)
	{
		/*
		 * Con GROUP BY cada worker agrupa SUS stripes, así que un grupo
		 * puede salir partido en varios estados parciales.  El Finalize de
		 * arriba los junta por clave — es la misma forma que arma el
		 * planner con Partial/Finalize Agg, solo que el parcial no
		 * construye una tupla por fila.
		 */
		double		ngroups = estimate_num_groups(
			root, get_sortgrouplist_exprs(root->parse->groupClause,
										  root->parse->targetList),
			gather->rows, NULL, NULL);

		final = (Path *) create_agg_path(root, output_rel, gather,
										 output_rel->reltarget,
										 AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
										 root->parse->groupClause, NIL,
										 &extra->agg_final_costs, ngroups);
	}
	else
		final = (Path *) create_agg_path(root, output_rel, gather,
										 output_rel->reltarget,
										 AGG_PLAIN, AGGSPLIT_FINAL_DESERIAL,
										 NIL, NULL,
										 &extra->agg_final_costs, 1);

	add_path(output_rel, final);
}

static void
dbe_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	Query	   *parse = root->parse;
	Path	   *input_path = NULL;
	CustomPath *path;
	List	   *agg_attnums = NIL;
	ListCell   *lc;
	RangeTblEntry *rte;
	Relation	relation;
	bool	   *wanted;
	int			natts;
	int			nwanted = 0;
	List	   *tlist;
	List	   *quals = NIL;
	List	   *prunable = NIL;
	List	   *group_attnums = NIL;
	List	   *group_eqops = NIL;
	bool		grouped;
	bool		hashed = false;
	double		ngroups = 1;
	int			n_distinct = 0;
	bool		all_metadata = false;

	if (prev_create_upper_paths_hook)
		prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

	if (stage != UPPERREL_GROUP_AGG || !dbe_enable_vectorized ||
		!dbe_enable_agg_pushdown)
		return;

	/*
	 * Sobre una única relación base.  Se acepta GROUP BY solo si el scan ya
	 * entrega las filas en ese orden (se comprueba más abajo, contra los
	 * pathkeys del path de entrada).  HAVING, grouping sets, ventanas y
	 * DISTINCT siguen cayendo al plan normal.
	 */
	if (parse->groupingSets != NIL ||
		parse->havingQual != NULL || parse->hasWindowFuncs ||
		parse->distinctClause != NIL || parse->hasTargetSRFs ||
		root->hasHavingQual)
		return;
	grouped = (parse->groupClause != NIL);
	if (!parse->hasAggs)
		return;
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;
	/*
	 * Una sola relación en el FROM.  No alcanza con contar la rtable: desde
	 * PG 17 una query con GROUP BY lleva además un RTE_GROUP, que no es una
	 * relación de la que haya que leer nada.  Contarlo hacía que este nodo
	 * nunca se ofreciera para una query agrupada.
	 */
	{
		int			nrels = 0;

		foreach(lc, root->parse->rtable)
			if (((RangeTblEntry *) lfirst(lc))->rtekind != RTE_GROUP)
				nrels++;
		if (nrels != 1)
			return;
	}

	rte = planner_rt_fetch(input_rel->relid, root);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	/* Mismas razones que en dbe_set_rel_pathlist(): hijos y muestreo. */
	if (rte->inh || rte->tablesample != NULL)
		return;

	/*
	 * El WHERE se evalúa adentro del nodo.
	 *
	 * Las cláusulas NO pueden ir en plan.qual ni en custom_exprs: como este
	 * nodo tiene custom_scan_tlist (los agregados), setrefs.c reescribiría
	 * sus Var contra ese tlist y no los encontraría.  Van en
	 * custom_private, que setrefs.c no toca, y se evalúan contra un slot
	 * con el descriptor de la relación.
	 *
	 * Se descartan las formas que no se pueden evaluar así: cualquier cosa
	 * que mire otra relación, subplanes, y funciones volátiles — a las que
	 * el orden de evaluación sí les importa.
	 */
	quals = extract_actual_clauses(input_rel->baserestrictinfo, false);
	if (quals != NIL)
	{
		Relids		varnos = pull_varnos(root, (Node *) quals);

		if (!bms_equal(varnos, bms_make_singleton(input_rel->relid)))
			return;
		if (contain_subplans((Node *) quals) ||
			contain_volatile_functions((Node *) quals))
			return;
		if (list_length(input_rel->baserestrictinfo) !=
			list_length(quals))
			return;				/* alguna pseudoconstant */
	}

	foreach(lc, input_rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, CustomPath) &&
			strcmp(((CustomPath *) p)->methods->CustomName,
				   "DbengineColumnarScan") == 0)
		{
			input_path = p;
			break;
		}
	}
	if (input_path == NULL)
		return;

	/*
	 * Con GROUP BY, la única forma que este nodo sabe ejecutar es la de
	 * grupos contiguos: se lleva un solo grupo abierto y se lo cierra al
	 * ver una clave distinta.  Eso vale exactamente cuando el scan ya
	 * entrega las filas ordenadas por las columnas de agrupación, que es
	 * lo que dicen los pathkeys del path de entrada (los pone el clustering
	 * de F6; ver dbe_scan_pathkeys en customscan.c).
	 *
	 * Si el orden no está, no se hace nada: agrupar con hash ya lo hace
	 * bien el executor de Postgres y no hay motivo para reimplementarlo.
	 */
	if (grouped)
	{
		/*
		 * Dos formas de agrupar, y la elección es del orden, no del costo:
		 *
		 *   - Si el scan ya entrega las filas ordenadas por las columnas de
		 *     agrupación, los grupos quedan contiguos y alcanza con llevar
		 *     uno abierto.  Sin tabla de hash y con memoria constante.
		 *   - Si no, tabla de hash.  Aplica siempre, a cambio de pagar el
		 *     hash por fila y de tener que acotar la memoria.
		 */
		hashed = !pathkeys_contained_in(root->group_pathkeys,
										input_path->pathkeys);

	}

	/*
	 * El target list puede traer agregados y las columnas por las que se
	 * agrupa.  Cualquier otra cosa —una expresión sobre la columna de
	 * agrupación, por ejemplo— cae al plan normal.
	 */
	tlist = make_tlist_from_pathtarget(output_rel->reltarget);
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		AttrNumber	attnum;

		if (IsA(tle->expr, Aggref))
		{
			bool		is_distinct;

			if (!dbe_agg_supported((Aggref *) tle->expr, input_rel->relid,
								   &attnum, grouped, &is_distinct))
				return;
			agg_attnums = lappend_int(agg_attnums, attnum);
			if (is_distinct)
				n_distinct++;
			continue;
		}

		if (grouped && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;
			SortGroupClause *sgc;

			if (var->varno != input_rel->relid || var->varattno < 1 ||
				var->varlevelsup != 0)
				return;

			/*
			 * Tiene que ser una de las columnas del GROUP BY, no una
			 * columna suelta: si no está en el groupClause, su valor no es
			 * constante dentro del grupo y elegir uno cualquiera sería
			 * inventar.  Postgres no deja escribirlo, pero el chequeo es
			 * barato y acá el que se equivoca soy yo, no el usuario.
			 */
			sgc = get_sortgroupref_clause_noerr(tle->ressortgroupref,
												parse->groupClause);
			if (sgc == NULL || !OidIsValid(sgc->eqop))
				return;

			agg_attnums = lappend_int(agg_attnums, var->varattno);
			group_attnums = lappend_int(group_attnums, var->varattno);

			/*
			 * El operador de igualdad sale del GROUP BY, no del tipo: es el
			 * que define qué filas son el mismo grupo, y usar otro sería
			 * agrupar con una semántica distinta de la que pidió la query.
			 */
			group_eqops = lappend_oid(group_eqops, sgc->eqop);
			continue;
		}

		return;					/* expresión que este nodo no sabe armar */
	}
	if (agg_attnums == NIL)
		return;

	/*
	 * Todas las columnas del GROUP BY tienen que estar en la salida.  Si
	 * alguna no está —`SELECT count(*) ... GROUP BY k`— el nodo igual
	 * sabría agrupar, pero no hay dónde comprobarlo contra el plan normal,
	 * así que se deja para el executor de Postgres.
	 */
	if (grouped && list_length(group_attnums) != list_length(parse->groupClause))
		return;

	/*
	 * La versión con hash no vuelca a disco, así que no se ofrece si la
	 * tabla no entra en work_mem.  nodeAgg sí sabe volcar: cuando la
	 * estimación no da, el plan normal es el correcto y no hay por qué
	 * competirle con algo que se va a comer la memoria.
	 *
	 * La estimación es grosera a propósito —la clave del grupo más el
	 * estado de cada agregado, redondeado para arriba— porque errar por
	 * poco no cambia la decisión y errar por mucho ya lo cubre el margen.
	 */
	if (grouped)
	{
		List	   *gexprs = get_sortgrouplist_exprs(parse->groupClause,
													parse->targetList);

		/*
		 * output_rel->rows todavía es 0 acá: el hook corre antes de que el
		 * planner arme sus propios paths de agregación, que es donde se
		 * llena.  La estimación hay que pedirla, no leerla.
		 */
		ngroups = estimate_num_groups(root, gexprs, input_path->rows,
									  NULL, NULL);

		if (hashed)
		{
			Size		per_group = sizeof(MinimalTupleData) + 64 +
				sizeof(Datum) * 4 * list_length(agg_attnums);

			if (ngroups <= 0 || ngroups * (double) per_group >
				(double) work_mem * 1024.0)
				return;
		}
	}

	relation = table_open(rte->relid, NoLock);
	if (!dbe_is_columnar_relation(relation))
	{
		table_close(relation, NoLock);
		return;
	}
	natts = RelationGetDescr(relation)->natts;
	table_close(relation, NoLock);

	/* Columnas que hay que leer: las de los agregados más las de los quals. */
	wanted = (bool *) palloc0(sizeof(bool) * natts);
	foreach(lc, agg_attnums)
	{
		int			a = lfirst_int(lc);

		if (a >= 1 && a <= natts && !wanted[a - 1])
		{
			wanted[a - 1] = true;
			nwanted++;
		}
	}
	{
		Bitmapset  *attrs = NULL;
		int			x = -1;

		foreach(lc, input_rel->baserestrictinfo)
			pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause,
						   input_rel->relid, &attrs);
		while ((x = bms_next_member(attrs, x)) >= 0)
		{
			AttrNumber	a = x + FirstLowInvalidHeapAttributeNumber;

			if (a == InvalidAttrNumber || a < 0)
			{
				pfree(wanted);
				return;			/* fila entera o columna de sistema */
			}
			if (a <= natts && !wanted[a - 1])
			{
				wanted[a - 1] = true;
				nwanted++;
			}
		}
	}

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = output_rel;
	path->path.pathtarget = output_rel->reltarget;
	path->path.param_info = NULL;
	path->path.parallel_aware = false;

	/*
	 * parallel_safe no es "corre en paralelo" sino "se puede ejecutar dentro
	 * de un worker".  Este nodo hace lo mismo que un Seq Scan no paralelo,
	 * que también es parallel_safe.
	 *
	 * Dejarlo en false tenía una consecuencia que no se ve: add_path()
	 * compara costos con 1% de tolerancia, y cuando dos paths empatan
	 * dentro de esa banda desempata por parallel_safe ANTES que por costo.
	 * O sea que el nodo se descartaba en silencio cada vez que el plan
	 * normal costaba parecido — justo el caso de un WHERE selectivo, donde
	 * la agregación pesa poco al lado del scan.
	 */
	path->path.parallel_safe = output_rel->consider_parallel &&
		input_path->parallel_safe;
	path->path.parallel_workers = 0;

	/*
	 * Sin agrupar sale una fila.  Agrupando, la estimación del planner para
	 * este mismo upper rel ya tiene en cuenta la cardinalidad de las
	 * columnas del GROUP BY; usarla evita inventar una propia que compita
	 * mal contra el plan normal.
	 */
	path->path.rows = grouped ? ngroups : 1;

	/*
	 * La salida sale en el orden del grupo, que es el del scan.  Decirlo
	 * ahorra el Sort de un ORDER BY sobre la misma columna.
	 */
	path->path.pathkeys = grouped ? input_path->pathkeys : NIL;

	/*
	 * Si son todos count(*), no hay que leer nada: el costo es recorrer la
	 * lista de stripes.  Si no, se recorre la relación una vez pero sin
	 * construir tuplas, que es de donde sale la ganancia.
	 */
	{
		bool		all_count_star = true;

		foreach(lc, agg_attnums)
			if (lfirst_int(lc) != InvalidAttrNumber)
				all_count_star = false;

		/*
		 * Con WHERE hay que mirar las filas: el row_count y el zone map del
		 * stripe describen TODAS sus filas, no las que pasan el filtro.
		 */
		/*
		 * Con GROUP BY nunca se contesta desde metadatos: el zone map
		 * describe el stripe entero, no cada grupo.  El chequeo va primero
		 * porque dbe_all_minmax() recorre el tlist suponiendo que todo es
		 * Aggref, y con una columna de agrupación adentro decía que sí —
		 * dejando el costo en 0,2 y haciendo que este nodo le ganara a
		 * cualquier plan, incluso a uno 2,3× más rápido.
		 */
		all_metadata = !grouped && n_distinct == 0 && (quals == NIL) &&
			(all_count_star || dbe_all_minmax(tlist));

		if (all_metadata)
		{
			path->path.startup_cost = 0;
			path->path.total_cost = cpu_operator_cost * 100;
		}
		else if (grouped)
		{
			/*
			 * Las dos formas de agrupar no cuestan lo mismo, y darles el
			 * mismo costo hacía que la barata perdiera contra el plan
			 * normal:
			 *
			 *   - Con los grupos contiguos alcanza una comparación por
			 *     fila y no hay tabla de hash.  Medido: 165 ms contra los
			 *     610 ms del GroupAggregate de Postgres sobre lo mismo.
			 *   - Con hash hay una búsqueda por fila.  Medido: entre 1,08×
			 *     y 1,23× contra el HashAggregate paralelo, o sea casi
			 *     igual.
			 */
			double		in_rows = input_path->rows;
			double		nagg = (double) list_length(agg_attnums);
			double		ngcols = (double) list_length(group_attnums);
			double		por_fila;

			if (hashed)
				por_fila = cpu_operator_cost * (nagg + ngcols) * 0.85;
			else
				por_fila = cpu_operator_cost * (nagg + ngcols) * 0.25;

			path->path.startup_cost = input_path->total_cost +
				por_fila * in_rows;
			path->path.total_cost = path->path.startup_cost +
				cpu_tuple_cost * ngroups;
		}
		else
		{
			path->path.startup_cost = input_path->total_cost;
			path->path.total_cost = input_path->total_cost;
		}
	}
	path->methods = &dbe_agg_path_methods;
	path->flags = 0;
	path->custom_paths = NIL;

	{
		List	   *attnums = NIL;
		int			i;

		for (i = 0; i < natts; i++)
			if (wanted[i])
				attnums = lappend_int(attnums, i + 1);

		/*
		 * El relid viaja acá y no en custom_paths: la versión paralela no
		 * tiene un plan hijo del que sacarlo, y este nodo no ejecuta
		 * ninguno — hace el scan él mismo.
		 */
		foreach(lc, quals)
			if (dbe_filter_usable((Expr *) lfirst(lc), input_rel->relid))
				prunable = lappend(prunable, lfirst(lc));

		path->custom_private = lappend(
			lappend(list_make5(attnums, agg_attnums,
							   makeInteger(input_rel->relid), quals, prunable),
					group_eqops),
			makeInteger(hashed ? 1 : 0));
	}

	/*
	 * add_path() libera el path si lo descarta, así que todo lo que se
	 * necesite después hay que sacarlo antes.
	 */
	{
		List	   *private = path->custom_private;

		/*
		 * El camino SERIAL con tabla de hash no se ofrece.
		 *
		 * Es el único que no lleva un Finalize arriba, así que no puede
		 * emitir la misma clave dos veces y por lo tanto no puede volcar
		 * cuando se queda sin memoria.  Con una estimación de grupos mala
		 * —en ClickBench el planner dice 1.529 donde hay 835.092— eso no
		 * degrada, revienta: la misma query pasó de 1,2 s a 19 minutos.
		 *
		 * El camino paralelo cubre lo mismo y sí puede volcar.  Si no hay
		 * paralelismo disponible, agrupa el HashAggregate de Postgres, que
		 * vuelca a disco desde PG 13.
		 */
		if (!(grouped && hashed))
			add_path(output_rel, (Path *) path);

		/*
		 * El camino paralelo no sabe agrupar: cada worker ve un subconjunto
		 * de stripes, así que un grupo puede quedar partido entre varios y
		 * habría que emitir estados de transición parciales para que un
		 * Finalize los combine.  Con GROUP BY se ofrece solo el nodo serial
		 * y el planner elige contra el plan paralelo normal.
		 */
		/*
		 * count(DISTINCT) no se puede partir entre workers: combinar dos
		 * conteos parciales daría de más, y combinar los conjuntos es otra
		 * cosa que este nodo no emite.
		 */
		if (n_distinct == 0 && !all_metadata && input_rel->consider_parallel &&
			output_rel->consider_parallel && extra != NULL &&
			((GroupPathExtraData *) extra)->partial_costs_set)
			dbe_add_parallel_agg_path(root, input_rel, output_rel,
									  (GroupPathExtraData *) extra,
									  tlist, private, natts);
	}
}

static Plan *
dbe_agg_plan_path(PlannerInfo *root, RelOptInfo *rel,
				  struct CustomPath *best_path, List *tlist,
				  List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Index		scanrelid = (Index) intVal(lthird(best_path->custom_private));

	/*
	 * Los Aggref no pueden quedar en el targetlist del plan: el executor los
	 * rechaza fuera de un nodo Agg.  La salida se describe en
	 * custom_scan_tlist y setrefs.c se encarga de reemplazar cada expresión
	 * del targetlist por un Var sobre INDEX_VAR que apunta ahí.
	 */
	cscan->scan.plan.targetlist = tlist;
	cscan->custom_scan_tlist = copyObject(tlist);

	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = scanrelid;
	cscan->flags = best_path->flags;
	cscan->methods = &dbe_agg_scan_methods;
	cscan->custom_private = best_path->custom_private;
	cscan->custom_exprs = NIL;

	return &cscan->scan.plan;
}

/* ------------------------------------------------------------------------
 * Ejecución
 * ------------------------------------------------------------------------ */

static Node *
dbe_agg_create_state(CustomScan *cscan)
{
	DbeVectorAggState *state = (DbeVectorAggState *)
		newNode(sizeof(DbeVectorAggState), T_CustomScanState);

	state->css.methods = &dbe_agg_exec_methods;
	return (Node *) state;
}

/*
 * Prepara un agregado copiando lo que dice pg_aggregate.
 *
 * La inicialización es la misma que hace nodeAgg: si hay agginitval se usa;
 * si no y la transición es strict, el estado arranca "vacío" y el primer
 * valor no nulo pasa a ser el estado inicial.  De ahí sale, por ejemplo,
 * que sum() sobre cero filas dé NULL y count() dé 0.
 */
static void
dbe_agg_init_one(DbeAggState *agg, Aggref *ref, AttrNumber attnum,
				 TupleDesc desc)
{
	HeapTuple	tup;
	Form_pg_aggregate aggform;
	Datum		textInitVal;
	bool		initnull;
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			nargs;
	Expr	   *transfnexpr;

	memset(agg, 0, sizeof(*agg));
	agg->attnum = attnum;
	agg->is_count_star = ref->aggstar;
	agg->collation = ref->inputcollid;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "dbengine: no se encontró el agregado %u", ref->aggfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);

	/*
	 * Igual que nodeAgg: el tipo del estado se resuelve contra los tipos de
	 * entrada (un agregado polimórfico declara anyarray o anyelement), y las
	 * funciones reciben una expresión con esos tipos en fn_expr.  Sin ella,
	 * las polimórficas —range_agg, array_agg sobre ciertos tipos— no pueden
	 * preguntar con get_fn_expr_argtype() qué les llegó, y fallan.
	 */
	nargs = get_aggregate_argtypes(ref, inputTypes);
	agg->transtype = resolve_aggregate_transtype(ref->aggfnoid,
												 aggform->aggtranstype,
												 inputTypes, nargs);
	get_typlenbyval(agg->transtype, &agg->transtyplen, &agg->transtypbyval);

	build_aggregate_transfn_expr(inputTypes, nargs, 0, ref->aggvariadic,
								 agg->transtype, ref->inputcollid,
								 aggform->aggtransfn, InvalidOid,
								 &transfnexpr, NULL);
	fmgr_info(aggform->aggtransfn, &agg->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &agg->transfn);
	agg->transfn_strict = func_strict(aggform->aggtransfn);

	if (OidIsValid(aggform->aggfinalfn))
	{
		Expr	   *finalfnexpr;

		/* Con aggfinalextra la final recibe además un NULL por argumento. */
		agg->finalfn_nargs = (aggform->aggfinalextra ? nargs + 1 : 1);
		build_aggregate_finalfn_expr(inputTypes, agg->finalfn_nargs,
									 agg->transtype, ref->aggtype,
									 ref->inputcollid, aggform->aggfinalfn,
									 &finalfnexpr);
		fmgr_info(aggform->aggfinalfn, &agg->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &agg->finalfn);
		agg->has_finalfn = true;
	}

	agg->skip_final = DO_AGGSPLIT_SKIPFINAL(ref->aggsplit);
	if (DO_AGGSPLIT_SERIALIZE(ref->aggsplit) &&
		OidIsValid(aggform->aggserialfn))
	{
		fmgr_info(aggform->aggserialfn, &agg->serialfn);
		agg->serialize = true;
	}

	textInitVal = SysCacheGetAttr(AGGFNOID, tup,
								  Anum_pg_aggregate_agginitval, &initnull);
	if (initnull)
	{
		agg->transvalue = (Datum) 0;
		agg->transnull = true;
		agg->nothing_yet = agg->transfn_strict;
	}
	else
	{
		Oid			typinput;
		Oid			typioparam;

		getTypeInputInfo(agg->transtype, &typinput, &typioparam);
		agg->transvalue = OidInputFunctionCall(typinput,
											   TextDatumGetCString(textInitVal),
											   typioparam, -1);
		agg->transnull = false;
		agg->nothing_yet = false;
	}

	ReleaseSysCache(tup);

	if (attnum != InvalidAttrNumber)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);

		agg->inputtype = att->atttypid;
		agg->inputtyplen = att->attlen;
		agg->inputtypbyval = att->attbyval;
	}
}

/*
 * Marca el agregado como MIN o MAX si lo es, y deja lista la comparación.
 * Se hace aparte de dbe_agg_init_one() porque necesita volver a mirar
 * pg_aggregate y puede fallar sin que eso sea un problema: si no se puede,
 * simplemente se agrega recorriendo las filas.
 */
static void
dbe_agg_detect_minmax(DbeAggState *agg, Oid aggfnoid)
{
	HeapTuple	tup;
	Form_pg_aggregate aggform;
	Oid			sortop;
	TypeCacheEntry *tc;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	agg->is_minmax = false;

	if (agg->is_count_star || agg->attnum == InvalidAttrNumber)
		return;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		return;
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);
	sortop = aggform->aggsortop;
	ReleaseSysCache(tup);

	if (!OidIsValid(sortop))
		return;

	tc = lookup_type_cache(agg->inputtype,
						   TYPECACHE_BTREE_OPFAMILY | TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(tc->btree_opf) || tc->cmp_proc_finfo.fn_oid == InvalidOid)
		return;
	if (!op_in_opfamily(sortop, tc->btree_opf))
		return;

	get_op_opfamily_properties(sortop, tc->btree_opf, false,
							   &strategy, &lefttype, &righttype);
	if (lefttype != agg->inputtype || righttype != agg->inputtype)
		return;

	if (strategy == BTLessStrategyNumber)
		agg->want_min = true;
	else if (strategy == BTGreaterStrategyNumber)
		agg->want_min = false;
	else
		return;

	fmgr_info_copy(&agg->minmax_cmp, &tc->cmp_proc_finfo, CurrentMemoryContext);
	agg->is_minmax = true;
}

static void
dbe_agg_advance(DbeAggState *agg, Datum value, bool isnull, MemoryContext cxt,
				AggState *aggctx)
{
	FunctionCallInfoBaseData *fcinfo;
	LOCAL_FCINFO(fcinfo_data, 2);
	Datum		newval;
	MemoryContext old;

	if (agg->is_count_star)
	{
		agg->count++;
		return;
	}

	/* Transición strict: los nulos de entrada no cuentan. */
	if (agg->transfn_strict && isnull)
		return;

	if (agg->nothing_yet)
	{
		/*
		 * Primer valor no nulo: pasa a ser el estado, sin llamar a la
		 * transición.  Es lo que hace nodeAgg y es lo que hace que
		 * sum() sobre cero filas dé NULL.
		 */
		old = MemoryContextSwitchTo(cxt);
		agg->transvalue = datumCopy(value, agg->inputtypbyval, agg->inputtyplen);
		MemoryContextSwitchTo(old);
		agg->transnull = false;
		agg->nothing_yet = false;
		return;
	}

	if (agg->transfn_strict && agg->transnull)
		return;

	fcinfo = fcinfo_data;
	InitFunctionCallInfoData(*fcinfo, &agg->transfn, 2, agg->collation,
							 (Node *) aggctx, NULL);
	fcinfo->args[0].value = agg->transvalue;
	fcinfo->args[0].isnull = agg->transnull;
	fcinfo->args[1].value = value;
	fcinfo->args[1].isnull = isnull;
	fcinfo->isnull = false;

	old = MemoryContextSwitchTo(cxt);
	newval = FunctionCallInvoke(fcinfo);

	/*
	 * Las transiciones que trabajan sobre un estado interno devuelven el
	 * mismo puntero; copiar solo cuando cambia evita duplicar sin sentido.
	 */
	if (!agg->transtypbyval && !fcinfo->isnull &&
		DatumGetPointer(newval) != DatumGetPointer(agg->transvalue))
		newval = datumCopy(newval, agg->transtypbyval, agg->transtyplen);

	MemoryContextSwitchTo(old);

	agg->transvalue = newval;
	agg->transnull = fcinfo->isnull;
}

static Datum
dbe_agg_finalize(DbeAggState *agg, bool *isnull, AggState *aggctx)
{
	LOCAL_FCINFO(fcinfo, 2);

	/*
	 * count(*) y count(DISTINCT col) llevan el conteo aparte, sin pasar por
	 * la función de transición: el primero porque no hay valor que mirar, el
	 * segundo porque lo que cuenta son los valores nuevos del conjunto.
	 */
	if (agg->is_count_star || agg->is_distinct)
	{
		*isnull = false;
		return Int64GetDatum(agg->count);
	}

	/*
	 * Modo parcial: se entrega el estado de transición, serializado si hace
	 * falta.  El Finalize Agg de arriba lo combina con el de los demás
	 * workers.
	 */
	if (agg->skip_final)
	{
		if (!agg->serialize)
		{
			*isnull = agg->transnull;
			return agg->transvalue;
		}
		if (agg->transnull)
		{
			*isnull = true;
			return (Datum) 0;
		}

		{
			LOCAL_FCINFO(sfc, 1);

			InitFunctionCallInfoData(*sfc, &agg->serialfn, 1,
									 InvalidOid, (Node *) aggctx, NULL);
			sfc->args[0].value = agg->transvalue;
			sfc->args[0].isnull = false;
			sfc->isnull = false;
			*isnull = false;
			return FunctionCallInvoke(sfc);
		}
	}

	if (!agg->has_finalfn)
	{
		*isnull = agg->transnull;
		return agg->transvalue;
	}

	InitFunctionCallInfoData(*fcinfo, &agg->finalfn, agg->finalfn_nargs,
							 agg->collation, (Node *) aggctx, NULL);
	fcinfo->args[0].value = agg->transvalue;
	fcinfo->args[0].isnull = agg->transnull;
	for (int k = 1; k < agg->finalfn_nargs; k++)
	{
		fcinfo->args[k].value = (Datum) 0;
		fcinfo->args[k].isnull = true;
	}
	fcinfo->isnull = false;

	if (agg->transnull && agg->finalfn.fn_strict)
	{
		*isnull = true;
		return (Datum) 0;
	}

	{
		Datum		result = FunctionCallInvoke(fcinfo);

		*isnull = fcinfo->isnull;
		return result;
	}
}

/*
 * Intenta contestar min/max (y count(*)) desde los zone maps.
 * Devuelve false si algún stripe no lo permite; ahí se recorre normal.
 */
static bool
dbe_agg_try_metadata(DbeVectorAggState *state, TupleTableSlot *slot)
{
	int			nstripes = dbe_cursor_nstripes(state->cursor);
	Datum	   *best;
	bool	   *have;
	int			i;
	int			k;
	bool		ok = true;

	best = (Datum *) palloc0(sizeof(Datum) * state->naggs);
	have = (bool *) palloc0(sizeof(bool) * state->naggs);

	for (k = 0; k < nstripes && ok; k++)
	{
		for (i = 0; i < state->naggs && ok; i++)
		{
			DbeAggState *agg = &state->aggs[i];
			DbeSegDesc	seg;
			bool		all_null;
			Datum		v;

			if (agg->is_count_star)
				continue;

			if (!dbe_cursor_stripe_zonemap(state->cursor, k, agg->attnum,
										   &seg, &all_null))
			{
				ok = false;
				break;
			}
			if (all_null)
				continue;

			v = (agg->inputtyplen > 0)
				? fetch_att(agg->want_min ? seg.min_value : seg.max_value,
							agg->inputtypbyval, agg->inputtyplen)
				: PointerGetDatum(agg->want_min ? seg.min_value : seg.max_value);

			if (!have[i])
			{
				best[i] = v;
				have[i] = true;
			}
			else
			{
				int			cmp = DatumGetInt32(
					FunctionCall2Coll(&agg->minmax_cmp, agg->collation,
									  v, best[i]));

				if (agg->want_min ? (cmp < 0) : (cmp > 0))
					best[i] = v;
			}
		}
	}

	if (!ok)
	{
		pfree(best);
		pfree(have);
		return false;
	}

	ExecClearTuple(slot);
	for (i = 0; i < state->naggs; i++)
	{
		if (state->aggs[i].is_count_star)
		{
			slot->tts_values[i] =
				Int64GetDatum((int64) dbe_cursor_count_rows(state->cursor));
			slot->tts_isnull[i] = false;
		}
		else
		{
			slot->tts_values[i] = best[i];
			slot->tts_isnull[i] = !have[i];
		}
	}
	ExecStoreVirtualTuple(slot);

	pfree(best);
	pfree(have);
	return true;
}

static void
dbe_agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *attnums = (List *) linitial(cscan->custom_private);
	List	   *agg_attnums = (List *) lsecond(cscan->custom_private);
	Relation	relation = node->ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(relation);
	ListCell   *lc;
	ListCell   *lc2;
	int			i = 0;

	state->relation = relation;
	state->done = false;
	state->rows_seen = 0;

	state->wanted = (bool *) palloc0(sizeof(bool) * desc->natts);
	foreach(lc, attnums)
	{
		int			a = lfirst_int(lc);

		if (a >= 1 && a <= desc->natts)
			state->wanted[a - 1] = true;
	}

	state->naggs = list_length(cscan->custom_scan_tlist);
	state->aggs = (DbeAggState *) palloc0(sizeof(DbeAggState) * state->naggs);

	state->ngroupcols = 0;
	forboth(lc, cscan->custom_scan_tlist, lc2, agg_attnums)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		DbeAggState *agg = &state->aggs[i++];

		if (!IsA(tle->expr, Aggref))
		{
			/* Columna de agrupación: no agrega, su valor sale de la clave. */
			agg->is_group = true;
			agg->group_attnum = (AttrNumber) lfirst_int(lc2);
			agg->group_idx = state->ngroupcols++;
			continue;
		}

		dbe_agg_init_one(agg, (Aggref *) tle->expr,
						 (AttrNumber) lfirst_int(lc2), desc);

		if (((Aggref *) tle->expr)->aggdistinct != NIL)
		{
			SortGroupClause *sgc =
				(SortGroupClause *) linitial(((Aggref *) tle->expr)->aggdistinct);
			Form_pg_attribute att = TupleDescAttr(desc, agg->attnum - 1);
			TupleDesc	ddesc = CreateTemplateTupleDesc(1);
			AttrNumber	kc = 1;
			Oid			eqop = sgc->eqop;
			Oid		   *eqf;
			FmgrInfo   *hf;
			Oid			coll = att->attcollation;

			TupleDescInitEntry(ddesc, 1, NameStr(att->attname), att->atttypid,
							   att->atttypmod, 0);
			TupleDescAttr(ddesc, 0)->attcollation = coll;

			agg->is_distinct = true;
			agg->dist_slot = MakeSingleTupleTableSlot(ddesc, &TTSOpsVirtual);
			agg->dist_cxt = AllocSetContextCreate(CurrentMemoryContext,
												  "dbengine distinct",
												  ALLOCSET_DEFAULT_SIZES);
			agg->dist_tempcxt = AllocSetContextCreate(CurrentMemoryContext,
													  "dbengine distinct tmp",
													  ALLOCSET_SMALL_SIZES);
			execTuplesHashPrepare(1, &eqop, &eqf, &hf);
			agg->dist_table =
				BuildTupleHashTable(&node->ss.ps, ddesc, &TTSOpsVirtual,
									1, &kc, eqf, hf, &coll, 256, 0,
									CurrentMemoryContext, agg->dist_cxt,
									agg->dist_tempcxt, false);
		}

		/*
		 * El valor inicial se guarda para reiniciar el agregado en cada
		 * grupo.  Vive en el contexto de la query, así que sobrevive a los
		 * resets del contexto de agregación.
		 */
		agg->initvalue = agg->transvalue;
		agg->initnull = agg->transnull;
		agg->init_nothing_yet = agg->nothing_yet;
	}

	if (state->ngroupcols > 0)
	{
		int			g = 0;

		state->groupcols = (AttrNumber *)
			palloc0(sizeof(AttrNumber) * state->ngroupcols);
		state->groupvecs = (DbeColumnVector *)
			palloc0(sizeof(DbeColumnVector) * state->ngroupcols);
		state->groupvec_ok = (bool *) palloc0(sizeof(bool) * state->ngroupcols);
		state->group_typlen = (int16 *) palloc0(sizeof(int16) * state->ngroupcols);
		state->group_typbyval = (bool *) palloc0(sizeof(bool) * state->ngroupcols);
		state->group_eq = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * state->ngroupcols);
		state->group_collation = (Oid *) palloc0(sizeof(Oid) * state->ngroupcols);
		state->cur_group = (Datum *) palloc0(sizeof(Datum) * state->ngroupcols);
		state->cur_group_null = (bool *) palloc0(sizeof(bool) * state->ngroupcols);

		for (i = 0; i < state->naggs; i++)
		{
			DbeAggState *agg = &state->aggs[i];
			Form_pg_attribute att;
			TypeCacheEntry *tc;

			if (!agg->is_group)
				continue;

			g = agg->group_idx;
			state->groupcols[g] = agg->group_attnum;
			att = TupleDescAttr(desc, agg->group_attnum - 1);
			state->group_typlen[g] = att->attlen;
			state->group_typbyval[g] = att->attbyval;
			state->group_collation[g] = att->attcollation;

			/*
			 * La igualdad es la del opclass btree por defecto del tipo, que
			 * es la misma con la que se ordenó.  Tiene que serlo: si el
			 * corte de grupo usara otra noción de igualdad que el orden,
			 * dos filas "iguales" podrían quedar separadas.
			 */
			tc = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(tc->eq_opr_finfo.fn_oid))
				elog(ERROR, "dbengine: el tipo %u no tiene igualdad btree",
					 att->atttypid);
			fmgr_info_copy(&state->group_eq[g], &tc->eq_opr_finfo,
						   CurrentMemoryContext);
		}

		state->group_cxt = AllocSetContextCreate(CurrentMemoryContext,
												 "dbengine group key",
												 ALLOCSET_SMALL_SIZES);
		state->have_group = false;
		state->in_stripe = false;

		state->hashed = (intVal(list_nth(cscan->custom_private, 6)) != 0);
		if (state->hashed)
		{
			List	   *eqops = (List *) list_nth(cscan->custom_private, 5);
			TupleDesc	gdesc = CreateTemplateTupleDesc(state->ngroupcols);
			AttrNumber *keycols = (AttrNumber *)
				palloc(sizeof(AttrNumber) * state->ngroupcols);
			Oid		   *eqarr = (Oid *) palloc(sizeof(Oid) * state->ngroupcols);
			Oid		   *colls = (Oid *) palloc(sizeof(Oid) * state->ngroupcols);
			Oid		   *eqfuncs;
			FmgrInfo   *hashfuncs;
			ListCell   *lc3;
			int			naggs_reales = 0;

			g = 0;
			foreach(lc3, eqops)
				eqarr[g++] = lfirst_oid(lc3);

			for (i = 0; i < state->ngroupcols; i++)
			{
				Form_pg_attribute att =
					TupleDescAttr(desc, state->groupcols[i] - 1);

				TupleDescInitEntry(gdesc, (AttrNumber) (i + 1),
								   NameStr(att->attname), att->atttypid,
								   att->atttypmod, 0);
				TupleDescAttr(gdesc, i)->attcollation = att->attcollation;
				keycols[i] = (AttrNumber) (i + 1);
				colls[i] = att->attcollation;
			}

			for (i = 0; i < state->naggs; i++)
				if (!state->aggs[i].is_group)
					naggs_reales++;

			state->group_slot = MakeSingleTupleTableSlot(gdesc,
														 &TTSOpsVirtual);
			state->group_key_slot =
				MakeSingleTupleTableSlot(gdesc, &TTSOpsMinimalTuple);

			execTuplesHashPrepare(state->ngroupcols, eqarr, &eqfuncs,
								  &hashfuncs);

			/*
			 * Los estados de transición viven acá y no en el contexto por
			 * tupla: tienen que sobrevivir todo el recorrido, porque un
			 * grupo se sigue tocando hasta la última fila.
			 */
			state->hash_aggcxt =
				AllocSetContextCreate(CurrentMemoryContext,
									  "dbengine hash agg", ALLOCSET_DEFAULT_SIZES);
			state->hash_tempcxt =
				AllocSetContextCreate(CurrentMemoryContext,
									  "dbengine hash temp", ALLOCSET_SMALL_SIZES);
			state->dictcxt =
				AllocSetContextCreate(CurrentMemoryContext,
									  "dbengine dict cache", ALLOCSET_SMALL_SIZES);

			state->hashtable =
				BuildTupleHashTable(&node->ss.ps, gdesc, &TTSOpsVirtual,
									state->ngroupcols, keycols, eqfuncs,
									hashfuncs, colls,
									256, sizeof(DbeGroupAggs) * naggs_reales,
									CurrentMemoryContext, state->hash_aggcxt,
									state->hash_tempcxt, false);
			state->hash_filled = false;
			state->hash_more = false;
			state->hash_budget = (Size) work_mem * 1024L;
		}
	}

	state->agg_context = makeNode(AggState);
	state->agg_context->ss.ps.state = estate;
	state->agg_context->curaggcontext = CreateExprContext(estate);

	state->rel_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	i = 0;
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		DbeAggState *agg = &state->aggs[i++];

		if (agg->is_group)
			continue;
		dbe_agg_detect_minmax(agg, ((Aggref *) tle->expr)->aggfnoid);
	}

	state->only_count_star = true;
	state->only_metadata = true;
	for (i = 0; i < state->naggs; i++)
	{
		if (!state->aggs[i].is_count_star)
			state->only_count_star = false;
		if (!state->aggs[i].is_count_star && !state->aggs[i].is_minmax)
			state->only_metadata = false;
	}

	/* WHERE empujado: se compila una vez y se evalúa fila por fila. */
	{
		List	   *quals = (List *) lfourth(cscan->custom_private);
		List	   *prunable = (List *) list_nth(cscan->custom_private, 4);
		Bitmapset  *cols = NULL;
		int			x = -1;

		state->rows_filtered = 0;
		state->filters = dbe_filterset_build(prunable,
											 (Index) intVal(lthird(cscan->custom_private)),
											 desc);

		if (quals != NIL)
		{
			/*
			 * Con parent = NULL el compilador de expresiones no intenta
			 * deducir de qué slot sale cada Var: emite el camino genérico,
			 * que lee ecxt_scantuple.  Es lo mismo que hace
			 * ExecPrepareQual() para los quals que no cuelgan de un nodo.
			 */
			state->qual = ExecPrepareQual(quals, estate);

			foreach(lc, quals)
				pull_varattnos((Node *) lfirst(lc),
							   (Index) intVal(lthird(cscan->custom_private)),
							   &cols);
		}

		state->nqualcols = bms_num_members(cols);
		state->qualcols = (AttrNumber *)
			palloc0(sizeof(AttrNumber) * Max(state->nqualcols, 1));
		state->qualvecs = (DbeColumnVector *)
			palloc0(sizeof(DbeColumnVector) * Max(state->nqualcols, 1));
		state->qualvec_ok = (bool *) palloc0(sizeof(bool) * Max(state->nqualcols, 1));
		state->qual_typlen = (int16 *) palloc0(sizeof(int16) * Max(state->nqualcols, 1));
		state->qual_typbyval = (bool *) palloc0(sizeof(bool) * Max(state->nqualcols, 1));

		i = 0;
		while ((x = bms_next_member(cols, x)) >= 0)
		{
			AttrNumber	a = x + FirstLowInvalidHeapAttributeNumber;
			Form_pg_attribute att;

			/*
			 * La planificación ya descarta la fila entera y las columnas de
			 * sistema, así que esto no debería pasar nunca.  Si pasara, la
			 * alternativa a fallar es evaluar el predicado contra un NULL
			 * inventado y devolver un resultado incorrecto en silencio.
			 */
			if (a < 1 || a > desc->natts)
				elog(ERROR, "dbengine: el predicado empujado referencia la columna %d, fuera de rango",
					 (int) a);

			att = TupleDescAttr(desc, a - 1);
			state->qualcols[i] = a;
			state->qual_typlen[i] = att->attlen;
			state->qual_typbyval[i] = att->attbyval;
			i++;
		}
	}

	state->cursor = dbe_cursor_begin(relation, estate->es_snapshot,
									 state->wanted);

	if (state->filters->nfilters > 0)
		dbe_cursor_set_filter(state->cursor, dbe_filterset_passes,
							  state->filters,
							  dbe_filterset_zone_columns(state->filters,
														  desc->natts));
}

/*
 * Un valor de una columna del stripe.
 *
 * Las columnas de largo fijo sin diccionario tienen arreglo contiguo y se
 * indexan directo.  Las demás (varlena, diccionario, RLE) se piden al
 * lector del segmento, que igual no arma tuplas.
 */
static void
dbe_vec_value(DbeColumnVector *vec, uint64 row, bool typbyval, int16 typlen,
			  Datum *value, bool *isnull)
{
	if (vec->contiguous)
	{
		*isnull = vec->nulls != NULL &&
			(vec->nulls[row / 8] & (1u << (row % 8))) == 0;
		*value = *isnull ? (Datum) 0
			: fetch_att(vec->data + row * vec->width, typbyval, typlen);
		return;
	}

	{
		const void *data;
		uint32		len;
		int32		vnull;
		DbeStatus	st;

		st = dbe_segreader_get(vec->reader, (uint32) row, &data, &len, &vnull);
		dbe_error_if_not_ok(&st, "leyendo un valor de la columna");
		*isnull = (vnull != 0);
		if (*isnull)
			*value = (Datum) 0;
		else if (typlen == -1)
			*value = dbe_varlena_datum(data, vec->attalign, vec->cxt);
		else
			*value = PointerGetDatum(data);
	}
}

/*
 * ¿Pasa esta fila el WHERE?
 *
 * Se arma un slot con SOLO las columnas que el predicado mira — las demás
 * quedan en NULL y nadie las lee — y se evalúa la expresión contra él.  Es
 * una fila por tupla candidata, pero angosta: sin esto habría que
 * reconstruir la fila entera y pasarla por el nodo Agg del executor, que es
 * justamente lo que este nodo evita.
 *
 * Los stripes que el zone map descarta no llegan hasta acá.
 */
static bool
dbe_agg_row_passes(DbeVectorAggState *state, uint64 row)
{
	TupleTableSlot *slot = state->rel_slot;
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	int			i;
	bool		pass;

	/*
	 * El slot se prepara una sola vez.  Es virtual y nadie lo materializa,
	 * así que después alcanza con pisar las columnas del predicado: las
	 * demás quedan en NULL para siempre, que es correcto porque el
	 * predicado no las mira.  Limpiar y rearmar el slot por fila costaba un
	 * memset de natts más dos llamadas, por cada fila candidata.
	 */
	if (TTS_EMPTY(slot))
	{
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		ExecStoreVirtualTuple(slot);
	}

	for (i = 0; i < state->nqualcols; i++)
	{
		AttrNumber	a = state->qualcols[i];

		if (!state->qualvec_ok[i])
			continue;			/* columna agregada después: queda NULL */

		dbe_vec_value(&state->qualvecs[i], row, state->qual_typbyval[i],
					  state->qual_typlen[i], &slot->tts_values[a - 1],
					  &slot->tts_isnull[a - 1]);
	}

	econtext->ecxt_scantuple = slot;
	pass = ExecQual(state->qual, econtext);
	ResetExprContext(econtext);

	return pass;
}

/* ------------------------------------------------------------------------
 * GROUP BY con grupos contiguos
 *
 * El planner solo arma este nodo si el scan entrega las filas ordenadas por
 * las columnas de agrupación, así que alcanza con llevar UN grupo abierto:
 * mientras la clave no cambie se sigue acumulando, y cuando cambia se cierra
 * el grupo, se emite la fila y se arranca el siguiente.  No hay tabla de
 * hash ni un estado por grupo; la memoria no depende de cuántos grupos haya.
 *
 * Como el executor pide una fila por llamada, el recorrido tiene que poder
 * cortarse en medio de un stripe y retomar: eso es lo que guardan in_stripe
 * y cur_row.
 * ------------------------------------------------------------------------ */

/* Vuelve a dejar el agregado como recién inicializado, para el próximo grupo. */
static void
dbe_agg_reset_one(DbeAggState *agg, MemoryContext cxt)
{
	agg->count = 0;
	agg->transnull = agg->initnull;
	agg->nothing_yet = agg->init_nothing_yet;

	if (agg->initnull)
		agg->transvalue = (Datum) 0;
	else
	{
		/*
		 * Copiado, no compartido: una función de transición puede modificar
		 * su estado in situ, y si ese estado fuera el valor inicial lo
		 * rompería para todos los grupos siguientes.
		 */
		MemoryContext old = MemoryContextSwitchTo(cxt);

		agg->transvalue = datumCopy(agg->initvalue, agg->transtypbyval,
									agg->transtyplen);
		MemoryContextSwitchTo(old);
	}
}

/* Suma una fila a los agregados del grupo abierto. */
static void
dbe_agg_accumulate(DbeVectorAggState *state, uint64 row, MemoryContext aggcxt)
{
	int			i;

	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];
		Datum		value;
		bool		isnull;

		if (agg->is_group)
			continue;
		if (agg->is_count_star)
		{
			agg->count++;
			continue;
		}
		if (!agg->has_vector)
			dbe_agg_missing_value(state, agg->attnum, &value, &isnull);
		else
			dbe_vec_value(&agg->vec, row, agg->inputtypbyval, agg->inputtyplen,
						  &value, &isnull);
		dbe_agg_advance(agg, value, isnull, aggcxt, state->agg_context);
	}
}

/*
 * Cierra el grupo abierto en el slot.
 *
 * Se materializa antes de volver: los Datum que devuelve la finalización
 * viven en el contexto de agregación, que se resetea al arrancar el grupo
 * siguiente.  Materializar copia todo adentro del slot y lo desacopla.
 */
static TupleTableSlot *
dbe_agg_emit_group(DbeVectorAggState *state, TupleTableSlot *slot)
{
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];

		if (agg->is_group)
		{
			slot->tts_values[i] = state->cur_group[agg->group_idx];
			slot->tts_isnull[i] = state->cur_group_null[agg->group_idx];
			continue;
		}
		slot->tts_values[i] = dbe_agg_finalize(agg, &slot->tts_isnull[i],
											   state->agg_context);
	}
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return slot;
}

/* Arranca un grupo nuevo con la clave de la fila actual. */
static void
dbe_group_start(DbeVectorAggState *state, const Datum *values,
				const bool *nulls, MemoryContext aggcxt)
{
	MemoryContext old;
	int			i;

	MemoryContextReset(state->group_cxt);
	old = MemoryContextSwitchTo(state->group_cxt);
	for (i = 0; i < state->ngroupcols; i++)
	{
		state->cur_group_null[i] = nulls[i];
		state->cur_group[i] = nulls[i] ? (Datum) 0 :
			datumCopy(values[i], state->group_typbyval[i],
					  state->group_typlen[i]);
	}
	MemoryContextSwitchTo(old);

	MemoryContextReset(aggcxt);
	for (i = 0; i < state->naggs; i++)
		if (!state->aggs[i].is_group)
			dbe_agg_reset_one(&state->aggs[i], aggcxt);

	state->have_group = true;
}

/* ¿La clave de esta fila es la del grupo abierto? */
static bool
dbe_group_same(DbeVectorAggState *state, const Datum *values, const bool *nulls)
{
	int			i;

	for (i = 0; i < state->ngroupcols; i++)
	{
		if (nulls[i] || state->cur_group_null[i])
		{
			/* NULL agrupa con NULL, que es lo que hace GROUP BY. */
			if (nulls[i] != state->cur_group_null[i])
				return false;
			continue;
		}
		if (!DatumGetBool(FunctionCall2Coll(&state->group_eq[i],
											state->group_collation[i],
											state->cur_group[i], values[i])))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------------
 * GROUP BY por tabla de hash
 *
 * La versión de grupos contiguos necesita que la tabla esté clusterizada por
 * la columna de agrupación, y una tabla se ordena por una sola cosa.  Esta
 * aplica siempre, a cambio de pagar un hash por fila.
 *
 * Lo que se ahorra igual es la parte cara del plan normal: armar un
 * TupleTableSlot por fila para pasárselo al nodo de arriba.  Acá el valor se
 * saca del vector de la columna y va directo a la tabla; el único slot que
 * se llena es el de la clave del grupo, que tiene las columnas del GROUP BY
 * y no las 103 de la tabla.
 *
 * No vuelca a disco: el planner solo ofrece este camino si la estimación de
 * grupos entra en work_mem.  Cuando no entra, nodeAgg sí sabe volcar y es el
 * plan correcto.
 * ------------------------------------------------------------------------ */

/*
 * Valor de una columna que no tiene segmento en este stripe.
 *
 * Casi siempre es NULL, pero no cuando la columna se agregó con
 * ALTER TABLE ADD COLUMN ... DEFAULT: desde PG 11 eso no reescribe la tabla,
 * guarda el valor en pg_attribute.attmissingval y espera que el AM lo
 * complete.  El cursor de lectura ya lo hacía; este nodo lee las columnas
 * por su cuenta y no, así que sobre una tabla con una columna agregada así
 * devolvía NULL donde había un valor.
 */
static void
dbe_agg_missing_value(DbeVectorAggState *state, AttrNumber attnum,
					  Datum *value, bool *isnull)
{
	TupleDesc	desc = RelationGetDescr(state->relation);
	Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);

	if (att->atthasmissing && !att->attisdropped)
	{
		*value = getmissingattr(desc, attnum, isnull);
		return;
	}
	*value = (Datum) 0;
	*isnull = true;
}

/* Carga en el agregado el estado del grupo que se está tocando. */
static inline void
dbe_group_load(DbeAggState *agg, const DbeGroupAggs *g)
{
	agg->transvalue = g->transvalue;
	agg->transnull = g->transnull;
	agg->nothing_yet = g->nothing_yet;
	agg->count = g->count;
}

static inline void
dbe_group_save(const DbeAggState *agg, DbeGroupAggs *g)
{
	g->transvalue = agg->transvalue;
	g->transnull = agg->transnull;
	g->nothing_yet = agg->nothing_yet;
	g->count = agg->count;
}

/* Recorre toda la relación llenando la tabla de hash. */
static void
dbe_agg_fill_hash(DbeVectorAggState *state)
{
	MemoryContext aggcxt = state->hash_aggcxt;
	uint64		since_reset = 0;
	DbeGroupAggs **dict_cache = NULL;
	uint32_t	dict_size = 0;
	int			i;

	for (;;)
	{
		DbeStripeTake take;
		uint64		nrows;
		bool		all_visible;
		uint64		row;

		/*
		 * Presupuesto de memoria, comprobado en tiempo de ejecución.
		 *
		 * No alcanza con mirarlo al planificar: en ClickBench, para
		 * `GROUP BY SearchPhrase`, el planner estima 1.529 grupos y hay
		 * 835.092 — 546 veces más.  Con la tabla creciendo sin techo esa
		 * query pasó de 1,2 s a 19 minutos.
		 *
		 * Al pasarse se emite lo acumulado y se sigue con la tabla vacía.
		 * Emitir la misma clave más de una vez es válido porque este nodo
		 * entrega agregados PARCIALES y siempre tiene un Finalize arriba
		 * que los combina; es lo mismo que hace nodeAgg al volcar.
		 *
		 * Va ANTES de pedir el stripe siguiente, no después: cortar con un
		 * stripe ya tomado lo saltea entero, porque al retomar el cursor
		 * avanza al que sigue.  Puesta después, esta guarda perdía filas —
		 * q13 daba 4947 donde heap daba 4948.
		 */
		if (MemoryContextMemAllocated(state->hash_aggcxt, true) >
			state->hash_budget && dbe_agg_is_partial(state))
		{
			state->hash_more = true;
			break;
		}

		take = dbe_agg_take_stripe(state);
		if (take == DBE_TAKE_DONE)
			break;
		if (take == DBE_TAKE_SKIP)
			continue;

		nrows = dbe_cursor_stripe_rows(state->cursor);
		all_visible = dbe_cursor_stripe_fully_visible(state->cursor);

		for (i = 0; i < state->naggs; i++)
		{
			DbeAggState *agg = &state->aggs[i];

			if (agg->is_group)
				continue;
			agg->has_vector = !agg->is_count_star &&
				dbe_cursor_column(state->cursor, agg->attnum, &agg->vec);
		}
		for (i = 0; i < state->ngroupcols; i++)
			state->groupvec_ok[i] =
				dbe_cursor_column(state->cursor, state->groupcols[i],
								  &state->groupvecs[i]);
		for (i = 0; i < state->nqualcols; i++)
			state->qualvec_ok[i] =
				dbe_cursor_column(state->cursor, state->qualcols[i],
								  &state->qualvecs[i]);

		/*
		 * Agrupar por el código del diccionario.
		 *
		 * Cuando se agrupa por UNA columna de texto codificada con
		 * diccionario, el string es el mismo para todas las filas que
		 * comparten código.  En vez de hashearlo una vez por fila se
		 * resuelve una vez por entrada del diccionario y se guarda a qué
		 * grupo fue a parar; después cada fila es leer su código y seguir el
		 * puntero.  Medido sobre ClickBench: en un stripe de 65.536 filas
		 * hay 12.856 URLs distintas y 2.508 frases de búsqueda, o sea entre
		 * 5 y 26 veces menos búsquedas.
		 *
		 * Se cachea el payload del grupo y NO la entrada de la tabla: la
		 * tabla de hash guarda sus entradas en un arreglo que se reubica al
		 * crecer, así que un puntero a entrada deja de valer apenas entra
		 * otro grupo.  El payload cuelga de la MinimalTuple, que no se
		 * mueve.
		 */
		MemoryContextReset(state->dictcxt);
		dict_cache = NULL;
		dict_size = 0;
		if (state->ngroupcols == 1 && state->groupvec_ok[0] &&
			state->groupvecs[0].reader != NULL)
		{
			int32_t		is_dict = 0;
			uint32_t	n = 0;
			DbeStatus	st = dbe_segreader_dict(state->groupvecs[0].reader,
												&is_dict, &n);

			dbe_error_if_not_ok(&st, "consultando el diccionario del segmento");
			if (is_dict && n > 0)
			{
				MemoryContext old_cxt = MemoryContextSwitchTo(state->dictcxt);

				/* La última ranura es la del NULL. */
				dict_size = n;
				dict_cache = (DbeGroupAggs **)
					palloc0(sizeof(DbeGroupAggs *) * (n + 1));
				MemoryContextSwitchTo(old_cxt);
			}
		}

		/*
		 * El paso lo da el cursor: dentro de un stripe que pasó el filtro,
		 * los gránulos que igual no pueden tener filas que pasen se saltean
		 * enteros en vez de recorrerse fila por fila.
		 */
		for (row = dbe_cursor_next_live_row(state->cursor, 0); row < nrows;
			 row = dbe_cursor_next_live_row(state->cursor, row + 1))
		{
			TupleHashEntry entry;
			DbeGroupAggs *groups;
			bool		isnew;
			int			gi = 0;
			uint32_t	slot_idx = 0;
			bool		from_cache = false;

			if (!all_visible &&
				!dbe_cursor_row_is_visible(state->cursor, row))
				continue;
			if (state->qual != NULL && !dbe_agg_row_passes(state, row))
			{
				state->rows_filtered++;
				continue;
			}
			state->rows_seen++;

			if (dict_cache != NULL)
			{
				uint32_t	code;
				int32_t		code_null;
				DbeStatus	st = dbe_segreader_code(state->groupvecs[0].reader,
													(uint32_t) row, &code,
													&code_null);

				dbe_error_if_not_ok(&st, "leyendo el código del diccionario");
				slot_idx = code_null ? dict_size : code;

				if (dict_cache[slot_idx] != NULL)
				{
					groups = dict_cache[slot_idx];
					isnew = false;
					from_cache = true;
				}
			}

			if (!from_cache)
			{
				ExecClearTuple(state->group_slot);
				for (i = 0; i < state->ngroupcols; i++)
				{
					if (!state->groupvec_ok[i])
					{
						state->group_slot->tts_values[i] = (Datum) 0;
						state->group_slot->tts_isnull[i] = true;
						continue;
					}
					dbe_vec_value(&state->groupvecs[i], row,
								  state->group_typbyval[i],
								  state->group_typlen[i],
								  &state->group_slot->tts_values[i],
								  &state->group_slot->tts_isnull[i]);
				}
				ExecStoreVirtualTuple(state->group_slot);

				entry = LookupTupleHashEntry(state->hashtable,
											 state->group_slot, &isnew, NULL);
				groups = (DbeGroupAggs *)
					TupleHashEntryGetAdditional(state->hashtable, entry);

				if (dict_cache != NULL)
					dict_cache[slot_idx] = groups;
			}

			for (i = 0; i < state->naggs; i++)
			{
				DbeAggState *agg = &state->aggs[i];
				Datum		value;
				bool		isnull;

				if (agg->is_group)
					continue;

				if (isnew)
				{
					dbe_agg_reset_one(agg, aggcxt);
					dbe_group_save(agg, &groups[gi]);
				}
				dbe_group_load(agg, &groups[gi]);

				if (agg->is_count_star)
					agg->count++;
				else
				{
					if (!agg->has_vector)
						dbe_agg_missing_value(state, agg->attnum,
											  &value, &isnull);
					else
						dbe_vec_value(&agg->vec, row, agg->inputtypbyval,
									  agg->inputtyplen, &value, &isnull);
					dbe_agg_advance(agg, value, isnull, aggcxt,
									state->agg_context);
				}
				dbe_group_save(agg, &groups[gi]);
				gi++;
			}

			/*
			 * LookupTupleHashEntry() calcula el hash en el tempcxt y no lo
			 * resetea: es del que llama.  Cada 1024 filas alcanza, y hace
			 * que el reset no pese por fila.
			 */
			if (++since_reset >= 1024)
			{
				MemoryContextReset(state->hash_tempcxt);
				since_reset = 0;
			}
		}
	}

	MemoryContextReset(state->hash_tempcxt);
	state->hash_filled = true;
	InitTupleHashIterator(state->hashtable, &state->hash_iter);
}

/* ¿Se puede emitir la misma clave más de una vez? Solo con Finalize arriba. */
/*
 * El próximo stripe que le toca a este participante.
 *
 * En paralelo los stripes se reparten con un contador atómico compartido:
 * cada participante se lleva el siguiente entero y nadie repite.  Un grupo
 * puede quedar partido entre varios participantes; los junta el Finalize de
 * arriba, que recibe estados de transición parciales.
 *
 * Sin este reparto cada worker recorre la tabla ENTERA y los agregados
 * salen multiplicados por la cantidad de participantes.  Pasó exactamente
 * eso en el camino de agrupación ordenada: `count(*)` daba 10M pero el
 * `GROUP BY` sumaba 30M, que es el líder más dos workers.  Por eso el
 * protocolo vive en un solo lugar y no copiado en cada bucle de stripes.
 */
static DbeStripeTake
dbe_agg_take_stripe(DbeVectorAggState *state)
{
	uint64		idx;

	if (state->shared == NULL)
		return dbe_cursor_next_stripe(state->cursor) ? DBE_TAKE_STRIPE
			: DBE_TAKE_DONE;

	idx = pg_atomic_fetch_add_u64(&state->shared->next_stripe, 1);

	if (idx >= (uint64) dbe_cursor_nstripes(state->cursor))
		return DBE_TAKE_DONE;
	if (!dbe_cursor_set_stripe(state->cursor, (int) idx))
		return DBE_TAKE_SKIP;
	if (!dbe_cursor_stripe_has_pending(state->cursor))
		return DBE_TAKE_SKIP;
	return DBE_TAKE_STRIPE;
}

static bool
dbe_agg_is_partial(DbeVectorAggState *state)
{
	int			i;

	for (i = 0; i < state->naggs; i++)
		if (!state->aggs[i].is_group && !state->aggs[i].skip_final)
			return false;
	return true;
}

static TupleTableSlot *
dbe_agg_exec_hashed(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	TupleHashEntry entry;
	DbeGroupAggs *groups;
	int			i;
	int			gi = 0;

	if (state->done)
		return NULL;

	if (!state->hash_filled)
		dbe_agg_fill_hash(state);

	entry = ScanTupleHashTable(state->hashtable, &state->hash_iter);
	if (entry == NULL)
	{
		if (!state->hash_more)
		{
			state->done = true;
			return NULL;
		}

		/* Se cortó por memoria: vaciar y seguir donde quedó. */
		state->hash_more = false;
		state->hash_filled = false;
		ResetTupleHashTable(state->hashtable);
		MemoryContextReset(state->hash_aggcxt);
		dbe_agg_fill_hash(state);

		entry = ScanTupleHashTable(state->hashtable, &state->hash_iter);
		if (entry == NULL)
		{
			state->done = true;
			return NULL;
		}
	}

	/* La clave del grupo quedó guardada como MinimalTuple en la entrada. */
	ExecStoreMinimalTuple(TupleHashEntryGetTuple(entry),
						  state->group_key_slot, false);
	slot_getallattrs(state->group_key_slot);
	groups = (DbeGroupAggs *)
		TupleHashEntryGetAdditional(state->hashtable, entry);

	ExecClearTuple(slot);
	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];

		if (agg->is_group)
		{
			slot->tts_values[i] =
				state->group_key_slot->tts_values[agg->group_idx];
			slot->tts_isnull[i] =
				state->group_key_slot->tts_isnull[agg->group_idx];
			continue;
		}
		dbe_group_load(agg, &groups[gi++]);
		slot->tts_values[i] = dbe_agg_finalize(agg, &slot->tts_isnull[i],
											   state->agg_context);
	}
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return slot;
}

static TupleTableSlot *
dbe_agg_exec_grouped(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	MemoryContext aggcxt =
		state->agg_context->curaggcontext->ecxt_per_tuple_memory;
	Datum	   *values = (Datum *) palloc(sizeof(Datum) *
										  Max(state->ngroupcols, 1));
	bool	   *nulls = (bool *) palloc(sizeof(bool) *
										Max(state->ngroupcols, 1));
	int			i;

	if (state->done)
		return NULL;

	for (;;)
	{
		if (!state->in_stripe)
		{
			DbeStripeTake take = dbe_agg_take_stripe(state);

			if (take == DBE_TAKE_SKIP)
				continue;
			if (take == DBE_TAKE_DONE)
			{
				state->done = true;
				if (state->have_group)
				{
					state->have_group = false;
					return dbe_agg_emit_group(state, slot);
				}
				return NULL;
			}

			state->stripe_nrows = dbe_cursor_stripe_rows(state->cursor);
			state->stripe_all_visible =
				dbe_cursor_stripe_fully_visible(state->cursor);

			for (i = 0; i < state->naggs; i++)
			{
				DbeAggState *agg = &state->aggs[i];

				if (agg->is_group)
					continue;
				agg->has_vector = !agg->is_count_star &&
					dbe_cursor_column(state->cursor, agg->attnum, &agg->vec);
			}
			for (i = 0; i < state->ngroupcols; i++)
				state->groupvec_ok[i] =
					dbe_cursor_column(state->cursor, state->groupcols[i],
									  &state->groupvecs[i]);
			for (i = 0; i < state->nqualcols; i++)
				state->qualvec_ok[i] =
					dbe_cursor_column(state->cursor, state->qualcols[i],
									  &state->qualvecs[i]);

			state->cur_row = dbe_cursor_next_live_row(state->cursor, 0);
			state->in_stripe = true;
		}

		while (state->cur_row < state->stripe_nrows)
		{
			uint64		row = state->cur_row;

			state->cur_row = dbe_cursor_next_live_row(state->cursor, row + 1);

			if (!state->stripe_all_visible &&
				!dbe_cursor_row_is_visible(state->cursor, row))
				continue;

			if (state->qual != NULL && !dbe_agg_row_passes(state, row))
			{
				state->rows_filtered++;
				continue;
			}

			state->rows_seen++;

			for (i = 0; i < state->ngroupcols; i++)
			{
				if (!state->groupvec_ok[i])
				{
					values[i] = (Datum) 0;
					nulls[i] = true;
					continue;
				}
				dbe_vec_value(&state->groupvecs[i], row,
							  state->group_typbyval[i], state->group_typlen[i],
							  &values[i], &nulls[i]);
			}

			if (state->have_group && !dbe_group_same(state, values, nulls))
			{
				TupleTableSlot *out = dbe_agg_emit_group(state, slot);

				dbe_group_start(state, values, nulls, aggcxt);
				dbe_agg_accumulate(state, row, aggcxt);
				return out;
			}

			if (!state->have_group)
				dbe_group_start(state, values, nulls, aggcxt);

			dbe_agg_accumulate(state, row, aggcxt);
		}

		state->in_stripe = false;
	}
}

static TupleTableSlot *
dbe_agg_exec(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	MemoryContext aggcxt =
		state->agg_context->curaggcontext->ecxt_per_tuple_memory;
	int			i;

	if (state->ngroupcols > 0)
		return state->hashed ? dbe_agg_exec_hashed(node)
			: dbe_agg_exec_grouped(node);

	if (state->done)
		return NULL;

	/*
	 * count(*), min y max se pueden contestar desde el catálogo: el
	 * row_count y el zone map de cada stripe alcanzan.  No se lee un solo
	 * byte de datos.
	 *
	 * Si algún stripe no tiene zone map utilizable, o si tiene visibilidad
	 * parcial — donde el zone map abarca filas que este snapshot no ve — se
	 * cae al recorrido normal, que siempre está disponible.
	 */
	if (state->qual == NULL && state->only_metadata &&
		!state->only_count_star &&
		dbe_agg_try_metadata(state, slot))
	{
		state->done = true;
		return slot;
	}

	if (state->only_count_star && state->qual == NULL)
	{
		uint64		n = dbe_cursor_count_rows(state->cursor);

		state->rows_seen = n;
		state->done = true;

		ExecClearTuple(slot);
		for (i = 0; i < state->naggs; i++)
		{
			slot->tts_values[i] = Int64GetDatum((int64) n);
			slot->tts_isnull[i] = false;
		}
		ExecStoreVirtualTuple(slot);
		return slot;
	}

	/*
	 * Recorrido por columna, no por fila.
	 *
	 * De cada stripe se pide el arreglo de la columna UNA vez y se lo
	 * recorre en un lazo propio.  Lo que se saca del medio es lo que
	 * costaba: limpiar y armar un TupleTableSlot por fila, y una llamada
	 * cruzando el airlock por cada valor.  Medido a 100M filas, esa
	 * maquinaria eran 54 ns por fila.
	 *
	 * Las columnas de largo variable y las de diccionario no tienen arreglo
	 * contiguo; ahí se pide valor por valor, pero igual sin armar tuplas.
	 */
	for (;;)
	{
		DbeStripeTake take;
		uint64		nrows;
		bool		all_visible;
		uint64		row;

		take = dbe_agg_take_stripe(state);
		if (take == DBE_TAKE_DONE)
			break;
		if (take == DBE_TAKE_SKIP)
			continue;

		nrows = dbe_cursor_stripe_rows(state->cursor);
		all_visible = dbe_cursor_stripe_fully_visible(state->cursor);

		for (i = 0; i < state->naggs; i++)
		{
			DbeAggState *agg = &state->aggs[i];

			agg->has_vector = !agg->is_count_star &&
				dbe_cursor_column(state->cursor, agg->attnum, &agg->vec);
		}

		for (i = 0; i < state->nqualcols; i++)
			state->qualvec_ok[i] = dbe_cursor_column(state->cursor,
													 state->qualcols[i],
													 &state->qualvecs[i]);

		/*
		 * El paso lo da el cursor: dentro de un stripe que pasó el filtro,
		 * los gránulos que igual no pueden tener filas que pasen se saltean
		 * enteros en vez de recorrerse fila por fila.
		 */
		for (row = dbe_cursor_next_live_row(state->cursor, 0); row < nrows;
			 row = dbe_cursor_next_live_row(state->cursor, row + 1))
		{
			if (!all_visible && !dbe_cursor_row_is_visible(state->cursor, row))
				continue;

			if (state->qual != NULL && !dbe_agg_row_passes(state, row))
			{
				state->rows_filtered++;
				continue;
			}

			state->rows_seen++;

			for (i = 0; i < state->naggs; i++)
			{
				DbeAggState *agg = &state->aggs[i];
				Datum		value;
				bool		isnull;

				if (agg->is_count_star)
				{
					agg->count++;
					continue;
				}
				if (!agg->has_vector)
				{
					dbe_agg_missing_value(state, agg->attnum, &value, &isnull);
					if (agg->is_distinct)
					{
						bool		isnew;

						if (isnull)
							continue;
						ExecClearTuple(agg->dist_slot);
						agg->dist_slot->tts_values[0] = value;
						agg->dist_slot->tts_isnull[0] = false;
						ExecStoreVirtualTuple(agg->dist_slot);
						LookupTupleHashEntry(agg->dist_table, agg->dist_slot,
											 &isnew, NULL);
						if (isnew)
							agg->count++;
						continue;
					}
					dbe_agg_advance(agg, value, isnull, aggcxt,
									state->agg_context);
					continue;
				}

				dbe_vec_value(&agg->vec, row, agg->inputtypbyval,
							  agg->inputtyplen, &value, &isnull);

				if (agg->is_distinct)
				{
					bool		isnew;

					/*
					 * LookupTupleHashEntry() arma el hash en el tempcxt y no
					 * lo resetea; es del que llama.  Cada 1024 filas alcanza.
					 */
					if ((++agg->dist_since_reset & 1023) == 0)
						MemoryContextReset(agg->dist_tempcxt);

					/* count() no cuenta nulos, con DISTINCT tampoco. */
					if (isnull)
						continue;

					ExecClearTuple(agg->dist_slot);
					agg->dist_slot->tts_values[0] = value;
					agg->dist_slot->tts_isnull[0] = false;
					ExecStoreVirtualTuple(agg->dist_slot);

					LookupTupleHashEntry(agg->dist_table, agg->dist_slot,
										 &isnew, NULL);
					if (isnew)
						agg->count++;
					continue;
				}

				dbe_agg_advance(agg, value, isnull, aggcxt, state->agg_context);
			}
		}
	}

	state->done = true;

	ExecClearTuple(slot);
	for (i = 0; i < state->naggs; i++)
		slot->tts_values[i] = dbe_agg_finalize(&state->aggs[i],
											   &slot->tts_isnull[i],
											   state->agg_context);
	ExecStoreVirtualTuple(slot);

	return slot;
}

static void
dbe_agg_end(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;

	if (state->cursor != NULL)
	{
		dbe_cursor_end(state->cursor);
		state->cursor = NULL;
	}
	if (state->rel_slot != NULL)
	{
		ExecDropSingleTupleTableSlot(state->rel_slot);
		state->rel_slot = NULL;
	}
}

static void
dbe_agg_rescan(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *agg_attnums = (List *) lsecond(cscan->custom_private);
	TupleDesc	desc = RelationGetDescr(state->relation);
	ListCell   *lc;
	ListCell   *lc2;
	int			i = 0;

	forboth(lc, cscan->custom_scan_tlist, lc2, agg_attnums)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		DbeAggState *agg = &state->aggs[i++];

		if (agg->is_group)
			continue;

		dbe_agg_init_one(agg, (Aggref *) tle->expr,
						 (AttrNumber) lfirst_int(lc2), desc);
		agg->initvalue = agg->transvalue;
		agg->initnull = agg->transnull;
		agg->init_nothing_yet = agg->nothing_yet;
	}

	/* El recorrido por grupos arranca de cero. */
	state->have_group = false;
	state->in_stripe = false;
	state->cur_row = 0;

	state->done = false;
	state->rows_seen = 0;
	state->rows_filtered = 0;
	state->stripes_pruned = 0;
	state->granules_pruned = 0;
	state->published = false;
	state->totals_ready = false;
	dbe_cursor_rescan(state->cursor);
}

/*
 * En paralelo cada worker se lleva stripes enteros con un fetch-add y
 * agrega los suyos; el Finalize Agg de arriba combina los estados
 * parciales.  Es la misma forma que arma el planner con Partial Agg, solo
 * que sin construir una tupla por fila.
 */
static Size
dbe_agg_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	return sizeof(DbeAggParallelShared);
}

static void
dbe_agg_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					   void *coordinate)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	DbeAggParallelShared *shared = (DbeAggParallelShared *) coordinate;

	pg_atomic_init_u64(&shared->next_stripe, 0);
	pg_atomic_init_u64(&shared->rows_seen, 0);
	pg_atomic_init_u64(&shared->rows_filtered, 0);
	pg_atomic_init_u64(&shared->stripes_pruned, 0);
	pg_atomic_init_u64(&shared->granules_pruned, 0);
	state->shared = shared;
}

/*
 * Cada participante publica sus contadores antes de que el EXPLAIN imprima.
 * Sin esto, un plan paralelo muestra solo lo que vio el líder.
 */
static void
dbe_agg_shutdown(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;

	if (state->shared == NULL || state->published)
		return;
	state->published = true;

	pg_atomic_fetch_add_u64(&state->shared->rows_seen, state->rows_seen);
	pg_atomic_fetch_add_u64(&state->shared->rows_filtered, state->rows_filtered);
	if (state->cursor != NULL)
	{
		pg_atomic_fetch_add_u64(&state->shared->stripes_pruned,
								dbe_cursor_stripes_pruned(state->cursor));
		pg_atomic_fetch_add_u64(&state->shared->granules_pruned,
								dbe_cursor_granules_pruned(state->cursor));
	}

	/* Ver la nota de dbe_shutdown_scan(): el DSM no sobrevive al EXPLAIN. */
	if (!IsParallelWorker())
	{
		state->rows_seen = pg_atomic_read_u64(&state->shared->rows_seen);
		state->rows_filtered = pg_atomic_read_u64(&state->shared->rows_filtered);
		state->stripes_pruned = pg_atomic_read_u64(&state->shared->stripes_pruned);
		state->granules_pruned = pg_atomic_read_u64(&state->shared->granules_pruned);
		state->totals_ready = true;
	}
}

static void
dbe_agg_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
						 void *coordinate)
{
	DbeAggParallelShared *shared = (DbeAggParallelShared *) coordinate;

	pg_atomic_write_u64(&shared->next_stripe, 0);
	pg_atomic_write_u64(&shared->rows_seen, 0);
	pg_atomic_write_u64(&shared->rows_filtered, 0);
	pg_atomic_write_u64(&shared->stripes_pruned, 0);
	pg_atomic_write_u64(&shared->granules_pruned, 0);
}

static void
dbe_agg_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	((DbeVectorAggState *) node)->shared = (DbeAggParallelShared *) coordinate;
}

static void
dbe_agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;

	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *quals = (List *) lfourth(cscan->custom_private);

	ExplainPropertyInteger("Agregados empujados al scan", NULL,
						   state->naggs, es);

	if (quals != NIL)
	{
		List	   *context;
		char	   *str;

		context = set_deparse_context_plan(es->deparse_cxt,
										   node->ss.ps.plan, ancestors);
		str = deparse_expression((Node *) make_ands_explicit(quals), context,
								 true, false);
		ExplainPropertyText("Filtro", str, es);
	}
	if (state->filters != NULL && state->filters->nfilters > 0)
		ExplainPropertyInteger("Predicados sobre zone maps", NULL,
							   state->filters->nfilters, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Filas agregadas", NULL,
							   (int64) state->rows_seen, es);
		if (quals != NIL)
		{
			ExplainPropertyInteger("Filas descartadas por el filtro", NULL,
								   (int64) state->rows_filtered, es);
			if (state->totals_ready || state->cursor != NULL)
			{
				int64		gran;

				ExplainPropertyInteger("Stripes descartados", NULL,
									   (int64) (state->totals_ready
												? state->stripes_pruned
												: dbe_cursor_stripes_pruned(state->cursor)),
									   es);

				gran = (int64) (state->totals_ready
								? state->granules_pruned
								: dbe_cursor_granules_pruned(state->cursor));
				if (gran > 0 || es->format != EXPLAIN_FORMAT_TEXT)
					ExplainPropertyInteger("Granulos descartados", NULL,
										   gran, es);
			}
		}
	}
}
