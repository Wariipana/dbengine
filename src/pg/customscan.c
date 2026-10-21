/*-------------------------------------------------------------------------
 *
 * customscan.c
 *	  Custom Scan sobre el storage columnar (F3).  Ver customscan.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "access/tupmacs.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "access/parallel.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/ruleutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/customscan.h"
#include "dbengine/dbengine.h"
#include "dbengine/workload.h"
#include "dbengine/zonemap.h"

#define DBE_SCAN_NAME "DbengineColumnarScan"

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/*
 * Estado compartido del scan paralelo.  Cada worker se lleva el siguiente
 * stripe entero: es la unidad natural de reparto y evita tener que
 * coordinar nada más.
 */
typedef struct DbeParallelScanShared
{
	pg_atomic_uint64 next_stripe;

	/*
	 * Contadores del EXPLAIN.  Cada participante —los workers y el líder—
	 * suma los suyos acá al terminar.  Sin esto, el EXPLAIN de un plan
	 * paralelo muestra solo lo que vio el líder: con dos workers, "Stripes
	 * descartados: 15" cuando en realidad se descartaron 30.  Es un número
	 * con el que se toman decisiones, así que tiene que estar bien.
	 */
	pg_atomic_uint64 stripes_pruned;
	pg_atomic_uint64 granules_pruned;
	pg_atomic_uint64 rows_filtered;
} DbeParallelScanShared;

typedef struct DbeScanState
{
	CustomScanState css;
	DbeFilterSet *filters;
	bool	   *wanted;			/* indexado por attnum-1; NULL = todas */
	DbeRowCursor *cursor;
	DbeParallelScanShared *shared;	/* NULL si el scan no es paralelo */

	/*
	 * El predicado se evalúa acá adentro, no en plan.qual, para poder
	 * hacerlo ANTES de terminar de armar la fila (late materialization).
	 */
	ExprState  *qual;
	uint64		rows_filtered;
	uint64		stripes_pruned;	/* total del plan, solo si totals_ready */
	uint64		granules_pruned;
	bool		published;		/* ya sumó sus contadores al DSM */
	bool		totals_ready;	/* los totales ya están en local */
} DbeScanState;

static void dbe_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								 Index rti, RangeTblEntry *rte);
static Plan *dbe_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  struct CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *dbe_create_scan_state(CustomScan *cscan);
static void dbe_begin_scan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *dbe_exec_scan(CustomScanState *node);
static void dbe_end_scan(CustomScanState *node);
static void dbe_rescan_scan(CustomScanState *node);
static void dbe_shutdown_scan(CustomScanState *node);
static void dbe_explain_scan(CustomScanState *node, List *ancestors,
							 ExplainState *es);
static Size dbe_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
static void dbe_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
							   void *coordinate);
static void dbe_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								 void *coordinate);
static void dbe_initialize_worker(CustomScanState *node, shm_toc *toc,
								  void *coordinate);

static const CustomPathMethods dbe_path_methods = {
	.CustomName = DBE_SCAN_NAME,
	.PlanCustomPath = dbe_plan_custom_path,
};

static const CustomScanMethods dbe_scan_methods = {
	.CustomName = DBE_SCAN_NAME,
	.CreateCustomScanState = dbe_create_scan_state,
};

static const CustomExecMethods dbe_exec_methods = {
	.CustomName = DBE_SCAN_NAME,
	.BeginCustomScan = dbe_begin_scan,
	.ExecCustomScan = dbe_exec_scan,
	.EndCustomScan = dbe_end_scan,
	.ReScanCustomScan = dbe_rescan_scan,
	.ShutdownCustomScan = dbe_shutdown_scan,
	.EstimateDSMCustomScan = dbe_estimate_dsm,
	.InitializeDSMCustomScan = dbe_initialize_dsm,
	.ReInitializeDSMCustomScan = dbe_reinitialize_dsm,
	.InitializeWorkerCustomScan = dbe_initialize_worker,
	.ExplainCustomScan = dbe_explain_scan,
};

void
dbe_customscan_init(void)
{
	RegisterCustomScanMethods(&dbe_scan_methods);

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = dbe_set_rel_pathlist;
}

/* ------------------------------------------------------------------------
 * Planificación
 * ------------------------------------------------------------------------ */



/*
 * Qué columnas necesita la query: las del target list del scan más las que
 * aparecen en sus predicados.
 *
 * Devuelve false si no se puede acotar — una referencia a la fila entera
 * (`t.*`) o a una columna de sistema que el storage columnar no tiene.  En
 * ese caso no se pierde nada: se cae al seq scan del Table AM, que es
 * correcto y ya funciona.
 */
/*
 * ¿Hay algún Var con varno ROWID_VAR?
 *
 * Es el marcador que el planner usa para la identidad de fila en MERGE y en
 * UPDATE con varias relaciones destino.  Este nodo no la puede producir, y
 * setrefs.c tiene un Assert de que no queden.  Mejor ni intentar.
 */
static bool
dbe_has_rowid_var(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == ROWID_VAR)
		return true;
	return expression_tree_walker(node, dbe_has_rowid_var, context);
}

static bool
dbe_needed_columns(RelOptInfo *rel, int natts, bool **wanted_out, int *nwanted)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;
	bool	   *wanted;
	int			x;

	if (dbe_has_rowid_var((Node *) rel->reltarget->exprs, NULL))
		return false;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, rel->baserestrictinfo)
		pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause,
					   rel->relid, &attrs);

	wanted = (bool *) palloc0(sizeof(bool) * natts);
	*nwanted = 0;

	x = -1;
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	attnum = x + FirstLowInvalidHeapAttributeNumber;

		if (attnum == InvalidAttrNumber)
			return false;		/* fila entera: hacen falta todas */

		if (attnum < 0)
		{
			/*
			 * Columnas de sistema.  ctid y tableoid las produce el scan;
			 * xmin/xmax/cmin/cmax no existen en un storage columnar, así
			 * que ahí ni siquiera conviene intentar.
			 */
			if (attnum == SelfItemPointerAttributeNumber ||
				attnum == TableOidAttributeNumber)
				continue;
			return false;
		}

		if (attnum <= natts && !wanted[attnum - 1])
		{
			wanted[attnum - 1] = true;
			(*nwanted)++;
		}
	}

	*wanted_out = wanted;
	return true;
}

/*
 * Costo del scan.
 *
 * La parte que importa es cuántas páginas hay que leer de verdad, y eso el
 * catálogo interno lo sabe exacto: es la suma de los segmentos de las
 * columnas pedidas, no una fracción inventada del tamaño de la tabla.  Con
 * eso el planner puede comparar de verdad contra el seq scan.
 */
static void
dbe_cost_scan(PlannerInfo *root, RelOptInfo *rel, Relation relation,
			  const bool *wanted, int natts, int nwanted, CustomPath *path)
{
	uint64		bytes = 0;
	uint64		rows = 0;
	double		pages;
	Cost		cpu_per_tuple;

	dbe_catalog_column_bytes(relation->rd_locator.relNumber,
							 GetActiveSnapshot(), wanted, natts,
							 &bytes, &rows);

	pages = ceil((double) bytes / BLCKSZ);

	/*
	 * Un path parametrizado estima sus filas con las cláusulas del outer ya
	 * aplicadas; rel->rows no las tiene en cuenta.
	 */
	path->path.rows = path->path.param_info != NULL
		? path->path.param_info->ppi_rows
		: rel->rows;
	path->path.startup_cost = 0;

	/*
	 * El costo por fila es proporcional a las columnas que se reconstruyen.
	 *
	 * cost_seqscan() cobra cpu_tuple_cost por fila suponiendo un heap, y
	 * para una tabla columnar eso subestima muchísimo: el seq scan del
	 * Table AM decodifica TODAS las columnas (medido: 5x más caro por fila
	 * que este nodo leyendo una sola).  El planner no tiene cómo saberlo.
	 *
	 * Así que este costo se expresa como fracción del de una fila completa:
	 * proporcional a las columnas pedidas, con un piso por el trabajo fijo
	 * de producir la tupla.  Leyendo todas las columnas queda a la par del
	 * seq scan, que es exactamente lo que corresponde — ahí no hay nada que
	 * ganar.
	 */
	{
		int			live = 0;
		int			i;
		TupleDesc	desc = RelationGetDescr(relation);

		for (i = 0; i < natts; i++)
			if (!TupleDescAttr(desc, i)->attisdropped)
				live++;

		cpu_per_tuple = cpu_tuple_cost *
			(0.25 + 0.75 * (live > 0 ? (double) nwanted / live : 1.0));
	}

	path->path.total_cost = seq_page_cost * pages +
		cpu_per_tuple * rel->tuples +
		rel->baserestrictcost.per_tuple * rel->tuples;
}


/*
 * Pathkeys: decirle al planner que las filas ya salen ordenadas.
 *
 * Sirve para que un GROUP BY o un ORDER BY sobre la columna de clustering
 * no necesite un Sort, y para que un LIMIT pueda cortar temprano.  Es la
 * diferencia entre agrupar 10M filas y agrupar hasta juntar 10 grupos.
 *
 * La condición NO es pg_index.indisclustered: ese flag dice con qué índice
 * se corrió CLUSTER la última vez y sobrevive a los INSERT posteriores, que
 * son justamente los que rompen el orden.  La condición es la marca que
 * escribe CLUSTER y borra cualquier escritura (ver dbe_catalog_set_order).
 * indisclustered solo se usa para encontrar el IndexOptInfo, y ni siquiera
 * hace falta mirarlo: la marca guarda el OID del índice.
 *
 * Devuelve NIL si no hay orden garantizado, que es el caso conservador:
 * cuesta un Sort de más, nunca un resultado mal ordenado.
 */
static List *
dbe_scan_pathkeys(PlannerInfo *root, RelOptInfo *rel, Relation relation)
{
	Oid			ordered_by;
	ListCell   *lc;

	ordered_by = dbe_catalog_get_order(relation->rd_locator.relNumber);
	if (!OidIsValid(ordered_by))
		return NIL;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		if (index->indexoid != ordered_by)
			continue;

		/*
		 * build_index_pathkeys() devuelve NIL sola si el AM no define un
		 * orden (sortopfamily == NULL), que es el caso del PGM.
		 */
		return build_index_pathkeys(root, index, ForwardScanDirection);
	}

	return NIL;
}


static void
dbe_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					 RangeTblEntry *rte)
{
	Relation	relation;
	CustomPath *path;
	bool	   *wanted = NULL;
	int			nwanted = 0;
	int			natts;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!dbe_enable_vectorized)
		return;
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	/*
	 * También las particiones y los hijos de herencia.
	 *
	 * Son RELOPT_OTHER_MEMBER_REL, no RELOPT_BASEREL, y quedaban afuera: una
	 * tabla columnar particionada no recibía ningún nodo de F3 y cada
	 * partición se leía con el seq scan del Table AM.  Para el hook no hay
	 * diferencia — el reltarget y el baserestrictinfo del hijo ya vienen
	 * traducidos a SUS números de columna, que es todo lo que se mira.
	 *
	 * El padre particionado no entra por otro lado: su relkind es
	 * RELKIND_PARTITIONED_TABLE y no tiene storage.
	 */
	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;

	relation = table_open(rte->relid, NoLock);

	if (!dbe_is_columnar_relation(relation))
	{
		table_close(relation, NoLock);
		return;
	}

	natts = RelationGetDescr(relation)->natts;

	if (!dbe_needed_columns(rel, natts, &wanted, &nwanted))
	{
		table_close(relation, NoLock);
		return;
	}

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;

	/*
	 * Si la relación tiene referencias laterales, su reltarget puede
	 * contener Var de OTRAS relaciones (el clásico `LEFT JOIN LATERAL
	 * (SELECT t2.q1 FROM t3)`, donde el scan de t3 arrastra t2.q1).  Esos
	 * Var los convierte en Param build_path_tlist(), pero solo si el path
	 * está marcado como parametrizado.  Sin param_info quedan como Var con
	 * el varno equivocado y el executor aborta en tlist_matches_tupdesc().
	 *
	 * Es exactamente lo que hace create_seqscan_path() con
	 * required_outer = rel->lateral_relids.
	 */
	path->path.param_info = get_baserel_parampathinfo(root, rel,
													  rel->lateral_relids);
	path->path.parallel_aware = false;
	path->path.parallel_safe = rel->consider_parallel;
	path->path.parallel_workers = 0;
	path->path.pathkeys = dbe_scan_pathkeys(root, rel, relation);
	path->methods = &dbe_path_methods;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path->custom_paths = NIL;

	/*
	 * custom_private viaja hasta el executor y tiene que ser copiable por
	 * copyObject(), así que va como lista de enteros y no como Bitmapset.
	 */
	{
		List	   *attnums = NIL;
		List	   *prunable = NIL;
		ListCell   *lc2;
		int			i;

		for (i = 0; i < natts; i++)
			if (wanted[i])
				attnums = lappend_int(attnums, i + 1);

		/*
		 * Las cláusulas que sirven para descartar stripes viajan al
		 * executor tal cual.  Siguen ADEMÁS en plan.qual: el pruning es una
		 * optimización, no reemplaza la evaluación del predicado.
		 */
		foreach(lc2, rel->baserestrictinfo)
		{
			Expr	   *clause = ((RestrictInfo *) lfirst(lc2))->clause;
			AttrNumber	att;
			StrategyNumber strat;

			if (!dbe_filter_column(clause, rel->relid, &att, &strat))
				continue;

			prunable = lappend(prunable, clause);

			/*
			 * Canal de feedback de F6.  El planner es la única parte del
			 * sistema que ve por cuál columna se filtra; la compactación lo
			 * necesita para saber por cuál conviene reordenar.
			 */
			dbe_workload_note(relation->rd_locator.relNumber, att,
							  strat == BTEqualStrategyNumber);
		}

		/*
		 * Las columnas del GROUP BY también van al canal.
		 *
		 * Antes solo se anotaban los predicados, porque lo único que
		 * ganaba con el orden físico era el descarte de stripes.  Desde que
		 * el scan declara pathkeys y la agregación se puede empujar, la
		 * columna por la que se agrupa es la que más gana de todas: cambia
		 * un HashAggregate por un recorrido de grupos contiguos.  Sin
		 * anotarla, el consejo de F6 recomendaba reordenar por la columna
		 * de filtro y dejaba eso sobre la mesa.
		 *
		 * Se anota aparte de la igualdad: las dos quieren la misma
		 * reorganización física, pero un filtro de Bloom solo sirve para
		 * el filtro.  Contarlas juntas hacía construir filtros sobre
		 * columnas de texto anchas por las que nadie filtra nunca.
		 */
		foreach(lc2, root->parse->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc2);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   root->parse->targetList);

			if (tle != NULL && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				if (var->varno == rel->relid && var->varattno >= 1 &&
					var->varlevelsup == 0)
					dbe_workload_note_group(relation->rd_locator.relNumber,
											var->varattno);
			}
		}

		/*
		 * Las columnas viajan en custom_private (setrefs.c no lo toca) y
		 * los predicados podables en custom_exprs, que sí recibe el mismo
		 * rtoffset que scanrelid.  Guardarlos en custom_private funcionaba
		 * solo mientras el rtoffset fuera 0: con más de un nivel de
		 * subconsulta, los varno dejaban de coincidir con scanrelid y el
		 * descarte se apagaba en silencio.
		 */
		path->custom_private = list_make2(attnums, prunable);
	}

	dbe_cost_scan(root, rel, relation, wanted, natts, nwanted, path);

	/*
	 * El path parcial se arma ANTES de add_path().
	 *
	 * add_path() libera el path que descarta, así que copiarlo después es
	 * leer memoria liberada.  El síntoma era un segfault en planning que
	 * solo aparecía con paralelismo habilitado — o sea, nunca en single-user
	 * y de forma intermitente en el resto.
	 */
	{
		CustomPath *ppath = NULL;

		if (rel->consider_parallel && rel->lateral_relids == NULL)
		{
			/*
			 * Para decidir si vale la pena paralelizar se usa el tamaño de
			 * la TABLA, no el de las columnas que se van a leer: el trabajo
			 * que se reparte es reconstruir las filas, y eso escala con la
			 * tabla.  Con los bytes leídos, una query que pide una sola
			 * columna angosta nunca llegaría al umbral y perdería contra el
			 * Parallel Seq Scan.
			 */
			int			workers = compute_parallel_worker(rel, rel->pages, -1,
														  max_parallel_workers_per_gather);

			if (workers > 0)
			{
				ppath = makeNode(CustomPath);
				memcpy(ppath, path, sizeof(CustomPath));

				/*
				 * El memcpy se lleva también los pathkeys, y está bien:
				 * cada worker toma stripes con un fetch_add sobre el mismo
				 * contador, así que la secuencia que ve CADA UNO es
				 * creciente y su salida sale ordenada.  Es lo que necesita
				 * Gather Merge para intercalarlas sin romper el orden; el
				 * Gather pelado no declara pathkeys, así que tampoco miente.
				 */
				ppath->path.parallel_aware = true;
				ppath->path.parallel_workers = workers;
				ppath->path.rows = rel->rows / (workers + 1);
				ppath->path.total_cost = path->path.startup_cost +
					(path->path.total_cost - path->path.startup_cost) /
					(workers + 0.5);
			}
		}

		add_path(rel, (Path *) path);

		if (ppath != NULL)
			add_partial_path(rel, (Path *) ppath);
	}

	table_close(relation, NoLock);
}

static Plan *
dbe_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
					 struct CustomPath *best_path, List *tlist,
					 List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;

	/*
	 * El predicado NO queda en plan.qual: lo evalúa el nodo, entre llenar
	 * las columnas que el predicado mira y llenar las demás.  Va en
	 * custom_exprs porque es lo único que setrefs.c ajusta igual que
	 * scanrelid (rtoffset) y donde create_customscan_plan() reemplaza los
	 * Var del outer por Params cuando el path está parametrizado.
	 */
	cscan->scan.plan.qual = NIL;
	cscan->custom_exprs = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->methods = &dbe_scan_methods;
	cscan->custom_private = list_make1(linitial(best_path->custom_private));
	cscan->custom_scan_tlist = NIL;

	return &cscan->scan.plan;
}

/* ------------------------------------------------------------------------
 * Ejecución
 * ------------------------------------------------------------------------ */

static Node *
dbe_create_scan_state(CustomScan *cscan)
{
	DbeScanState *state = (DbeScanState *) newNode(sizeof(DbeScanState),
												   T_CustomScanState);

	state->css.methods = &dbe_exec_methods;
	return (Node *) state;
}




static void
dbe_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
	DbeScanState *state = (DbeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	relation = node->ss.ss_currentRelation;
	int			natts = RelationGetDescr(relation)->natts;
	List	   *attnums = (List *) linitial(cscan->custom_private);
	bool	   *filter_cols = NULL;
	ListCell   *lc;

	state->wanted = (bool *) palloc0(sizeof(bool) * natts);
	foreach(lc, attnums)
	{
		int			attnum = lfirst_int(lc);

		if (attnum >= 1 && attnum <= natts)
			state->wanted[attnum - 1] = true;
	}

	/* Predicados podables: se traducen a comparaciones contra el zone map. */
	state->filters = dbe_filterset_build(cscan->custom_exprs,
										 cscan->scan.scanrelid,
										 RelationGetDescr(relation));

	/*
	 * El predicado se compila contra este mismo nodo: sus Var apuntan a la
	 * relación que estamos escaneando, que es exactamente lo que describe
	 * el scan tuple slot.
	 */
	state->rows_filtered = 0;
	if (cscan->custom_exprs != NIL)
	{
		Bitmapset  *cols = NULL;
		int			x = -1;

		state->qual = ExecInitQual(cscan->custom_exprs, &node->ss.ps);

		filter_cols = (bool *) palloc0(sizeof(bool) * natts);
		foreach(lc, cscan->custom_exprs)
			pull_varattnos((Node *) lfirst(lc), cscan->scan.scanrelid, &cols);

		while ((x = bms_next_member(cols, x)) >= 0)
		{
			AttrNumber	a = x + FirstLowInvalidHeapAttributeNumber;

			if (a >= 1 && a <= natts)
			{
				filter_cols[a - 1] = true;

				/*
				 * Una columna que el predicado mira tiene que leerse sí o
				 * sí.  Debería estar ya en `wanted` — el planner mete en el
				 * reltarget los Var de las cláusulas de join, y de ahí sale
				 * la lista — pero si alguna vez no estuviera, el predicado
				 * la leería como NULL y el resultado sería incorrecto en
				 * silencio.  Sale más barato asegurarlo que confiar.
				 */
				state->wanted[a - 1] = true;
			}
		}
	}

	state->cursor = dbe_cursor_begin(relation, estate->es_snapshot,
									 state->wanted);

	if (state->filters->nfilters > 0)
		dbe_cursor_set_filter(state->cursor, dbe_filterset_passes,
							  state->filters,
							  dbe_filterset_zone_columns(state->filters, natts));

	if (filter_cols != NULL)
	{
		dbe_cursor_set_filter_columns(state->cursor, filter_cols);
		pfree(filter_cols);
	}
}

/*
 * ¿Pasa la fila que next_keys() dejó a medio armar?
 *
 * Sin predicado la respuesta es siempre sí y no se evalúa nada — el cursor
 * ya llenó todo, porque set_filter_columns() no se llamó.
 */
static bool
dbe_scan_row_passes(DbeScanState *state, TupleTableSlot *slot)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;

	/*
	 * Sin predicado no hay nada que evaluar, pero igual hay que completar
	 * la fila: next_keys() solo llenó las columnas del filtro, que sin
	 * filtro son ninguna.
	 */
	if (state->qual == NULL)
	{
		dbe_cursor_fill_rest(state->cursor, slot);
		return true;
	}

	econtext->ecxt_scantuple = slot;
	if (!ExecQual(state->qual, econtext))
	{
		state->rows_filtered++;
		ResetExprContext(econtext);
		return false;
	}
	ResetExprContext(econtext);

	/* Recién ahora se decodifican las columnas que el predicado no mira. */
	dbe_cursor_fill_rest(state->cursor, slot);
	return true;
}

static TupleTableSlot *
dbe_scan_next(CustomScanState *node)
{
	DbeScanState *state = (DbeScanState *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (state->shared != NULL)
	{
		for (;;)
		{
			uint64		idx;

			while (dbe_cursor_next_keys_in_stripe(state->cursor, slot))
				if (dbe_scan_row_passes(state, slot))
					return slot;

			idx = pg_atomic_fetch_add_u64(&state->shared->next_stripe, 1);
			if (idx >= (uint64) dbe_cursor_nstripes(state->cursor))
				return ExecClearTuple(slot);
			if (!dbe_cursor_set_stripe(state->cursor, (int) idx))
				return ExecClearTuple(slot);
		}
	}

	while (dbe_cursor_next_keys(state->cursor, slot))
		if (dbe_scan_row_passes(state, slot))
			return slot;

	return ExecClearTuple(slot);
}

static bool
dbe_scan_recheck(CustomScanState *node, TupleTableSlot *slot)
{
	/* Sin EvalPlanQual: el storage es append-only, la fila no cambia. */
	return true;
}

static TupleTableSlot *
dbe_exec_scan(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) dbe_scan_next,
					(ExecScanRecheckMtd) dbe_scan_recheck);
}

static void
dbe_end_scan(CustomScanState *node)
{
	DbeScanState *state = (DbeScanState *) node;

	if (state->cursor != NULL)
	{
		dbe_cursor_end(state->cursor);
		state->cursor = NULL;
	}
}

static void
dbe_rescan_scan(CustomScanState *node)
{
	DbeScanState *state = (DbeScanState *) node;

	state->rows_filtered = 0;
	state->stripes_pruned = 0;
	state->granules_pruned = 0;
	state->published = false;
	state->totals_ready = false;
	if (state->cursor != NULL)
		dbe_cursor_rescan(state->cursor);
}

/* ------------------------------------------------------------------------
 * Scan paralelo
 *
 * El reparto es por stripe: cada worker toma el siguiente con un fetch-add
 * y lo recorre entero.  Los workers arman su propia lista de stripes con el
 * mismo snapshot y el mismo orden (el índice de dbengine.stripe), así que
 * el índice compartido significa lo mismo en todos.
 * ------------------------------------------------------------------------ */

static Size
dbe_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	return sizeof(DbeParallelScanShared);
}

static void
dbe_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
				   void *coordinate)
{
	DbeScanState *state = (DbeScanState *) node;
	DbeParallelScanShared *shared = (DbeParallelScanShared *) coordinate;

	pg_atomic_init_u64(&shared->next_stripe, 0);
	pg_atomic_init_u64(&shared->stripes_pruned, 0);
	pg_atomic_init_u64(&shared->granules_pruned, 0);
	pg_atomic_init_u64(&shared->rows_filtered, 0);
	state->shared = shared;
	dbe_cursor_set_stripe(state->cursor, -1);
}

static void
dbe_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					 void *coordinate)
{
	DbeParallelScanShared *shared = (DbeParallelScanShared *) coordinate;

	pg_atomic_write_u64(&shared->next_stripe, 0);
	pg_atomic_write_u64(&shared->stripes_pruned, 0);
	pg_atomic_write_u64(&shared->granules_pruned, 0);
	pg_atomic_write_u64(&shared->rows_filtered, 0);
}

static void
dbe_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	DbeScanState *state = (DbeScanState *) node;

	state->shared = (DbeParallelScanShared *) coordinate;
	dbe_cursor_set_stripe(state->cursor, -1);
}

/*
 * Contadores del EXPLAIN en un plan paralelo.
 *
 * Cada participante suma los suyos al DSM, y el LÍDER se los lleva a su
 * estado local acá mismo.  No puede leerlos después: ExecShutdownGather()
 * destruye el contexto paralelo —y con él el segmento— antes de que
 * ExplainPrintPlan() imprima nada.  Leerlos ahí es un segfault adentro de
 * pg_atomic_read_u64(), que es exactamente lo que pasaba.
 *
 * Para cuando corre el shutdown del líder, el plan ya se ejecutó entero, así
 * que los workers terminaron y ya publicaron.  Si alguno murió con error se
 * pierden sus números, que es lo de menos.
 */
static void
dbe_shutdown_scan(CustomScanState *node)
{
	DbeScanState *state = (DbeScanState *) node;

	if (state->shared == NULL || state->published)
		return;
	state->published = true;

	pg_atomic_fetch_add_u64(&state->shared->rows_filtered,
							state->rows_filtered);
	if (state->cursor != NULL)
	{
		pg_atomic_fetch_add_u64(&state->shared->stripes_pruned,
								dbe_cursor_stripes_pruned(state->cursor));
		pg_atomic_fetch_add_u64(&state->shared->granules_pruned,
								dbe_cursor_granules_pruned(state->cursor));
	}

	if (!IsParallelWorker())
	{
		state->rows_filtered = pg_atomic_read_u64(&state->shared->rows_filtered);
		state->stripes_pruned = pg_atomic_read_u64(&state->shared->stripes_pruned);
		state->granules_pruned = pg_atomic_read_u64(&state->shared->granules_pruned);
		state->totals_ready = true;
	}
}

static void
dbe_explain_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	DbeScanState *state = (DbeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	relation = node->ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(relation);
	StringInfoData buf;
	int			i;
	bool		first = true;

	initStringInfo(&buf);
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || !state->wanted[i])
			continue;
		appendStringInfo(&buf, "%s%s", first ? "" : ", ", NameStr(att->attname));
		first = false;
	}

	ExplainPropertyText("Columnas leídas", first ? "(ninguna)" : buf.data, es);
	pfree(buf.data);

	/*
	 * El predicado no está en plan.qual, así que explain.c no lo imprime:
	 * lo imprime el nodo, con el mismo formato.
	 */
	if (cscan->custom_exprs != NIL)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   node->ss.ps.plan,
													   ancestors);

		ExplainPropertyText("Filter",
							deparse_expression((Node *) make_ands_explicit(cscan->custom_exprs),
											   context, true, false),
							es);
		/* Mismo criterio que show_instrumentation_count() de explain.c. */
		if (es->analyze &&
			(state->rows_filtered > 0 || es->format != EXPLAIN_FORMAT_TEXT))
			ExplainPropertyInteger("Rows Removed by Filter", NULL,
								   (int64) state->rows_filtered, es);
	}

	if (state->filters->nfilters > 0)
		ExplainPropertyInteger("Predicados sobre zone maps", NULL,
							   state->filters->nfilters, es);
	if (es->analyze && state->cursor != NULL)
	{
		int64		gran;

		ExplainPropertyInteger("Stripes descartados", NULL,
							   (int64) (state->totals_ready
										? state->stripes_pruned
										: dbe_cursor_stripes_pruned(state->cursor)),
							   es);

		/*
		 * Los gránulos son el descarte DENTRO de los stripes que sí se
		 * leyeron; se muestran solo si hubo alguno para no ensuciar el plan
		 * de las consultas que no filtran por una columna con zone map fino.
		 */
		gran = (int64) (state->totals_ready
						? state->granules_pruned
						: dbe_cursor_granules_pruned(state->cursor));
		if (gran > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Granulos descartados", NULL, gran, es);
	}
}
