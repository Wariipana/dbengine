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

#include <math.h>

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
#include "mb/pg_wchar.h"
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
#include "optimizer/prep.h"
#include "parser/parse_agg.h"
#include "parser/scansup.h"
#include "port/pg_bitutils.h"
#include "common/int.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "rewrite/rewriteManip.h"

#include "dbengine/catalog.h"
#include "dbengine/columnar.h"
#include "dbengine/customscan.h"
#include "dbengine/dbengine.h"
#include "dbengine/dictqual.h"
#include "dbengine/flathash.h"
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
	Oid			inputtype;		/* de la columna */
	int16		inputtyplen;
	bool		inputtypbyval;
	Oid			collation;

	/*
	 * Argumento que no es la columna sola: se evalúa contra el slot de la
	 * relación con la columna cargada.  val* es el tipo de lo que le llega
	 * a la transición: el de la expresión, o el de la columna si no hay.
	 */
	ExprState  *argexpr;
	int16		valtyplen;
	bool		valtypbyval;

	/*
	 * El argumento es length(col) sobre texto: se cuenta acá en vez de
	 * evaluar la expresión.  len_bytes: la codificación es de un byte por
	 * carácter y el largo es el de los bytes.
	 */
	bool		textlen;
	bool		len_bytes;

	/*
	 * MIN/MAX se reconocen por pg_aggregate.aggsortop, que es la misma
	 * marca que usa el planner para el truco de resolverlos con un índice.
	 * Con eso se pueden contestar desde los zone maps, sin leer datos.
	 */
	bool		is_minmax;
	bool		want_min;
	FmgrInfo	minmax_cmp;

	/*
	 * sum, avg y count(col) sobre smallint/integer se pueden avanzar de a
	 * un stripe entero con los agregados precalculados del catálogo (SMA):
	 * la suma exacta y null_count.  sma_now dice si se hizo con el stripe
	 * actual, y entonces el recorrido por fila lo saltea.
	 */
	uint8		sma;
	bool		sma_now;

	/*
	 * sum(col + k): el stripe aporta sum(col) + k·count(col).  sma_lo y
	 * sma_hi son el rango del tipo de la expresión: fila por fila, col + k
	 * puede dar overflow, así que el atajo vale solo si el zone map prueba
	 * que ni min + k ni max + k se salen.
	 */
	bool		sma_affine;
	int64		sma_k;
	int64		sma_lo;
	int64		sma_hi;

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
	Expr	   *group_expr;		/* clave que es una expresión sobre la columna */

	/*
	 * Una constante del tlist (solo con la tabla propia): viaja como columna
	 * de agrupación para que los lazos la salteen, pero sin clave.
	 */
	bool		is_const;
	Datum		constvalue;
	bool		constisnull;

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

	/*
	 * Para no buscar lo que ya se buscó, dentro de un stripe: con
	 * diccionario, cada código una vez (dist_codes son los de todas las
	 * filas, dist_seen un byte por código); sin diccionario, la fila que
	 * repite el valor de la anterior se saltea.
	 */
	bool		dist_dict;
	uint32	   *dist_codes;
	uint8	   *dist_seen;
	uint32		dist_cap;
	uint32		dist_seen_cap;
	Datum		dist_prev;
	bool		dist_has_prev;
} DbeAggState;

typedef struct DbeAggParallelShared
{
	pg_atomic_uint64 next_stripe;

	/*
	 * Tabla propia en paralelo: los participantes se reparten baldes de
	 * grupos, no stripes.  Hay tantos como participantes planeados; si se
	 * lanzan menos workers, los que sobran los toma quien termine antes.
	 */
	pg_atomic_uint32 next_bucket;
	uint32		nbuckets;
	pg_atomic_uint64 flat_groups;
	pg_atomic_uint64 flat_spilled;
	pg_atomic_uint64 flat_parts;
	pg_atomic_uint64 flat_peak;	/* el máximo de un participante */

	/* Contadores del EXPLAIN; ver la nota en customscan.c. */
	pg_atomic_uint64 rows_seen;
	pg_atomic_uint64 rows_filtered;
	pg_atomic_uint64 stripes_pruned;
	pg_atomic_uint64 granules_pruned;
	pg_atomic_uint64 stripes_sma;
} DbeAggParallelShared;

typedef struct DbeVectorAggState
{
	CustomScanState css;
	DbeAggParallelShared *shared;	/* NULL si el nodo corre serial */
	bool		only_count_star;
	bool		only_metadata;	/* todos count(*) o min/max */
	bool		sma_any;		/* algún agregado admite SMA */
	int			next_index;		/* próximo stripe del recorrido serial */
	uint64		stripes_sma;	/* stripes contestados sin leer datos */

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
	DbeDictQual *qual;
	DbeFilterSet *filters;
	int			nqualcols;
	AttrNumber *qualcols;
	DbeColumnVector *qualvecs;
	bool	   *qualvec_ok;
	bool	   *qual_lazy;		/* solo la miran cláusulas de diccionario */
	uint64		qual_row;		/* fila que se está evaluando, para el fill */
	int16	   *qual_typlen;
	bool	   *qual_typbyval;
	uint8	   *sel;			/* dbe_agg_prefilter(): un byte por fila */
	uint64		sel_cap;
	uint8	   *stripe_sel;		/* el del stripe que se recorre, o NULL */
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

	/*
	 * Agrupación por hash con la tabla propia (flathash.h): claves de
	 * ancho fijo y agregados de estado entero.  Es un agregado completo, no
	 * parcial, y vuelca a disco por su cuenta.
	 */
	bool		flat;
	DbeFlatAgg *flatagg;
	bool		flat_filled;
	Datum	   *flat_keys;
	bool	   *flat_keynulls;
	Datum	   *flat_vals;		/* uno por agregado de verdad, en orden */
	bool	   *flat_valnulls;
	Datum	   *flat_outs;
	bool	   *flat_outnulls;

	/*
	 * count(DISTINCT col) por grupo: `flatagg` agrupa por las claves más la
	 * columna, con los demás agregados, y `flatdist` por las claves solas,
	 * con los agregados en el orden del tlist.  flat_dist es el índice en
	 * `aggs` del primer count(DISTINCT), -1 sin ninguno.  flat_map[j] es
	 * dónde empieza en los estados de `flatagg` el agregado j de `flatdist`,
	 * o -1 si es un count(DISTINCT), que suma 1 por valor no nulo.
	 */
	/*
	 * Claves que son una expresión sobre la columna (NULL donde es la
	 * columna sola), con su propio ExprContext: el de los argumentos de los
	 * agregados se resetea en cada uno, y el valor de la clave tiene que
	 * durar hasta que la tabla lo copie.
	 */
	ExprState **group_exprs;
	ExprContext *key_econtext;

	/*
	 * Top-N (dbe_flat_topn_spec()): los k mejores grupos según el ORDER BY,
	 * en un heap con el peor arriba.  Cada entrada guarda las claves, los
	 * estados y los valores de orden; los demás agregados se calculan
	 * recién al emitir.  topn_src[j] es de dónde sale el valor de orden j:
	 * >= 0 una clave, <= -1 el agregado -1 - v de la tabla que se emite.
	 */
	int			topn_k;
	int			topn_nsort;
	int		   *topn_src;
	bool	   *topn_byval;
	int16	   *topn_len;
	SortSupport topn_ssup;
	int			topn_n;
	int			topn_emit;
	bool		topn_filled;	/* ya se llenó para el balde actual */
	int		   *topn_heap;
	Datum	   *topn_keys;
	bool	   *topn_keynulls;
	uint64	   *topn_states;
	Datum	   *topn_sv;
	bool	   *topn_svnull;
	Datum	   *topn_cand;
	bool	   *topn_candnull;
	MemoryContext topn_cxt;		/* copias de lo que está en el heap */
	MemoryContext topn_tmpcxt;	/* lo de un candidato o una fila emitida */

	DbeFlatAgg *flatdist;
	int			flat_dist;
	int		   *flat_map;
	DbeFlatAggDef *flat_defs2;
	int			flat_ndefs2;
	uint64	   *flat_states2;

	/*
	 * En paralelo cada balde recorre la tabla entera y vuelve a descartar
	 * los mismos stripes: se publica lo que descartó la vuelta del balde 0.
	 * Y las cifras de la tabla, sumadas entre participantes.
	 */
	uint64		flat_stripes_pruned;
	uint64		flat_granules_pruned;
	uint64		flat_groups_total;
	uint64		flat_spilled_total;
	int			flat_parts_total;
	Size		flat_peak_total;
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
static void dbe_agg_value(DbeVectorAggState *state, DbeAggState *agg,
						  uint64 row, Datum *value, bool *isnull);
static void dbe_agg_missing_value(DbeVectorAggState *state, AttrNumber attnum,
								  Datum *value, bool *isnull);
static void dbe_agg_detect_textlen(DbeAggState *agg, Expr *arg);
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
	 * Por grupo lo resuelve solo la tabla propia (ver
	 * dbe_flat_distinct_ok()); quien llama se encarga de exigirlo.
	 */
	if (ref->aggdistinct != NIL)
	{
		if (list_length(ref->args) != 1 ||
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
	if (IsA(arg, Var))
	{
		Var		   *var = (Var *) arg;

		if (var->varno != relid || var->varattno <= 0 || var->varlevelsup != 0)
			return false;
		*attnum = var->varattno;
		return true;
	}

	/*
	 * Una expresión sobre UNA columna —`sum(ResolutionWidth + 1)`, q30— se
	 * evalúa fila por fila contra el slot de la relación, como el WHERE.
	 * Con DISTINCT no: el conjunto de hash es sobre la columna.
	 */
	if (*is_distinct || contain_volatile_functions(arg) ||
		contain_subplans(arg) || expression_returns_set(arg))
		return false;
	{
		List	   *vars = pull_var_clause(arg, 0);
		ListCell   *lc;

		*attnum = InvalidAttrNumber;
		foreach(lc, vars)
		{
			Var		   *var = (Var *) lfirst(lc);

			if (var->varno != relid || var->varattno <= 0 ||
				var->varlevelsup != 0 ||
				(*attnum != InvalidAttrNumber && var->varattno != *attnum))
				return false;
			*attnum = var->varattno;
		}
		if (*attnum == InvalidAttrNumber)
			return false;		/* sin columnas: que lo haga el executor */
	}

	return true;
}

/*
 * ¿La columna puede ser clave de la tabla propia (flathash.h)?  Su
 * igualdad tiene que ser la binaria: dos valores iguales para el operador
 * del GROUP BY si y solo si son iguales sus bytes.  Por eso los float
 * quedan afuera (0 y -0 son iguales, NaN también), y el operador tiene que
 * ser el = del tipo y no otro.
 */
static bool
dbe_flat_key_ok(Oid typid, Oid eqop, Oid collid)
{
	TypeCacheEntry *tc;

	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case BOOLOID:
		case OIDOID:
			break;

			/*
			 * Texto, si la collation es determinista: ahí texteq compara
			 * bytes.  bpchar no, porque ignora los espacios del final.
			 */
		case TEXTOID:
		case VARCHAROID:
			if (!OidIsValid(collid) || !get_collation_isdeterministic(collid))
				return false;
			break;
		default:
			return false;
	}
	tc = lookup_type_cache(typid, TYPECACHE_EQ_OPR);
	return OidIsValid(eqop) && eqop == tc->eq_opr;
}

/*
 * ¿`extract(campo FROM ts)` con un campo de resultado entero?  Postgres arma
 * ese numeric con int64_to_numeric() —o con numeric_in() para un timestamp
 * infinito—, siempre con la misma escala, así que dos resultados iguales
 * tienen los mismos bytes.  Con second, epoch o julian no: tienen
 * decimales.
 */
static bool
dbe_flat_extract_integral(Node *expr)
{
	FuncExpr   *f = (FuncExpr *) expr;
	Const	   *c;
	char	   *units;
	char	   *low;
	int			val = 0;
	int			type;

	if (!IsA(f, FuncExpr) || list_length(f->args) != 2 ||
		(f->funcid != F_EXTRACT_TEXT_TIMESTAMP &&
		 f->funcid != F_EXTRACT_TEXT_TIMESTAMPTZ))
		return false;
	c = (Const *) linitial(f->args);
	if (!IsA(c, Const) || c->constisnull || c->consttype != TEXTOID)
		return false;
	units = TextDatumGetCString(c->constvalue);
	low = downcase_truncate_identifier(units, strlen(units), false);
	type = DecodeUnits(0, low, &val);
	if (type != UNITS)
		return false;
	switch (val)
	{
		case DTK_MICROSEC:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_WEEK:
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_ISOYEAR:
		case DTK_DOW:
		case DTK_ISODOW:
		case DTK_DOY:
			return true;
		default:
			return false;
	}
}

/*
 * ¿Una expresión del GROUP BY puede ser clave de la tabla propia?  Tiene que
 * mirar una sola columna de la relación —se calcula fila por fila contra el
 * slot con esa columna cargada, como el argumento de un agregado—, no ser
 * volátil, y dar un tipo que sirva de clave (dbe_flat_key_ok()), o el
 * numeric de un extract() con campo entero.  *attnum queda con la columna.
 */
static bool
dbe_flat_expr_key_ok(Node *expr, Oid eqop, Index relid, AttrNumber *attnum)
{
	List	   *vars;
	ListCell   *lc;
	Oid			typid = exprType(expr);

	if (contain_volatile_functions(expr) || contain_subplans(expr) ||
		expression_returns_set(expr) || contain_agg_clause(expr) ||
		contain_window_function(expr))
		return false;

	*attnum = InvalidAttrNumber;
	vars = pull_var_clause(expr, 0);
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (var->varno != (int) relid || var->varattno <= 0 ||
			var->varlevelsup != 0 ||
			(*attnum != InvalidAttrNumber && var->varattno != *attnum))
			return false;
		*attnum = var->varattno;
	}
	if (*attnum == InvalidAttrNumber)
		return false;

	if (dbe_flat_key_ok(typid, eqop, exprCollation(expr)))
		return true;
	return typid == NUMERICOID && dbe_flat_extract_integral(expr) &&
		eqop == lookup_type_cache(NUMERICOID, TYPECACHE_EQ_OPR)->eq_opr;
}

/* Más que esto no se guarda en el top-N: con un LIMIT grande no conviene. */
#define DBE_TOPN_MAX	10000

/* Una constante entera no nula del LIMIT/OFFSET, 0 si no hay, o -1. */
static int64
dbe_limit_const(Node *n)
{
	Const	   *c = (Const *) n;

	if (n == NULL)
		return 0;
	if (!IsA(c, Const) || c->constisnull || c->consttype != INT8OID)
		return -1;
	return DatumGetInt64(c->constvalue);
}

/*
 * ¿El `ORDER BY ... LIMIT k` de la query se puede resolver con un top-N
 * adentro de la tabla propia?  Cada participante emite grupos completos,
 * así que los k primeros del total están entre los k primeros de alguno:
 * alcanza con que cada uno emita los suyos, y el Sort y el Limit de arriba
 * siguen igual.  Con agregados el planner no calcula el límite
 * (root->limit_tuples queda en -1): sale de las constantes.  Sin WITH
 * TIES, que necesita todos los empates.
 *
 * Devuelve [k, posiciones en tlist, sortops, collations, nulls_first], o
 * NIL.
 */
static List *
dbe_flat_topn_spec(PlannerInfo *root, List *tlist, double *k)
{
	Query	   *parse = root->parse;
	int64		count = dbe_limit_const(parse->limitCount);
	int64		offset = dbe_limit_const(parse->limitOffset);
	List	   *pos = NIL;
	List	   *ops = NIL;
	List	   *colls = NIL;
	List	   *nf = NIL;
	ListCell   *lc;

	if (parse->sortClause == NIL || parse->limitCount == NULL ||
		parse->limitOption != LIMIT_OPTION_COUNT ||
		count <= 0 || offset < 0 || count + offset > DBE_TOPN_MAX)
		return NIL;

	foreach(lc, parse->sortClause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *ste = get_sortgroupclause_tle(sgc, root->processed_tlist);
		ListCell   *lt;
		int			j = 0;
		int			found = -1;

		if (!OidIsValid(sgc->sortop))
			return NIL;
		foreach(lt, tlist)
		{
			if (equal(((TargetEntry *) lfirst(lt))->expr, ste->expr))
			{
				found = j;
				break;
			}
			j++;
		}
		if (found < 0)
			return NIL;
		pos = lappend_int(pos, found);
		ops = lappend_oid(ops, sgc->sortop);
		colls = lappend_oid(colls, exprCollation((Node *) ste->expr));
		nf = lappend_int(nf, sgc->nulls_first ? 1 : 0);
	}
	*k = (double) (count + offset);
	return list_make5(makeInteger((int) (count + offset)), pos, ops, colls, nf);
}

/*
 * ¿count(DISTINCT col) por grupo, con la tabla propia?  La columna tiene
 * que servir de clave (dbe_flat_key_ok()) con el operador del DISTINCT: la
 * tabla agrupa primero por las claves más la columna, y después por las
 * claves solas contando los valores no nulos (ver dbe_agg_fill_flat()).
 */
static bool
dbe_flat_distinct_ok(Aggref *ref, Index relid, AttrNumber *attnum)
{
	HeapTuple	tup;
	Form_pg_aggregate aggform;
	Var		   *var;
	SortGroupClause *sgc;
	bool		ok;

	if (ref->aggdistinct == NIL || list_length(ref->aggdistinct) != 1 ||
		list_length(ref->args) != 1 || ref->aggorder != NIL ||
		ref->aggfilter != NULL || ref->aggvariadic ||
		ref->aggkind != AGGKIND_NORMAL)
		return false;
	var = (Var *) ((TargetEntry *) linitial(ref->args))->expr;
	if (!IsA(var, Var) || var->varattno <= 0 || var->varlevelsup != 0 ||
		(relid != 0 && var->varno != (int) relid))
		return false;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return false;
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);
	ok = aggform->aggtransfn == F_INT8INC_ANY && !OidIsValid(aggform->aggfinalfn);
	ReleaseSysCache(tup);

	sgc = linitial_node(SortGroupClause, ref->aggdistinct);
	if (!ok || !dbe_flat_key_ok(var->vartype, sgc->eqop, ref->inputcollid))
		return false;
	*attnum = var->varattno;
	return true;
}

/*
 * ¿El agregado tiene un estado que la tabla propia sabe llevar?  Se
 * reconoce por las funciones de pg_aggregate y no por el nombre: un
 * agregado de usuario que se llame sum no las tiene.
 *
 *   count(*)                    int8inc
 *   count(col)                  int8inc_any, cualquier tipo
 *   sum(int2/int4)              int2_sum / int4_sum → int8
 *   avg(int2/int4)              int2/int4_avg_accum + int8_avg → numeric
 *   sum/avg(int8)               int8_avg_accum + numeric_poly_sum/avg → numeric
 *   min/max de enteros y fechas aggsortop = el < o > del tipo
 *
 * El argumento tiene que ser la columna sola.  `relid` 0 no comprueba de
 * qué relación es el Var: en ejecución los Var ya pasaron por setrefs.c.
 */
static bool
dbe_flat_agg_def(Aggref *ref, Index relid, DbeFlatAggDef *def)
{
	HeapTuple	tup;
	Form_pg_aggregate aggform;
	Var		   *var;
	Oid			transfn;
	Oid			finalfn;
	Oid			sortop;
	bool		ok = false;

	if (ref->aggorder != NIL || ref->aggdistinct != NIL ||
		ref->aggfilter != NULL || ref->aggvariadic ||
		ref->aggkind != AGGKIND_NORMAL)
		return false;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return false;
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);
	transfn = aggform->aggtransfn;
	finalfn = aggform->aggfinalfn;
	sortop = aggform->aggsortop;
	ReleaseSysCache(tup);

	if (ref->aggstar)
	{
		def->kind = DBE_FLAT_COUNT_STAR;
		def->vallen = 0;
		return transfn == F_INT8INC && !OidIsValid(finalfn);
	}

	if (list_length(ref->args) != 1)
		return false;
	var = (Var *) ((TargetEntry *) linitial(ref->args))->expr;
	if (!IsA(var, Var) || var->varattno <= 0 || var->varlevelsup != 0 ||
		(relid != 0 && var->varno != (int) relid))
		return false;

	if (transfn == F_INT8INC_ANY && !OidIsValid(finalfn))
	{
		def->kind = DBE_FLAT_COUNT;
		def->vallen = 0;		/* solo importa si es nulo */
		return true;
	}

	switch (var->vartype)
	{
		case INT2OID:
			def->vallen = 2;
			ok = (transfn == F_INT2_SUM || transfn == F_INT2_AVG_ACCUM);
			break;
		case INT4OID:
		case DATEOID:
			def->vallen = 4;
			ok = (var->vartype == INT4OID &&
				  (transfn == F_INT4_SUM || transfn == F_INT4_AVG_ACCUM));
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			def->vallen = 8;

			/*
			 * sum/avg(int8): int8_avg_accum, con numeric_poly_sum o
			 * numeric_poly_avg.  La tabla lleva la suma en int128, como el
			 * estado de Postgres.
			 */
			if (var->vartype == INT8OID && transfn == F_INT8_AVG_ACCUM)
			{
				if (finalfn == F_NUMERIC_POLY_SUM)
				{
					def->kind = DBE_FLAT_SUM8;
					return true;
				}
				if (finalfn == F_NUMERIC_POLY_AVG)
				{
					def->kind = DBE_FLAT_AVG8;
					return true;
				}
				return false;
			}
			break;
		default:
			return false;
	}
	if (ok)
	{
		if ((transfn == F_INT2_SUM || transfn == F_INT4_SUM) &&
			!OidIsValid(finalfn))
		{
			def->kind = DBE_FLAT_SUM;
			return true;
		}
		if ((transfn == F_INT2_AVG_ACCUM || transfn == F_INT4_AVG_ACCUM) &&
			finalfn == F_INT8_AVG)
		{
			def->kind = DBE_FLAT_AVG;
			return true;
		}
		return false;
	}

	if (OidIsValid(sortop) && ref->aggtype == var->vartype)
	{
		TypeCacheEntry *tc = lookup_type_cache(var->vartype,
											   TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

		if (sortop == tc->lt_opr)
		{
			def->kind = DBE_FLAT_MIN;
			return true;
		}
		if (sortop == tc->gt_opr)
		{
			def->kind = DBE_FLAT_MAX;
			return true;
		}
	}
	return false;
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

		/* El zone map es de la columna, no de una expresión sobre ella. */
		if (!IsA(((TargetEntry *) linitial(ref->args))->expr, Var))
			return false;

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
										 root->parse->groupClause,
										 (List *) extra->havingQual,
										 &extra->agg_final_costs, ngroups);
	}
	else
		final = (Path *) create_agg_path(root, output_rel, gather,
										 output_rel->reltarget,
										 AGG_PLAIN, AGGSPLIT_FINAL_DESERIAL,
										 NIL, (List *) extra->havingQual,
										 &extra->agg_final_costs, 1);

	add_path(output_rel, final);
}

/*
 * ¿Da lo mismo el agregado si una fila de entrada aparece dos veces?  Con
 * DISTINCT sí, por definición.  Sin DISTINCT, los que tienen aggsortop
 * —min, max, bool_and, bool_or—, que devuelven un elemento extremo.
 */
static bool
dbe_agg_ignores_duplicates(Aggref *ref)
{
	HeapTuple	tup;
	Oid			sortop;

	if (ref->aggkind != AGGKIND_NORMAL || ref->aggsplit != AGGSPLIT_SIMPLE)
		return false;
	if (ref->aggdistinct != NIL)
		return true;
	if (ref->aggstar || ref->aggorder != NIL)
		return false;

	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return false;
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	return OidIsValid(sortop);
}

/*
 * GROUP BY con agregados que ignoran duplicados: el plan normal con un scan
 * que descarta las filas repetidas por stripe (rowdedup.h).
 *
 * Es el plan de siempre —Sort por las claves del grupo y GroupAggregate— y
 * lo arma el planner con sus propias funciones; lo único distinto es el
 * scan de abajo, que entrega menos filas.  En q9 de ClickBench el Sort pasa
 * de 10M filas a las que sobreviven al descarte.
 */
static void
dbe_add_dedup_agg_path(PlannerInfo *root, RelOptInfo *input_rel,
					   RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	List	   *exprs;
	ListCell   *lc;
	double		nrows;
	double		ndistinct;
	double		ngroups;
	AggClauseCosts costs;

	if (!dbe_enable_vectorized || !dbe_enable_scan_dedup || extra == NULL ||
		extra->patype != PARTITIONWISE_AGGREGATE_NONE)
		return;
	if (parse->groupClause == NIL || parse->groupingSets != NIL ||
		!parse->hasAggs || input_rel->reloptkind != RELOPT_BASEREL)
		return;

	/*
	 * Con una función volátil, cuántas veces se evalúa es parte del
	 * resultado: `GROUP BY random()` reparte distinto si llegan menos filas.
	 */
	if (contain_volatile_functions((Node *) parse->targetList) ||
		contain_volatile_functions(parse->havingQual))
		return;

	exprs = pull_var_clause((Node *) list_make2(output_rel->reltarget->exprs,
												extra->havingQual),
							PVC_INCLUDE_AGGREGATES |
							PVC_RECURSE_WINDOWFUNCS |
							PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, exprs)
		if (IsA(lfirst(lc), Aggref) &&
			!dbe_agg_ignores_duplicates((Aggref *) lfirst(lc)))
			return;

	nrows = input_rel->rows;
	if (nrows < 2)
		return;

	/*
	 * Cuántas filas sobreviven no se sabe al planificar: depende de cuánto
	 * se repitan dentro de un stripe, que es una propiedad del orden físico.
	 * Las combinaciones distintas en toda la tabla son el piso, y la tabla
	 * entera el techo; se toma la media geométrica.  Si el planner cree que
	 * casi todas las filas son distintas, la estimación queda cerca de la
	 * tabla y el plan normal, que no paga el hash, gana solo.
	 */
	ndistinct = estimate_num_groups(root, input_rel->reltarget->exprs, nrows,
									NULL, NULL);
	ndistinct = clamp_row_est(sqrt(Min(ndistinct, nrows) * nrows));
	ngroups = estimate_num_groups(root,
								  get_sortgrouplist_exprs(root->processed_groupClause,
														  parse->targetList),
								  nrows, NULL, NULL);

	MemSet(&costs, 0, sizeof(costs));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &costs);

	/*
	 * Serial y paralelo.  En paralelo cada worker descarta y ordena sus
	 * stripes, y un Gather Merge intercala las salidas ya ordenadas: el
	 * GroupAggregate de arriba ve lo mismo que en serial.  Un duplicado
	 * entre workers es uno más entre stripes, y lo resuelve el DISTINCT.
	 */
	for (int pass = 0; pass < 2; pass++)
	{
		List	   *paths = pass == 0 ? input_rel->pathlist
			: input_rel->partial_pathlist;

		if (pass == 1 && !output_rel->consider_parallel)
			break;

		foreach(lc, paths)
		{
			Path	   *path = dbe_scan_dedup_path(root, (Path *) lfirst(lc),
												   ndistinct / nrows);
			GroupByOrdering *info;

			if (path == NULL)
				continue;

			info = (GroupByOrdering *)
				linitial(get_useful_group_keys_orderings(root, path));
			if (!pathkeys_contained_in(info->pathkeys, path->pathkeys))
				path = (Path *) create_sort_path(root, output_rel, path,
												 info->pathkeys, -1.0);
			if (pass == 1)
			{
				double		total = path->rows * path->parallel_workers;

				path = (Path *) create_gather_merge_path(root, output_rel, path,
														 path->pathtarget,
														 info->pathkeys, NULL,
														 &total);
			}
			add_path(output_rel, (Path *)
					 create_agg_path(root, output_rel, path,
									 output_rel->reltarget, AGG_SORTED,
									 AGGSPLIT_SIMPLE, info->clauses,
									 (List *) extra->havingQual, &costs,
									 ngroups));
			break;
		}
	}
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
	Node	   *having = NULL;
	bool		flat_ok;
	bool		partial_ok = true;
	bool		has_const = false;
	bool		has_expr_key = false;
	int			nflat_aggs = 0;
	AttrNumber	dist_attnum = InvalidAttrNumber;
	Node	   *dist_expr = NULL;
	double		flat_groups = 1;	/* las entradas de la tabla propia */

	if (prev_create_upper_paths_hook)
		prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

	if (stage == UPPERREL_GROUP_AGG)
		dbe_add_dedup_agg_path(root, input_rel, output_rel,
							   (GroupPathExtraData *) extra);

	if (stage != UPPERREL_GROUP_AGG || !dbe_enable_vectorized ||
		!dbe_enable_agg_pushdown)
		return;

	/*
	 * Sobre una única relación base.  Se acepta GROUP BY solo si el scan ya
	 * entrega las filas en ese orden (se comprueba más abajo, contra los
	 * pathkeys del path de entrada).  Grouping sets, ventanas y DISTINCT
	 * siguen cayendo al plan normal.  HAVING se acepta solo en el camino
	 * paralelo, donde lo evalúa el Finalize de arriba (ver más abajo).
	 */
	if (parse->groupingSets != NIL || parse->hasWindowFuncs ||
		parse->distinctClause != NIL || parse->hasTargetSRFs)
		return;
	if (root->hasHavingQual)
	{
		if (extra == NULL)
			return;
		having = ((GroupPathExtraData *) extra)->havingQual;
		if (contain_volatile_functions(having) || contain_subplans(having))
			return;
	}
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

		/*
		 * Con una expresión en el GROUP BY el planner la calcula en el scan,
		 * con una proyección encima.  Este nodo la calcula por su cuenta
		 * desde las columnas.
		 */
		if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
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
	/*
	 * La tabla propia es un agregado completo y serial: no puede dejarle el
	 * HAVING a un Finalize.
	 */
	flat_ok = grouped && hashed && having == NULL && dbe_enable_flat_hash;

	tlist = make_tlist_from_pathtarget(output_rel->reltarget);
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		AttrNumber	attnum;

		if (IsA(tle->expr, Aggref))
		{
			bool		is_distinct;
			DbeFlatAggDef def;

			if (!dbe_agg_supported((Aggref *) tle->expr, input_rel->relid,
								   &attnum, grouped, &is_distinct))
				return;

			/*
			 * count(DISTINCT) por grupo: todos sobre la misma columna, que
			 * es la que la tabla agrega como clave del primer nivel.
			 */
			if (is_distinct && grouped)
			{
				AttrNumber	da;

				if (!flat_ok ||
					!dbe_flat_distinct_ok((Aggref *) tle->expr,
										  input_rel->relid, &da) ||
					(dist_attnum != InvalidAttrNumber && da != dist_attnum) ||
					++nflat_aggs > 32)
					return;
				dist_attnum = da;
				dist_expr = (Node *) ((TargetEntry *)
									  linitial(((Aggref *) tle->expr)->args))->expr;
			}
			else if (flat_ok &&
					 (!dbe_flat_agg_def((Aggref *) tle->expr, input_rel->relid, &def) ||
					  ++nflat_aggs > 32))
				flat_ok = false;
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
			if (flat_ok &&
				!dbe_flat_key_ok(var->vartype, sgc->eqop, var->varcollid))
				flat_ok = false;

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

		/*
		 * Una expresión del GROUP BY sobre una columna —`ClientIP - 1` en
		 * q36, `extract(minute FROM EventTime)` en q19— la calcula la tabla
		 * propia fila por fila.  Los demás caminos no saben.
		 */
		if (grouped && tle->ressortgroupref != 0 && !IsA(tle->expr, Const))
		{
			SortGroupClause *sgc;
			AttrNumber	col;

			sgc = get_sortgroupref_clause_noerr(tle->ressortgroupref,
												parse->groupClause);
			if (sgc == NULL || !OidIsValid(sgc->eqop) || !flat_ok ||
				!dbe_flat_expr_key_ok((Node *) tle->expr, sgc->eqop,
									  input_rel->relid, &col))
				return;
			has_expr_key = true;
			agg_attnums = lappend_int(agg_attnums, col);
			group_attnums = lappend_int(group_attnums, col);
			group_eqops = lappend_oid(group_eqops, sgc->eqop);
			continue;
		}

		/*
		 * Una constante en el SELECT —`SELECT 1, URL, count(*)` en q35— la
		 * emite la tabla propia tal cual.  Los demás caminos no saben.
		 */
		if (IsA(tle->expr, Const))
		{
			has_const = true;
			agg_attnums = lappend_int(agg_attnums, InvalidAttrNumber);
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
	/*
	 * Contra el GROUP BY ya procesado: de ahí el planner saca las constantes
	 * (`GROUP BY 1, URL` de q35), que en parse->groupClause siguen estando.
	 */
	if (grouped &&
		list_length(group_attnums) != list_length(root->processed_groupClause))
		return;
	if (list_length(group_attnums) + (dist_attnum != InvalidAttrNumber) >
		DBE_FLAT_MAX_KEYS)
		flat_ok = false;
	if (grouped && n_distinct > 0)
	{
		if (!flat_ok)
			return;
		partial_ok = false;
	}

	if (has_const || has_expr_key)
	{
		if (!flat_ok)
			return;
		partial_ok = false;
	}

	/*
	 * Los agregados del HAVING tienen que salir del parcial aunque no estén
	 * en el SELECT —`HAVING count(*) > 100000` en q28—, porque el Finalize
	 * los arma combinando estados parciales.  Se agregan al final del tlist
	 * del nodo; un Var suelto del HAVING solo puede ser una columna del
	 * GROUP BY, que ya está.
	 */
	if (having != NULL)
	{
		List	   *hexprs = pull_var_clause(having,
											 PVC_INCLUDE_AGGREGATES |
											 PVC_RECURSE_PLACEHOLDERS);

		foreach(lc, hexprs)
		{
			Node	   *e = (Node *) lfirst(lc);
			AttrNumber	attnum;
			bool		is_distinct;

			if (tlist_member((Expr *) e, tlist) != NULL)
				continue;
			if (!IsA(e, Aggref) ||
				!dbe_agg_supported((Aggref *) e, input_rel->relid,
								   &attnum, grouped, &is_distinct))
				return;
			tlist = lappend(tlist,
							makeTargetEntry((Expr *) copyObject(e),
											list_length(tlist) + 1,
											NULL, false));
			agg_attnums = lappend_int(agg_attnums, attnum);
			if (is_distinct)
				n_distinct++;
		}
	}

	/*
	 * La versión con hash se ofrece aunque la estimación de grupos no entre
	 * en work_mem: solo existe como agregado parcial debajo de un Finalize
	 * (ver más abajo), y al pasarse del presupuesto emite lo que tiene y
	 * sigue con la tabla vacía.  Antes se descartaba si no entraba, y eso
	 * dejaba afuera justo los GROUP BY de texto con muchos valores —q34 y
	 * q35 agrupan 10M filas en 2,6M URLs—, donde agrupar por el código del
	 * diccionario es lo que más rinde.
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
		flat_groups = ngroups;
		if (dist_expr != NULL)
			flat_groups = estimate_num_groups(root, lappend(list_copy(gexprs),
															dist_expr),
											  input_path->rows, NULL, NULL);

		/*
		 * Con casi un grupo por fila el parcial no reduce nada: el Finalize
		 * recibe todas las filas y, si no le entran en memoria, su
		 * HashAggregate vuelca a disco.  En q32 —1,37M grupos en 1,37M
		 * filas— eso fue 1,28× más lento que el plan normal, que ordena.
		 * Así que se ofrece si la tabla entra en work_mem, donde no hay
		 * volcado que temer, o si se espera al menos reducir a la mitad.
		 * La estimación de memoria es grosera a propósito —la clave del
		 * grupo más el estado de cada agregado, redondeado para arriba—
		 * porque errar por poco no cambia la decisión.
		 */
		if (hashed)
		{
			Size		per_group = sizeof(MinimalTupleData) + 64 +
				sizeof(Datum) * 4 * list_length(agg_attnums);
			bool		fits = ngroups * (double) per_group <=
				(double) work_mem * 1024.0;

			if (!fits && ngroups > input_path->rows * 0.5)
				partial_ok = false;
		}
	}
	if (!partial_ok && !flat_ok)
		return;

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
		bool		partial_path;
		bool		few_groups;

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
		if (!(grouped && hashed) && having == NULL)
			add_path(output_rel, (Path *) path);

		/*
		 * Con la tabla propia el camino serial con hash sí se ofrece: vuelca
		 * a disco por su cuenta, así que una estimación mala cuesta un nivel
		 * más de particiones y no la memoria.  Lleva su propia copia de
		 * custom_private, con la marca 2 y la estimación de grupos, porque
		 * `private` sigue siendo la del camino paralelo.
		 */
		/*
		 * Con pocos grupos gana el parcial paralelo: reparte stripes y el
		 * Finalize junta unos pocos estados, mientras que la tabla propia en
		 * paralelo le hace leer la tabla entera a cada participante para
		 * elegir su balde.  `GROUP BY RegionID` (3.586 grupos en 10M filas):
		 * 410 contra 294 ms con count(*), 603 contra 440 con sum(int8).  El
		 * modelo de costos no lo veía; sumarle al camino paralelo el hash de
		 * todas las filas lo arreglaba acá pero empeoraba hasta 1,8× las de
		 * muchos grupos (q15, q16, q34, q35).  Así que se decide por la
		 * cantidad de grupos: más de mil filas por grupo, y que el parcial
		 * se vaya a ofrecer.
		 */
		partial_path = partial_ok && n_distinct == 0 && !all_metadata &&
			input_rel->consider_parallel &&
			output_rel->consider_parallel && extra != NULL &&
			((GroupPathExtraData *) extra)->partial_costs_set &&
			compute_parallel_worker(input_rel, input_rel->pages, -1,
									max_parallel_workers_per_gather) > 0;
		few_groups = grouped && partial_path &&
			ngroups * 1000.0 < input_path->rows;

		if (grouped && hashed && flat_ok && !few_groups)
		{
			double		in_rows = input_path->rows;
			double		nagg = (double) list_length(agg_attnums);
			Size		entry = sizeof(uint64) * (2 + list_length(agg_attnums) * 2) + 8;
			double		hash_mem = (double) get_hash_memory_limit();
			List	   *fprivate = list_copy(private);
			List	   *topn;
			double		topn_k = 0;

			llast(fprivate) = makeInteger(2);
			fprivate = lappend(fprivate,
							   makeInteger((int) Min(ngroups, (double) PG_INT32_MAX)));
			topn = dbe_flat_topn_spec(root, tlist, &topn_k);
			fprivate = lappend(fprivate, topn);
			path->custom_private = fprivate;
			path->path.pathkeys = NIL;	/* sale en el orden de la tabla */
			if (topn != NIL)
				path->path.rows = Min(ngroups, topn_k);

			/*
			 * Una búsqueda por fila en una tabla plana, sin armar tuplas ni
			 * llamar por fmgr.  Si los grupos no entran, casi todas las
			 * filas se escriben y se leen una vez más.
			 */
			/*
			 * El recorrido se cobra como en los demás caminos de este nodo:
			 * el 35% del scan, porque lee los mismos bytes sin armar tuplas.
			 */
			path->path.startup_cost = input_path->total_cost * 0.35 +
				cpu_operator_cost * nagg * 0.3 * in_rows;
			if (dist_expr != NULL)	/* el segundo nivel */
				path->path.startup_cost += cpu_operator_cost * nagg * flat_groups;
			if (flat_groups * (double) entry > hash_mem)
				path->path.startup_cost += 2.0 * seq_page_cost *
					(in_rows * (double) (entry / 2) / BLCKSZ);
			path->path.total_cost = path->path.startup_cost +
				cpu_tuple_cost * ngroups;

			/*
			 * Con top-N se recorren igual todos los grupos, pero salen k: lo
			 * que se ahorra es el Gather y el Sort de arriba.
			 */
			if (topn != NIL)
				path->path.total_cost = path->path.startup_cost +
					cpu_operator_cost * ngroups + cpu_tuple_cost * path->path.rows;

			/*
			 * La versión paralela no se reparte stripes sino grupos: cada
			 * participante toma un balde del hash de la clave, recorre la
			 * tabla entera y agrega solo las filas de su balde.  Los grupos
			 * salen completos y el Gather solo concatena: no hay Finalize,
			 * que con millones de grupos es justo lo que pesa (q34: 2,1 de
			 * los 5,5 s).  Lo que se paga es leer la tabla una vez por
			 * balde; leer es lo barato.
			 */
			if (input_rel->consider_parallel && output_rel->consider_parallel &&
				path->path.parallel_safe)
			{
				int			workers = compute_parallel_worker(input_rel,
															  input_rel->pages,
															  -1,
															  max_parallel_workers_per_gather);

				if (workers > 0)
				{
					CustomPath *pp = makeNode(CustomPath);
					GatherPath *gp;
					double		parts = workers + 1;
					double		scan = input_path->total_cost * 0.35;
					double		work = path->path.startup_cost - scan;
					double		rows = ngroups;
					double		per_tuple;

					memcpy(pp, path, sizeof(CustomPath));
					pp->path.parallel_aware = true;
					pp->path.parallel_workers = workers;
					pp->path.rows = clamp_row_est(ngroups / parts);
					if (topn != NIL)
					{
						/* Cada participante emite sus k mejores. */
						pp->path.rows = Min(pp->path.rows, topn_k);
						rows = pp->path.rows * parts;
					}
					pp->path.startup_cost = scan + work / parts;
					pp->path.total_cost = pp->path.startup_cost +
						cpu_tuple_cost * pp->path.rows;
					gp = create_gather_path(root, output_rel, (Path *) pp,
											output_rel->reltarget, NULL,
											&rows);

					/*
					 * parallel_tuple_cost está pensado para tuplas cualquiera
					 * cruzando la cola compartida, y acá suben filas angostas
					 * de grupos ya cerrados.  Medido en q33: 10M filas cuestan
					 * 0,75 s, y el camino serial, que cuesta 276K, tarda
					 * 6,2 s; o sea unas 0,0033 unidades por fila, menos que
					 * un cpu_tuple_cost.  Con el valor por omisión (0,1) el
					 * Gather costaba más que todo el resto del plan y el
					 * camino no se elegía, aunque era 1,6× más rápido.  Se
					 * cobra un cpu_tuple_cost por fila, o menos si así está
					 * configurado.
					 */
					per_tuple = Min(parallel_tuple_cost, cpu_tuple_cost);
					gp->path.startup_cost = pp->path.startup_cost +
						parallel_setup_cost;
					gp->path.total_cost = pp->path.total_cost +
						parallel_setup_cost + per_tuple * rows;
					add_path(output_rel, (Path *) gp);
				}
			}
			add_path(output_rel, (Path *) path);
		}

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
		if (partial_path)
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
		Expr	   *arg = ((TargetEntry *) linitial(ref->args))->expr;

		agg->inputtype = att->atttypid;
		agg->inputtyplen = att->attlen;
		agg->inputtypbyval = att->attbyval;
		agg->valtyplen = att->attlen;
		agg->valtypbyval = att->attbyval;

		if (!IsA(arg, Var))
		{
			/* Con parent NULL los Var se leen de ecxt_scantuple. */
			agg->argexpr = ExecInitExpr(arg, NULL);
			get_typlenbyval(exprType((Node *) arg), &agg->valtyplen,
							&agg->valtypbyval);
			dbe_agg_detect_textlen(agg, arg);
		}
	}
}

/*
 * ¿El argumento es length(col), char_length(col) o character_length(col)
 * con col de tipo text?  Las tres son textlen().
 *
 * textlen() cuenta caracteres con pg_mbstrlen_with_len(), que llama a
 * pg_mblen() por cada uno: en q28 eso es el 40% de la query.  En UTF-8 los
 * caracteres son los bytes que no son de continuación (10xxxxxx), y eso se
 * cuenta de a 8 bytes.  Vale porque lo guardado está validado: Postgres
 * rechaza UTF-8 inválido al entrar, por texto o por binario.  Con otra
 * codificación de varios bytes se evalúa la expresión.
 */
static void
dbe_agg_detect_textlen(DbeAggState *agg, Expr *arg)
{
	FuncExpr   *fe = (FuncExpr *) arg;
	Node	   *a;

	agg->textlen = false;
	if (!IsA(arg, FuncExpr) || list_length(fe->args) != 1)
		return;
	if (fe->funcid != F_LENGTH_TEXT && fe->funcid != F_CHAR_LENGTH_TEXT &&
		fe->funcid != F_CHARACTER_LENGTH_TEXT)
		return;
	a = (Node *) linitial(fe->args);
	if (!IsA(a, Var) || agg->inputtype != TEXTOID)
		return;
	if (GetDatabaseEncoding() == PG_UTF8)
		agg->len_bytes = false;
	else if (pg_database_encoding_max_length() == 1)
		agg->len_bytes = true;
	else
		return;
	agg->textlen = true;
}

/* Caracteres de un UTF-8 válido: los bytes que no son 10xxxxxx. */
static int32
dbe_utf8_chars(const char *p, Size len)
{
	Size		cont = 0;
	Size		i = 0;

	for (; i + 8 <= len; i += 8)
	{
		uint64		w;

		memcpy(&w, p + i, 8);
		/* Bit 7 prendido y bit 6 apagado, byte por byte. */
		cont += pg_popcount64(w & ~(w << 1) & UINT64CONST(0x8080808080808080));
	}
	for (; i < len; i++)
		cont += ((unsigned char) p[i] & 0xC0) == 0x80;
	return (int32) (len - cont);
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

	if (agg->is_count_star || agg->attnum == InvalidAttrNumber ||
		agg->argexpr != NULL)
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

/* ------------------------------------------------------------------------
 * Agregados precalculados (SMA)
 *
 * Por cada segmento de smallint, integer o bigint el catálogo guarda la
 * suma exacta de los no nulos, y siempre guarda null_count.  Con eso un
 * stripe visible entero aporta a sum, avg y count(col) sin leer un byte:
 * se arma su estado parcial y se combina con el acumulado, que es lo mismo
 * que hace el Finalize de un plan paralelo con los parciales de cada
 * worker.  Ref.: Moerkotte, "Small Materialized Aggregates", VLDB 1998.
 *
 * Solo para los agregados cuyo estado se conoce: sum(int2/int4) acumula en
 * int8, avg(int2/int4) en un int8[2] {cantidad, suma}, count(col) en int8.
 * sum(int8) y avg(int8) usan un estado interno (numeric/int128) y quedan
 * fila por fila.
 * ------------------------------------------------------------------------ */

#define DBE_SMA_NONE	0
#define DBE_SMA_SUM		1
#define DBE_SMA_AVG		2
#define DBE_SMA_COUNT	3

/*
 * ¿El argumento es `col + k` o `col - k`, con col y k smallint o integer?
 * Deja k y el rango del tipo del resultado.  Es la forma de q30: noventa
 * `sum(ResolutionWidth + i)`.
 */
static bool
dbe_agg_affine_arg(DbeAggState *agg, Expr *arg)
{
	OpExpr	   *op = (OpExpr *) arg;
	Node	   *l;
	Node	   *r;
	Const	   *k;
	bool		minus;
	int64		kv;
	Oid			restype;

	if (!IsA(arg, OpExpr) || list_length(op->args) != 2)
		return false;
	switch (op->opfuncid)
	{
		case F_INT2PL:
		case F_INT4PL:
		case F_INT24PL:
		case F_INT42PL:
			minus = false;
			break;
		case F_INT2MI:
		case F_INT4MI:
		case F_INT24MI:
		case F_INT42MI:
			minus = true;
			break;
		default:
			return false;
	}

	l = strip_implicit_coercions((Node *) linitial(op->args));
	r = strip_implicit_coercions((Node *) lsecond(op->args));
	if (IsA(l, Var) && IsA(r, Const))
		k = (Const *) r;
	else if (!minus && IsA(l, Const) && IsA(r, Var))
		k = (Const *) l;
	else
		return false;
	if (k->constisnull)
		return false;

	if (k->consttype == INT2OID)
		kv = DatumGetInt16(k->constvalue);
	else if (k->consttype == INT4OID)
		kv = DatumGetInt32(k->constvalue);
	else
		return false;

	restype = exprType((Node *) arg);
	if (restype == INT2OID)
	{
		agg->sma_lo = PG_INT16_MIN;
		agg->sma_hi = PG_INT16_MAX;
	}
	else if (restype == INT4OID)
	{
		agg->sma_lo = PG_INT32_MIN;
		agg->sma_hi = PG_INT32_MAX;
	}
	else
		return false;

	agg->sma_k = minus ? -kv : kv;
	agg->sma_affine = true;
	return true;
}

/* Un valor de smallint o integer del zone map, como int64. */
static int64
dbe_agg_int_of(DbeAggState *agg, const char *bytes)
{
	Datum		d = fetch_att(bytes, agg->inputtypbyval, agg->inputtyplen);

	return agg->inputtype == INT2OID ? DatumGetInt16(d) : DatumGetInt32(d);
}

static void
dbe_agg_detect_sma(DbeAggState *agg, Aggref *ref)
{
	Oid			aggfnoid = ref->aggfnoid;

	agg->sma = DBE_SMA_NONE;
	agg->sma_affine = false;
	if (agg->is_count_star || agg->is_distinct || agg->attnum == InvalidAttrNumber)
		return;
	if (agg->argexpr != NULL &&
		((agg->inputtype != INT2OID && agg->inputtype != INT4OID) ||
		 !dbe_agg_affine_arg(agg, ((TargetEntry *) linitial(ref->args))->expr)))
		return;

	switch (aggfnoid)
	{
		case F_SUM_INT2:
		case F_SUM_INT4:
			agg->sma = DBE_SMA_SUM;
			break;
		case F_AVG_INT2:
		case F_AVG_INT4:
			agg->sma = DBE_SMA_AVG;
			break;
		case F_COUNT_ANY:
			agg->sma = DBE_SMA_COUNT;
			break;
	}
}

/*
 * Suma un stripe al estado del agregado: `nonnull` valores no nulos que
 * suman `sum`.  Hace exactamente lo que harían las transiciones fila por
 * fila, con la misma detección de overflow que int8pl.
 */
static void
dbe_agg_apply_sma(DbeAggState *agg, int64 nonnull, int64 sum)
{
	int64		r;

	switch (agg->sma)
	{
		case DBE_SMA_COUNT:
			if (pg_add_s64_overflow(DatumGetInt64(agg->transvalue), nonnull, &r))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
			agg->transvalue = Int64GetDatum(r);
			break;

		case DBE_SMA_SUM:
			/* Sin valores no nulos el stripe no aporta: sum() sigue en NULL. */
			if (nonnull == 0)
				break;
			if (agg->transnull)
				r = sum;
			else if (pg_add_s64_overflow(DatumGetInt64(agg->transvalue), sum, &r))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
			agg->transvalue = Int64GetDatum(r);
			agg->transnull = false;
			agg->nothing_yet = false;
			break;

		case DBE_SMA_AVG:
		{
			/*
			 * El int8[2] de int4_avg_accum: se modifica en el lugar, igual
			 * que lo hace esa función cuando corre dentro de un agregado.
			 */
			ArrayType  *a = DatumGetArrayTypeP(agg->transvalue);
			int64	   *d = (int64 *) ARR_DATA_PTR(a);

			if (ARR_HASNULL(a) || ARR_SIZE(a) != ARR_OVERHEAD_NONULLS(1) + 2 * sizeof(int64))
				elog(ERROR, "dbengine: estado de avg con forma inesperada");
			d[0] += nonnull;
			d[1] += sum;
			break;
		}
	}
}

/*
 * ¿Se puede avanzar el stripe `idx` desde el catálogo?  Marca sma_now en
 * cada agregado que sí, y devuelve true si TODOS pudieron —entonces ya
 * quedaron avanzados y no hace falta leer el stripe—.  Si solo algunos,
 * esos se avanzan igual y el recorrido por fila los saltea.
 */
/*
 * El segmento de la columna del agregado i en el stripe idx.  Cada búsqueda
 * es un scan del catálogo, y q30 tiene noventa agregados sobre la MISMA
 * columna: se busca una vez y se copia.  have[i]: 0 sin buscar, 1 hallado,
 * -1 no hay.
 */
static bool
dbe_agg_stripe_segment(DbeVectorAggState *state, int idx, int i,
					   DbeSegDesc *segs, int8 *have, uint64 *rows)
{
	int			j;

	for (j = 0; j < i; j++)
		if (have[j] != 0 && state->aggs[j].attnum == state->aggs[i].attnum)
		{
			segs[i] = segs[j];
			have[i] = have[j];
			return have[i] > 0;
		}

	have[i] = dbe_cursor_stripe_segment(state->cursor, idx,
										state->aggs[i].attnum, &segs[i], rows)
		? 1 : -1;
	return have[i] > 0;
}

static bool
dbe_agg_stripe_sma(DbeVectorAggState *state, int idx)
{
	DbeSegDesc *segs = (DbeSegDesc *) palloc(sizeof(DbeSegDesc) * state->naggs);
	int8	   *have = (int8 *) palloc0(sizeof(int8) * state->naggs);
	int64	   *sums = (int64 *) palloc0(sizeof(int64) * state->naggs);
	uint64		rows = 0;
	bool		all = true;
	int			i;

	/*
	 * dbe_cursor_stripe_segment() ya exige que el stripe sea visible entero
	 * para el snapshot, así que con un solo agregado que pase alcanza para
	 * saber que el row_count del stripe vale como count(*).
	 */
	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];

		agg->sma_now = false;
		if (agg->is_count_star || agg->is_group)
			continue;
		if (agg->sma != DBE_SMA_NONE &&
			dbe_agg_stripe_segment(state, idx, i, segs, have, &rows) &&
			(agg->sma == DBE_SMA_COUNT || segs[i].has_sum ||
			 segs[i].null_count >= rows) &&
			(!agg->sma_affine || segs[i].null_count >= rows ||
			 (segs[i].has_minmax &&
			  dbe_agg_int_of(agg, segs[i].min_value) + agg->sma_k >= agg->sma_lo &&
			  dbe_agg_int_of(agg, segs[i].max_value) + agg->sma_k <= agg->sma_hi)))
			agg->sma_now = true;
		else
			all = false;
	}

	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];
		int64		sum = 0;

		/*
		 * count(*) sale del catálogo solo si el stripe entero se contesta
		 * así: si hay que leerlo, lo cuenta el recorrido por fila.
		 */
		if (agg->is_count_star)
		{
			if (all)
				agg->count += (int64) rows;
			continue;
		}
		if (!agg->sma_now)
			continue;

		if (segs[i].has_sum)
		{
			int			j;

			/* Misma columna que un agregado anterior: la misma suma. */
			for (j = 0; j < i; j++)
				if (state->aggs[j].sma_now &&
					state->aggs[j].attnum == agg->attnum)
					break;
			sums[i] = j < i ? sums[j]
				: DatumGetInt64(DirectFunctionCall1(numeric_int8,
													segs[i].sum_value));
			sum = sums[i];
		}
		/* sum(col + k) = sum(col) + k·count(col); n ≤ millones, k ≤ 2^31. */
		if (agg->sma_affine)
			sum += agg->sma_k * (int64) (rows - segs[i].null_count);
		dbe_agg_apply_sma(agg, (int64) (rows - segs[i].null_count), sum);
	}

	pfree(segs);
	pfree(have);
	pfree(sums);
	if (all)
	{
		state->rows_seen += rows;
		state->stripes_sma++;
	}
	return all;
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
		agg->transvalue = datumCopy(value, agg->valtypbyval, agg->valtyplen);
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

/*
 * Prepara la tabla propia.  El planner ya comprobó que cada agregado tiene
 * un estado que ella sabe llevar; si acá no, es que el plan y el código
 * dejaron de coincidir, y seguir daría resultados de otro agregado.
 */
/* Prepara el top-N con lo que dejó dbe_flat_topn_spec(). */
static void
dbe_agg_begin_topn(DbeVectorAggState *state, CustomScan *cscan, List *spec)
{
	List	   *pos = (List *) lsecond(spec);
	List	   *ops = (List *) lthird(spec);
	List	   *colls = (List *) lfourth(spec);
	List	   *nf = (List *) list_nth(spec, 4);
	DbeFlatAgg *out = state->flatdist != NULL ? state->flatdist : state->flatagg;
	int			k = intVal(linitial(spec));
	int			ns = list_length(pos);
	int			nk = state->ngroupcols;
	int			nst = dbe_flat_nstates(out);

	state->topn_k = k;
	state->topn_nsort = ns;
	state->topn_src = (int *) palloc(sizeof(int) * ns);
	state->topn_byval = (bool *) palloc(sizeof(bool) * ns);
	state->topn_len = (int16 *) palloc(sizeof(int16) * ns);
	state->topn_ssup = (SortSupport) palloc0(sizeof(SortSupportData) * ns);
	for (int j = 0; j < ns; j++)
	{
		int			p = list_nth_int(pos, j);
		TargetEntry *tle = (TargetEntry *) list_nth(cscan->custom_scan_tlist, p);
		DbeAggState *agg = &state->aggs[p];
		SortSupport ss = &state->topn_ssup[j];

		if (agg->is_const)
			elog(ERROR, "dbengine: top-N por una constante");
		if (agg->is_group)
			state->topn_src[j] = agg->group_idx;
		else
		{
			int			v = 0;

			/* Los agregados de la tabla que se emite van en el orden del tlist. */
			for (int i = 0; i < p; i++)
				if (!state->aggs[i].is_group)
					v++;
			state->topn_src[j] = -1 - v;
		}
		get_typlenbyval(exprType((Node *) tle->expr), &state->topn_len[j],
						&state->topn_byval[j]);
		ss->ssup_cxt = CurrentMemoryContext;
		ss->ssup_collation = list_nth_oid(colls, j);
		ss->ssup_nulls_first = list_nth_int(nf, j) != 0;
		PrepareSortSupportFromOrderingOp(list_nth_oid(ops, j), ss);
	}
	state->topn_heap = (int *) palloc(sizeof(int) * k);
	state->topn_keys = (Datum *) palloc(sizeof(Datum) * k * Max(nk, 1));
	state->topn_keynulls = (bool *) palloc(sizeof(bool) * k * Max(nk, 1));
	state->topn_states = (uint64 *) palloc(sizeof(uint64) * k * Max(nst, 1));
	state->topn_sv = (Datum *) palloc(sizeof(Datum) * k * ns);
	state->topn_svnull = (bool *) palloc(sizeof(bool) * k * ns);
	state->topn_cand = (Datum *) palloc(sizeof(Datum) * ns);
	state->topn_candnull = (bool *) palloc(sizeof(bool) * ns);
	state->topn_cxt = AllocSetContextCreate(CurrentMemoryContext,
											"dbengine top-N",
											ALLOCSET_DEFAULT_SIZES);
	state->topn_tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "dbengine top-N candidato",
											   ALLOCSET_SMALL_SIZES);
}

/* Compara los valores de orden de dos entradas, o de una con el candidato. */
static int
dbe_agg_topn_cmp(DbeVectorAggState *state, const Datum *a, const bool *an,
				 const Datum *b, const bool *bn)
{
	for (int j = 0; j < state->topn_nsort; j++)
	{
		int			r = ApplySortComparator(a[j], an[j], b[j], bn[j],
											&state->topn_ssup[j]);

		if (r != 0)
			return r;
	}
	return 0;
}

static inline int
dbe_agg_topn_cmp_slots(DbeVectorAggState *state, int x, int y)
{
	int			ns = state->topn_nsort;

	return dbe_agg_topn_cmp(state, &state->topn_sv[x * ns], &state->topn_svnull[x * ns],
							&state->topn_sv[y * ns], &state->topn_svnull[y * ns]);
}

/* El heap tiene arriba la entrada que va última en el orden. */
static void
dbe_agg_topn_sift_up(DbeVectorAggState *state, int i)
{
	int		   *h = state->topn_heap;

	while (i > 0)
	{
		int			parent = (i - 1) / 2;
		int			t;

		if (dbe_agg_topn_cmp_slots(state, h[i], h[parent]) <= 0)
			break;
		t = h[i];
		h[i] = h[parent];
		h[parent] = t;
		i = parent;
	}
}

static void
dbe_agg_topn_sift_down(DbeVectorAggState *state)
{
	int		   *h = state->topn_heap;
	int			n = state->topn_n;
	int			i = 0;

	for (;;)
	{
		int			l = 2 * i + 1;
		int			m = i;
		int			t;

		if (l < n && dbe_agg_topn_cmp_slots(state, h[l], h[m]) > 0)
			m = l;
		if (l + 1 < n && dbe_agg_topn_cmp_slots(state, h[l + 1], h[m]) > 0)
			m = l + 1;
		if (m == i)
			break;
		t = h[i];
		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

/* Guarda en la entrada `e` el grupo cuyos valores de orden son el candidato. */
static void
dbe_agg_topn_store(DbeVectorAggState *state, DbeFlatAgg *out, int e,
				   const uint64 *st, bool replacing)
{
	int			nk = state->ngroupcols;
	int			ns = state->topn_nsort;
	int			nst = dbe_flat_nstates(out);
	MemoryContext old = MemoryContextSwitchTo(state->topn_cxt);

	for (int g = 0; g < nk; g++)
	{
		Datum	   *d = &state->topn_keys[e * nk + g];

		if (replacing && !state->group_typbyval[g] && !state->topn_keynulls[e * nk + g])
			pfree(DatumGetPointer(*d));
		state->topn_keynulls[e * nk + g] = state->flat_keynulls[g];
		*d = state->flat_keynulls[g] ? (Datum) 0
			: datumCopy(state->flat_keys[g], state->group_typbyval[g],
						state->group_typlen[g]);
	}
	for (int j = 0; j < ns; j++)
	{
		Datum	   *d = &state->topn_sv[e * ns + j];

		if (replacing && !state->topn_byval[j] && !state->topn_svnull[e * ns + j])
			pfree(DatumGetPointer(*d));
		state->topn_svnull[e * ns + j] = state->topn_candnull[j];
		*d = state->topn_candnull[j] ? (Datum) 0
			: datumCopy(state->topn_cand[j], state->topn_byval[j],
						state->topn_len[j]);
	}
	memcpy(&state->topn_states[(Size) e * nst], st, sizeof(uint64) * nst);
	MemoryContextSwitchTo(old);
}

/*
 * Recorre los grupos de la tabla que se emite y se queda con los k
 * mejores.  De cada grupo se calculan solo los valores de orden; el resto
 * de los agregados, al emitir.
 */
static void
dbe_agg_topn_fill(DbeVectorAggState *state)
{
	DbeFlatAgg *out = state->flatdist != NULL ? state->flatdist : state->flatagg;
	int			ns = state->topn_nsort;
	const uint64 *st;

	MemoryContextReset(state->topn_cxt);
	state->topn_n = 0;
	state->topn_emit = 0;
	while (dbe_flat_next_states(out, state->flat_keys, state->flat_keynulls, &st))
	{
		MemoryContext old;

		MemoryContextReset(state->topn_tmpcxt);
		old = MemoryContextSwitchTo(state->topn_tmpcxt);
		for (int j = 0; j < ns; j++)
		{
			int			src = state->topn_src[j];

			if (src >= 0)
			{
				state->topn_cand[j] = state->flat_keys[src];
				state->topn_candnull[j] = state->flat_keynulls[src];
			}
			else
				state->topn_cand[j] = dbe_flat_final(out, -1 - src, st,
													 &state->topn_candnull[j]);
		}
		MemoryContextSwitchTo(old);

		if (state->topn_n < state->topn_k)
		{
			int			e = state->topn_n++;

			dbe_agg_topn_store(state, out, e, st, false);
			state->topn_heap[e] = e;
			dbe_agg_topn_sift_up(state, e);
		}
		else
		{
			int			top = state->topn_heap[0];

			if (dbe_agg_topn_cmp(state, state->topn_cand, state->topn_candnull,
								 &state->topn_sv[top * ns],
								 &state->topn_svnull[top * ns]) >= 0)
				continue;
			dbe_agg_topn_store(state, out, top, st, true);
			dbe_agg_topn_sift_down(state);
		}
	}
}

/* La próxima entrada del top-N, en flat_keys y flat_outs.  false al final. */
static bool
dbe_agg_topn_next(DbeVectorAggState *state)
{
	DbeFlatAgg *out = state->flatdist != NULL ? state->flatdist : state->flatagg;
	int			nk = state->ngroupcols;
	int			nst = dbe_flat_nstates(out);
	int			e;
	MemoryContext old;
	int			v = 0;

	if (state->topn_emit >= state->topn_n)
		return false;
	e = state->topn_emit++;
	memcpy(state->flat_keys, &state->topn_keys[e * nk], sizeof(Datum) * nk);
	memcpy(state->flat_keynulls, &state->topn_keynulls[e * nk], sizeof(bool) * nk);
	MemoryContextReset(state->topn_tmpcxt);
	old = MemoryContextSwitchTo(state->topn_tmpcxt);
	for (int i = 0; i < state->naggs; i++)
	{
		if (state->aggs[i].is_group)
			continue;
		state->flat_outs[v] = dbe_flat_final(out, v, &state->topn_states[(Size) e * nst],
											 &state->flat_outnulls[v]);
		v++;
	}
	MemoryContextSwitchTo(old);
	return true;
}

static void
dbe_agg_begin_flat(DbeVectorAggState *state, CustomScan *cscan, int eflags)
{
	DbeFlatAggDef *defs = (DbeFlatAggDef *)
		palloc0(sizeof(DbeFlatAggDef) * Max(state->naggs, 1));
	DbeFlatAggDef *defs2 = (DbeFlatAggDef *)
		palloc0(sizeof(DbeFlatAggDef) * Max(state->naggs, 1));
	int		   *map = (int *) palloc0(sizeof(int) * Max(state->naggs, 1));
	int16		keylen[DBE_FLAT_MAX_KEYS];
	int			nkeys = state->ngroupcols;
	int			n = 0;
	int			n2 = 0;
	int			off = 0;
	int			words2 = 0;
	int			i = 0;
	ListCell   *lc;
	double		est = (double) intVal(list_nth(cscan->custom_private, 7));
	Size		budget = get_hash_memory_limit();

	state->flat_dist = -1;
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Aggref	   *ref = (Aggref *) tle->expr;
		AttrNumber	da;

		/* `aggs` va paralelo a custom_scan_tlist. */
		i++;
		if (!IsA(ref, Aggref))
			continue;
		if (ref->aggdistinct != NIL)
		{
			if (!dbe_flat_distinct_ok(ref, 0, &da))
				elog(ERROR, "dbengine: el count(DISTINCT) no sirve para la tabla propia");
			if (state->flat_dist < 0)
				state->flat_dist = i - 1;
			defs2[n2].kind = DBE_FLAT_COUNT_STAR;
			defs2[n2].vallen = 0;
			map[n2] = -1;
		}
		else
		{
			if (!dbe_flat_agg_def(ref, 0, &defs[n]))
				elog(ERROR, "dbengine: el agregado %d no sirve para la tabla propia",
					 n + 1);
			defs2[n2] = defs[n];
			map[n2] = off;
			off += dbe_flat_state_words(defs[n].kind);
			n++;
		}
		words2 += dbe_flat_state_words(defs2[n2].kind);
		n2++;
	}

	/* Un EXPLAIN sin ANALYZE no ejecuta: no hace falta la tabla. */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		pfree(defs);
		pfree(defs2);
		pfree(map);
		return;
	}

	memcpy(keylen, state->group_typlen, sizeof(int16) * nkeys);
	if (state->flat_dist >= 0)
	{
		/*
		 * El segundo nivel suele ser chico —q10: 3.586 regiones contra
		 * millones de pares región, usuario—: se lleva un cuarto de la
		 * memoria.
		 */
		keylen[nkeys++] = state->aggs[state->flat_dist].inputtyplen;
		state->flatdist = dbe_flat_create(state->ngroupcols, state->group_typlen,
										  n2, defs2, budget / 4, est);
		state->flat_map = map;
		state->flat_defs2 = defs2;
		state->flat_ndefs2 = n2;
		state->flat_states2 = (uint64 *) palloc0(sizeof(uint64) * Max(words2, 1));
		budget -= budget / 4;
	}
	else
	{
		pfree(defs2);
		pfree(map);
	}
	state->flatagg = dbe_flat_create(nkeys, keylen, n, defs, budget, est);
	state->flat_filled = false;
	state->flat_keys = (Datum *) palloc0(sizeof(Datum) * nkeys);
	state->flat_keynulls = (bool *) palloc0(sizeof(bool) * nkeys);
	state->flat_vals = (Datum *) palloc0(sizeof(Datum) * Max(n, 1));
	state->flat_valnulls = (bool *) palloc0(sizeof(bool) * Max(n, 1));
	state->flat_outs = (Datum *) palloc0(sizeof(Datum) * Max(n2, 1));
	state->flat_outnulls = (bool *) palloc0(sizeof(bool) * Max(n2, 1));
	pfree(defs);

	if (list_length(cscan->custom_private) > 8 &&
		list_nth(cscan->custom_private, 8) != NIL)
		dbe_agg_begin_topn(state, cscan, (List *) list_nth(cscan->custom_private, 8));
}

/*
 * Con count(DISTINCT): pasa los grupos del primer nivel al segundo, que es
 * el que se emite.  Cada uno aporta sus estados a los agregados comunes, y
 * 1 a cada count(DISTINCT) si el valor no es nulo.
 */
static void
dbe_agg_flat_distinct(DbeVectorAggState *state)
{
	int			nk = state->ngroupcols;
	const uint64 *st;

	dbe_flat_input_done(state->flatagg);
	while (dbe_flat_next_states(state->flatagg, state->flat_keys,
								state->flat_keynulls, &st))
	{
		int			w = 0;

		for (int j = 0; j < state->flat_ndefs2; j++)
		{
			const DbeFlatAggDef *d = &state->flat_defs2[j];
			int			nw;

			if (state->flat_map[j] < 0)
			{
				state->flat_states2[w++] = state->flat_keynulls[nk] ? 0 : 1;
				continue;
			}
			nw = dbe_flat_state_words(d->kind);
			memcpy(&state->flat_states2[w], &st[state->flat_map[j]],
				   sizeof(uint64) * nw);
			w += nw;
		}
		dbe_flat_add_states(state->flatdist, state->flat_keys,
							state->flat_keynulls, state->flat_states2);
	}
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

		if (IsA(tle->expr, Const))
		{
			agg->is_group = true;
			agg->is_const = true;
			agg->group_idx = -1;
			agg->constvalue = ((Const *) tle->expr)->constvalue;
			agg->constisnull = ((Const *) tle->expr)->constisnull;
			continue;
		}
		if (!IsA(tle->expr, Aggref))
		{
			/* Columna de agrupación: no agrega, su valor sale de la clave. */
			agg->is_group = true;
			agg->group_attnum = (AttrNumber) lfirst_int(lc2);
			agg->group_idx = state->ngroupcols++;
			if (!IsA(tle->expr, Var))
				agg->group_expr = tle->expr;
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
			TupleDescInitEntryCollation(ddesc, 1, coll);
			dbe_tupdesc_finalize(ddesc);

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

			if (!agg->is_group || agg->is_const)
				continue;

			g = agg->group_idx;
			state->groupcols[g] = agg->group_attnum;
			att = TupleDescAttr(desc, agg->group_attnum - 1);
			if (agg->group_expr != NULL)
			{
				Oid			typid = exprType((Node *) agg->group_expr);

				/* Con parent NULL los Var se leen de ecxt_scantuple. */
				if (state->group_exprs == NULL)
				{
					state->group_exprs = (ExprState **)
						palloc0(sizeof(ExprState *) * state->ngroupcols);
					state->key_econtext = CreateExprContext(estate);
				}
				state->group_exprs[g] = ExecInitExpr(agg->group_expr, NULL);
				get_typlenbyval(typid, &state->group_typlen[g],
								&state->group_typbyval[g]);
				state->group_collation[g] = exprCollation((Node *) agg->group_expr);
				tc = lookup_type_cache(typid, TYPECACHE_EQ_OPR_FINFO);
			}
			else
			{
				state->group_typlen[g] = att->attlen;
				state->group_typbyval[g] = att->attbyval;
				state->group_collation[g] = att->attcollation;

				/*
				 * La igualdad es la del opclass btree por defecto del tipo,
				 * que es la misma con la que se ordenó.  Tiene que serlo: si
				 * el corte de grupo usara otra noción de igualdad que el
				 * orden, dos filas "iguales" podrían quedar separadas.
				 */
				tc = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO);
			}
			if (!OidIsValid(tc->eq_opr_finfo.fn_oid))
				elog(ERROR, "dbengine: el tipo %u no tiene igualdad btree",
					 tc->type_id);
			fmgr_info_copy(&state->group_eq[g], &tc->eq_opr_finfo,
						   CurrentMemoryContext);
		}

		state->group_cxt = AllocSetContextCreate(CurrentMemoryContext,
												 "dbengine group key",
												 ALLOCSET_SMALL_SIZES);
		state->have_group = false;
		state->in_stripe = false;

		state->hashed = (intVal(list_nth(cscan->custom_private, 6)) != 0);
		state->flat = (intVal(list_nth(cscan->custom_private, 6)) == 2);
		if (state->flat)
			dbe_agg_begin_flat(state, cscan, eflags);
		else if (state->hashed)
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
				TupleDescInitEntryCollation(gdesc, (AttrNumber) (i + 1),
											att->attcollation);
				keycols[i] = (AttrNumber) (i + 1);
				colls[i] = att->attcollation;
			}
			dbe_tupdesc_finalize(gdesc);

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
		dbe_agg_detect_sma(agg, (Aggref *) tle->expr);
		if (agg->sma != DBE_SMA_NONE)
			state->sma_any = true;
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
			state->qual = dbe_dictqual_build(quals,
											 (Index) intVal(lthird(cscan->custom_private)),
											 desc, NULL, estate);

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
		state->qual_lazy = (bool *) palloc0(sizeof(bool) * Max(state->nqualcols, 1));

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
			state->qual_lazy[i] = dbe_dictqual_lazy_column(state->qual, a);
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
/*
 * El slot contra el que se evalúa el WHERE, listo y puesto en el
 * ExprContext.
 *
 * Se prepara una sola vez.  Es virtual y nadie lo materializa, así que
 * después alcanza con pisar las columnas del predicado: las demás quedan
 * en NULL para siempre, que es correcto porque el predicado no las mira.
 * Limpiar y rearmar el slot por fila costaba un memset de natts más dos
 * llamadas, por cada fila candidata.
 */
static pg_attribute_always_inline TupleTableSlot *
dbe_agg_qual_slot(DbeVectorAggState *state)
{
	TupleTableSlot *slot = state->rel_slot;

	if (TTS_EMPTY(slot))
	{
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		ExecStoreVirtualTuple(slot);
	}
	state->css.ss.ps.ps_ExprContext->ecxt_scantuple = slot;
	return slot;
}

/*
 * El WHERE de un stripe entero, con los códigos del diccionario: ver
 * dbe_dictqual_prefilter().  NULL si no hay WHERE o no se pudo decidir
 * nada por código; si no, un byte por fila, que vale hasta el próximo
 * stripe.
 */
static uint8 *
dbe_agg_prefilter(DbeVectorAggState *state, uint64 nrows)
{
	if (state->qual == NULL)
		return NULL;
	if (state->sel_cap < nrows)
	{
		if (state->sel != NULL)
			pfree(state->sel);
		state->sel = (uint8 *) MemoryContextAlloc(GetMemoryChunkContext(state),
												  nrows);
		state->sel_cap = nrows;
	}
	dbe_agg_qual_slot(state);
	if (!dbe_dictqual_prefilter(state->qual, state->cursor, (uint32) nrows,
								state->sel, state->css.ss.ps.ps_ExprContext))
		return NULL;
	return state->sel;
}

/* Carga en el slot una columna perezosa: ver dbe_dictqual_lazy_column(). */
static void
dbe_agg_fill_qualcol(void *arg, AttrNumber attnum)
{
	DbeVectorAggState *state = (DbeVectorAggState *) arg;
	TupleTableSlot *slot = state->rel_slot;
	int			i;

	for (i = 0; i < state->nqualcols; i++)
	{
		if (state->qualcols[i] != attnum || !state->qualvec_ok[i])
			continue;
		dbe_vec_value(&state->qualvecs[i], state->qual_row,
					  state->qual_typbyval[i], state->qual_typlen[i],
					  &slot->tts_values[attnum - 1],
					  &slot->tts_isnull[attnum - 1]);
		return;
	}
}

static bool
dbe_agg_row_passes(DbeVectorAggState *state, uint64 row)
{
	TupleTableSlot *slot = dbe_agg_qual_slot(state);
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	int			i;

	for (i = 0; i < state->nqualcols; i++)
	{
		AttrNumber	a = state->qualcols[i];

		if (!state->qualvec_ok[i] || state->qual_lazy[i])
			continue;			/* agregada después (queda NULL), o perezosa */

		dbe_vec_value(&state->qualvecs[i], row, state->qual_typbyval[i],
					  state->qual_typlen[i], &slot->tts_values[a - 1],
					  &slot->tts_isnull[a - 1]);
	}

	econtext->ecxt_scantuple = slot;
	state->qual_row = row;
	return dbe_dictqual_eval(state->qual, state->cursor, row, econtext,
							 dbe_agg_fill_qualcol, state);
}

/*
 * ¿Pasa la fila el WHERE?  Con el byte del prefiltro del stripe si lo hay
 * (`sel`, de dbe_agg_prefilter()), y si no, o si el byte no alcanza, fila
 * por fila.
 */
static pg_attribute_always_inline bool
dbe_agg_row_selected(DbeVectorAggState *state, const uint8 *sel, uint64 row)
{
	if (state->qual == NULL)
		return true;
	if (sel != NULL && sel[row] != DBE_SEL_CHECK)
		return sel[row] == DBE_SEL_PASS;
	return dbe_agg_row_passes(state, row);
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
		dbe_agg_value(state, agg, row, &value, &isnull);
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

/*
 * El valor que le llega a la transición en la fila `row`: el de la columna,
 * o el de la expresión evaluada sobre ella.
 *
 * El contexto por tupla se vacía antes de evaluar: el valor de la fila
 * anterior ya lo consumió dbe_agg_advance(), que copia lo que se queda.
 */
static void
dbe_agg_value(DbeVectorAggState *state, DbeAggState *agg, uint64 row,
			  Datum *value, bool *isnull)
{
	TupleTableSlot *slot;
	ExprContext *econtext;

	if (!agg->has_vector)
		dbe_agg_missing_value(state, agg->attnum, value, isnull);
	else
		dbe_vec_value(&agg->vec, row, agg->inputtypbyval, agg->inputtyplen,
					  value, isnull);
	if (agg->argexpr == NULL)
		return;

	if (agg->textlen)
	{
		struct varlena *v = (struct varlena *) DatumGetPointer(*value);

		if (*isnull)
			return;				/* textlen() es estricta */
		/* Se guarda sin comprimir (columnar_write.c), pero por las dudas. */
		if (!VARATT_IS_EXTERNAL(v) && !VARATT_IS_COMPRESSED(v))
		{
			Size		len = VARSIZE_ANY_EXHDR(v);

			*value = Int32GetDatum(agg->len_bytes ? (int32) len
								   : dbe_utf8_chars(VARDATA_ANY(v), len));
			return;
		}
	}

	slot = state->rel_slot;
	econtext = state->css.ss.ps.ps_ExprContext;
	if (TTS_EMPTY(slot))
	{
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		ExecStoreVirtualTuple(slot);
	}
	slot->tts_values[agg->attnum - 1] = *value;
	slot->tts_isnull[agg->attnum - 1] = *isnull;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;
	*value = ExecEvalExprSwitchContext(agg->argexpr, econtext, isnull);
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
		state->stripe_sel = dbe_agg_prefilter(state, nrows);
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
			if (!dbe_agg_row_selected(state, state->stripe_sel, row))
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
					dbe_agg_value(state, agg, row, &value, &isnull);
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

/*
 * El próximo stripe a recorrer, sin cargarlo: -1 si no quedan.  Serial,
 * el siguiente en orden; en paralelo, el que toque del contador compartido.
 */
static int
dbe_agg_claim_index(DbeVectorAggState *state)
{
	uint64		idx;

	if (state->shared == NULL)
		idx = (uint64) state->next_index++;
	else
		idx = pg_atomic_fetch_add_u64(&state->shared->next_stripe, 1);

	return idx < (uint64) dbe_cursor_nstripes(state->cursor) ? (int) idx : -1;
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

/* Vacía las tablas propias: rescan, otro balde, o fin con un LIMIT. */
static void
dbe_agg_flat_reset(DbeVectorAggState *state)
{
	dbe_flat_reset(state->flatagg);
	if (state->flatdist != NULL)
		dbe_flat_reset(state->flatdist);
}

/*
 * Las cifras del EXPLAIN.  Con count(DISTINCT) los grupos son los del
 * segundo nivel, y el volcado y la memoria, los de los dos.
 */
static void
dbe_agg_flat_stats(DbeVectorAggState *state, uint64 *groups, int *parts,
				   uint64 *spilled, Size *peak)
{
	dbe_flat_stats(state->flatagg, groups, parts, spilled, peak);
	if (state->flatdist != NULL)
	{
		int			p2;
		uint64		s2;
		Size		k2;

		dbe_flat_stats(state->flatdist, groups, &p2, &s2, &k2);
		*parts += p2;
		*spilled += s2;
		*peak += k2;
	}
}

/*
 * Las claves de la fila, en flat_keys, cuando alguna es una expresión del
 * GROUP BY: esa se calcula sobre su columna, cargada en el slot de la
 * relación.  Una columna que el stripe no tiene vale su DEFAULT.  Va
 * aparte del lazo de las columnas solas, que es el de siempre: con la
 * rama adentro, q18 tardaba 11% más.
 */
static pg_noinline void
dbe_agg_flat_keys_expr(DbeVectorAggState *state, uint64 row)
{
	TupleTableSlot *slot = dbe_agg_qual_slot(state);

	ResetExprContext(state->key_econtext);
	state->key_econtext->ecxt_scantuple = slot;
	for (int g = 0; g < state->ngroupcols; g++)
	{
		Datum	   *key = &state->flat_keys[g];
		bool	   *isnull = &state->flat_keynulls[g];
		AttrNumber	a = state->groupcols[g];
		Form_pg_attribute att;

		if (state->group_exprs[g] == NULL)
		{
			if (!state->groupvec_ok[g])
				dbe_agg_missing_value(state, a, key, isnull);
			else
				dbe_vec_value(&state->groupvecs[g], row,
							  state->group_typbyval[g],
							  state->group_typlen[g], key, isnull);
			continue;
		}

		if (!state->groupvec_ok[g])
			dbe_agg_missing_value(state, a, &slot->tts_values[a - 1],
								  &slot->tts_isnull[a - 1]);
		else
		{
			att = TupleDescAttr(slot->tts_tupleDescriptor, a - 1);
			dbe_vec_value(&state->groupvecs[g], row, att->attbyval,
						  att->attlen, &slot->tts_values[a - 1],
						  &slot->tts_isnull[a - 1]);
		}
		*key = ExecEvalExprSwitchContext(state->group_exprs[g],
										 state->key_econtext, isnull);
	}
}

/*
 * Una corrida de filas seguidas con las mismas claves (de ancho fijo), que
 * va a la tabla una sola vez.  La tabla de ClickBench está ordenada por
 * CounterID, fecha y UserID: en q9, de 10M filas 8,4M repiten el par
 * (RegionID, UserID) de la anterior, y cuando el par no entraba en la tabla
 * se volcaba una vez por fila.  Una corrida de una fila va como fila; una
 * más larga, como estados.
 */
typedef struct DbeFlatRun
{
	int64		len;
	Datum		keys[DBE_FLAT_MAX_KEYS];
	bool		nulls[DBE_FLAT_MAX_KEYS];
	Datum	   *vals;			/* los de la primera fila */
	bool	   *valnulls;
	uint64	   *states;			/* desde la segunda */
} DbeFlatRun;

static void
dbe_agg_flat_flush_run(DbeVectorAggState *state, DbeFlatRun *run)
{
	if (run->len == 1)
		dbe_flat_add(state->flatagg, run->keys, run->nulls, run->vals,
					 run->valnulls);
	else if (run->len > 1)
		dbe_flat_add_states(state->flatagg, run->keys, run->nulls,
							run->states);
	run->len = 0;
}

/* Suma la fila de flat_keys/flat_vals a la corrida, o empieza otra. */
static void
dbe_agg_flat_run_add(DbeVectorAggState *state, DbeFlatRun *run, int nk,
					 int nvals)
{
	DbeFlatAgg *fa = state->flatagg;

	if (run->len > 0 &&
		memcmp(run->nulls, state->flat_keynulls, sizeof(bool) * nk) == 0)
	{
		int			i;

		for (i = 0; i < nk; i++)
			if (!run->nulls[i] && run->keys[i] != state->flat_keys[i])
				break;
		if (i == nk)
		{
			if (run->len == 1)
			{
				memset(run->states, 0, sizeof(uint64) * dbe_flat_nstates(fa));
				dbe_flat_accum(fa, run->states, run->vals, run->valnulls);
			}
			dbe_flat_accum(fa, run->states, state->flat_vals,
						   state->flat_valnulls);
			run->len++;
			return;
		}
	}
	dbe_agg_flat_flush_run(state, run);
	memcpy(run->keys, state->flat_keys, sizeof(Datum) * nk);
	memcpy(run->nulls, state->flat_keynulls, sizeof(bool) * nk);
	memcpy(run->vals, state->flat_vals, sizeof(Datum) * nvals);
	memcpy(run->valnulls, state->flat_valnulls, sizeof(bool) * nvals);
	run->len = 1;
}

/*
 * Llena la tabla propia con todo el scan.  Es el mismo recorrido que
 * dbe_agg_fill_hash() sin el corte por memoria: la tabla vuelca sola.
 *
 * Con `bucket` >= 0 (en paralelo) se recorre la tabla entera y se agregan
 * solo las filas cuya clave cae en ese balde de los `nbuckets`.
 */
static void
dbe_agg_fill_flat(DbeVectorAggState *state, int bucket, uint32 nbuckets)
{
	int			i;
	int64	   *dict_cache = NULL;
	uint32_t	dict_size = 0;
	uint32_t	dict_cap = 0;
	int			nstates = dbe_flat_nstates(state->flatagg);
	uint64	   *spill_states = NULL;	/* nstates por código volcado */
	uint32_t   *spill_code = NULL;	/* el código de cada uno; dict_size = NULL */
	uint32_t	nspill = 0;

	/*
	 * Varias claves con alguna de texto: el hash de cada texto con
	 * diccionario se calcula una vez por código, y el de la fila combina esos
	 * con los de las otras claves.  th_size[i] es el tamaño del diccionario
	 * de la clave i en el stripe, 0 si no tiene o no es de texto.
	 */
	bool		multi_text = false;
	uint64	   *th_hash[DBE_FLAT_MAX_KEYS] = {0};
	bool	   *th_known[DBE_FLAT_MAX_KEYS] = {0};
	uint32_t	th_cap[DBE_FLAT_MAX_KEYS] = {0};
	uint32_t	th_size[DBE_FLAT_MAX_KEYS] = {0};
	uint64		texthash[DBE_FLAT_MAX_KEYS];

	/*
	 * Con count(DISTINCT) la columna es una clave más, la última, y el
	 * balde sale del hash de las otras: los grupos que se emiten son los de
	 * `flatdist`.
	 */
	DbeAggState *dist = state->flat_dist >= 0 ? &state->aggs[state->flat_dist] : NULL;
	uint8	   *sel;
	bool		use_runs;
	DbeFlatRun	run;
	int			nk = state->ngroupcols + (dist != NULL);
	uint32		groupmask = ((uint32) 1 << state->ngroupcols) - 1;
	int16		ktyplen[DBE_FLAT_MAX_KEYS];
	DbeColumnVector *kvec[DBE_FLAT_MAX_KEYS];

	for (i = 0; i < state->ngroupcols; i++)
	{
		ktyplen[i] = state->group_typlen[i];
		kvec[i] = &state->groupvecs[i];
	}
	if (dist != NULL)
	{
		ktyplen[nk - 1] = dist->inputtyplen;
		kvec[nk - 1] = &dist->vec;
	}
	if (nk > 1)
		for (i = 0; i < nk; i++)
			if (ktyplen[i] == -1)
				multi_text = true;

	/* Las corridas, con claves de ancho fijo: se comparan como Datum. */
	use_runs = true;
	for (i = 0; i < nk; i++)
		if (ktyplen[i] == -1)
			use_runs = false;
	if (use_runs)
	{
		run.len = 0;
		run.vals = (Datum *) palloc(sizeof(Datum) * Max(state->naggs, 1));
		run.valnulls = (bool *) palloc(sizeof(bool) * Max(state->naggs, 1));
		run.states = (uint64 *) palloc(sizeof(uint64) * Max(nstates, 1));
	}

	for (;;)
	{
		DbeStripeTake take;
		uint64		nrows;
		bool		all_visible;
		uint64		row;

		CHECK_FOR_INTERRUPTS();

		/* Con un balde se recorre la tabla entera: los grupos son los repartidos. */
		if (bucket >= 0)
			take = dbe_cursor_next_stripe(state->cursor) ? DBE_TAKE_STRIPE
				: DBE_TAKE_DONE;
		else
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
		 * Una sola clave de texto con diccionario: como en
		 * dbe_agg_fill_hash(), el grupo se busca una vez por código y las
		 * demás filas con el mismo código van directo a la entrada.
		 *
		 * Si el grupo no entra en la tabla, sus filas del stripe se suman a
		 * un estado parcial propio, y se vuelca un registro por código al
		 * terminar el stripe en vez de uno por fila: en q34 una URL aparece
		 * unas 5 veces por stripe, y el texto se escribía cada vez.
		 *
		 * dict_cache: >= 0 índice del grupo, -2 todavía no se vio, -1 es de
		 * otro balde, y <= -3 el estado parcial número -3 - v.
		 */
		dict_size = 0;
		if (dist == NULL && state->group_exprs == NULL &&
			state->ngroupcols == 1 && state->group_typlen[0] == -1 &&
			state->groupvec_ok[0] && state->groupvecs[0].reader != NULL)
		{
			int32_t		is_dict = 0;
			uint32_t	n = 0;
			DbeStatus	st = dbe_segreader_dict(state->groupvecs[0].reader,
												&is_dict, &n);

			dbe_error_if_not_ok(&st, "consultando el diccionario del segmento");
			if (is_dict && n > 0)
			{
				if (dict_cap < n + 1)
				{
					if (dict_cache != NULL)
					{
						pfree(dict_cache);
						pfree(spill_states);
						pfree(spill_code);
					}
					dict_cap = n + 1;
					dict_cache = (int64 *) palloc(sizeof(int64) * dict_cap);
					spill_states = (uint64 *)
						palloc(sizeof(uint64) * Max(nstates, 1) * dict_cap);
					spill_code = (uint32_t *) palloc(sizeof(uint32_t) * dict_cap);
				}
				dict_size = n;
				for (uint32_t c = 0; c <= n; c++)
					dict_cache[c] = -2;
			}
		}

		sel = dbe_agg_prefilter(state, nrows);

		for (i = 0; multi_text && i < nk; i++)
		{
			int32_t		is_dict = 0;
			uint32_t	n = 0;
			bool		ok = i < state->ngroupcols ? state->groupvec_ok[i]
				: dist->has_vector;

			th_size[i] = 0;
			if (ktyplen[i] != -1 || !ok || kvec[i]->reader == NULL)
				continue;
			{
				DbeStatus	st = dbe_segreader_dict(kvec[i]->reader,
													&is_dict, &n);

				dbe_error_if_not_ok(&st, "consultando el diccionario del segmento");
			}
			if (!is_dict || n == 0)
				continue;
			if (th_cap[i] < n)
			{
				if (th_hash[i] != NULL)
				{
					pfree(th_hash[i]);
					pfree(th_known[i]);
				}
				th_cap[i] = n;
				th_hash[i] = (uint64 *) palloc(sizeof(uint64) * n);
				th_known[i] = (bool *) palloc(sizeof(bool) * n);
			}
			memset(th_known[i], 0, sizeof(bool) * n);
			th_size[i] = n;
		}

		for (row = dbe_cursor_next_live_row(state->cursor, 0); row < nrows;
			 row = dbe_cursor_next_live_row(state->cursor, row + 1))
		{
			int			v = 0;
			int64	   *cached = NULL;
			uint64		h = 0;

			if (!all_visible &&
				!dbe_cursor_row_is_visible(state->cursor, row))
				continue;
			if (!dbe_agg_row_selected(state, sel, row))
			{
				/* Cada balde ve las mismas: las cuenta uno solo. */
				if (bucket <= 0)
					state->rows_filtered++;
				continue;
			}

			if (dict_size > 0)
			{
				uint32_t	code;
				int32_t		code_null;
				DbeStatus	st = dbe_segreader_code(state->groupvecs[0].reader,
													(uint32_t) row, &code,
													&code_null);

				dbe_error_if_not_ok(&st, "leyendo el código del diccionario");
				cached = &dict_cache[code_null ? dict_size : code];
				if (*cached == -1)
					continue;	/* de otro balde */
			}

			if (state->group_exprs == NULL)
			{
				for (i = 0; i < state->ngroupcols && (cached == NULL || *cached == -2); i++)
				{
					/* Columna que el stripe no tiene: su valor por omisión. */
					if (!state->groupvec_ok[i])
						dbe_agg_missing_value(state, state->groupcols[i],
											  &state->flat_keys[i],
											  &state->flat_keynulls[i]);
					else
						dbe_vec_value(&state->groupvecs[i], row,
									  state->group_typbyval[i],
									  state->group_typlen[i],
									  &state->flat_keys[i],
									  &state->flat_keynulls[i]);
				}
			}
			else
				dbe_agg_flat_keys_expr(state, row);
			if (dist != NULL)
			{
				if (!dist->has_vector)
					dbe_agg_missing_value(state, dist->attnum,
										  &state->flat_keys[nk - 1],
										  &state->flat_keynulls[nk - 1]);
				else
					dbe_vec_value(&dist->vec, row, dist->inputtypbyval,
								  dist->inputtyplen, &state->flat_keys[nk - 1],
								  &state->flat_keynulls[nk - 1]);
			}
			if (multi_text)
			{
				uint32		have = 0;

				for (i = 0; i < nk; i++)
				{
					uint32_t	code;
					int32_t		code_null;
					DbeStatus	st;

					if (th_size[i] == 0 || state->flat_keynulls[i])
						continue;
					st = dbe_segreader_code(kvec[i]->reader,
											(uint32_t) row, &code, &code_null);
					dbe_error_if_not_ok(&st, "leyendo el código del diccionario");
					if (!th_known[i][code])
					{
						th_hash[i][code] = dbe_flat_text_hash(state->flat_keys[i]);
						th_known[i][code] = true;
					}
					texthash[i] = th_hash[i][code];
					have |= (uint32) 1 << i;
				}
				h = dbe_flat_hash_keys(state->flatagg, state->flat_keys,
									   state->flat_keynulls, texthash, have);
				if (bucket >= 0 &&
					dbe_flat_bucket_of(dist == NULL ? h
									   : dbe_flat_hash_keys(state->flatdist,
															state->flat_keys,
															state->flat_keynulls,
															texthash,
															have & groupmask),
									   nbuckets) != (uint32) bucket)
					continue;
			}
			else if (bucket >= 0 && (cached == NULL || *cached == -2) &&
					 dbe_flat_bucket(dist != NULL ? state->flatdist : state->flatagg,
									 state->flat_keys, state->flat_keynulls,
									 nbuckets) != (uint32) bucket)
			{
				if (cached != NULL)
					*cached = -1;
				continue;
			}
			state->rows_seen++;

			for (i = 0; i < state->naggs; i++)
			{
				DbeAggState *agg = &state->aggs[i];

				if (agg->is_group || agg->is_distinct)
					continue;
				if (agg->is_count_star)
				{
					state->flat_vals[v] = (Datum) 0;
					state->flat_valnulls[v] = false;
				}
				else
					dbe_agg_value(state, agg, row, &state->flat_vals[v],
								  &state->flat_valnulls[v]);
				v++;
			}
			if (multi_text)
			{
				dbe_flat_add_hashed(state->flatagg, h, state->flat_keys,
									state->flat_keynulls, state->flat_vals,
									state->flat_valnulls);
				continue;
			}
			if (cached == NULL && use_runs)
			{
				dbe_agg_flat_run_add(state, &run, nk, v);
				continue;
			}
			if (cached == NULL)
			{
				dbe_flat_add(state->flatagg, state->flat_keys,
							 state->flat_keynulls, state->flat_vals,
							 state->flat_valnulls);
				continue;
			}
			if (*cached == -2)
			{
				int64		idx = dbe_flat_find(state->flatagg, state->flat_keys,
												state->flat_keynulls);

				if (idx >= 0)
					*cached = idx;
				else
				{
					uint32_t	k = nspill++;

					memset(&spill_states[(Size) k * nstates], 0,
						   sizeof(uint64) * nstates);
					spill_code[k] = (uint32_t) (cached - dict_cache);
					*cached = -3 - (int64) k;
				}
			}
			if (*cached >= 0)
				dbe_flat_add_to(state->flatagg, *cached, state->flat_vals,
								state->flat_valnulls);
			else
				dbe_flat_accum(state->flatagg,
							   &spill_states[(Size) (-3 - *cached) * nstates],
							   state->flat_vals, state->flat_valnulls);
		}

		/* Los grupos que no entraron, un registro cada uno. */
		for (uint32_t k = 0; k < nspill; k++)
		{
			Datum		key = (Datum) 0;
			bool		keynull = (spill_code[k] == dict_size);

			if (!keynull)
			{
				const void *data;
				uint32_t	len;
				DbeStatus	st = dbe_segreader_dict_value(state->groupvecs[0].reader,
														  spill_code[k],
														  &data, &len);

				dbe_error_if_not_ok(&st, "leyendo una entrada del diccionario");
				key = dbe_varlena_datum(data, state->groupvecs[0].attalign,
										state->groupvecs[0].cxt);
			}
			dbe_flat_add_states(state->flatagg, &key, &keynull,
								&spill_states[(Size) k * nstates]);
		}
		nspill = 0;
	}
	if (dict_cache != NULL)
	{
		pfree(dict_cache);
		pfree(spill_states);
		pfree(spill_code);
	}
	for (i = 0; i < nk; i++)
		if (th_hash[i] != NULL)
		{
			pfree(th_hash[i]);
			pfree(th_known[i]);
		}
	if (use_runs)
	{
		dbe_agg_flat_flush_run(state, &run);
		pfree(run.vals);
		pfree(run.valnulls);
		pfree(run.states);
	}
	if (dist != NULL)
	{
		dbe_agg_flat_distinct(state);
		dbe_flat_input_done(state->flatdist);
	}
	else
		dbe_flat_input_done(state->flatagg);
	state->flat_filled = true;
}

static TupleTableSlot *
dbe_agg_exec_flat(CustomScanState *node)
{
	DbeVectorAggState *state = (DbeVectorAggState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	int			i;
	int			v = 0;

	if (state->done)
		return NULL;

	/*
	 * En paralelo, cada balde es una vuelta entera: tomarlo, recorrer la
	 * tabla, emitir sus grupos, y tomar el siguiente.  Los baldes se toman
	 * del contador compartido, así que el resultado no depende de cuántos
	 * workers se hayan lanzado.
	 */
	for (;;)
	{
		if (!state->flat_filled)
		{
			if (state->shared != NULL)
			{
				uint32		b = pg_atomic_fetch_add_u32(&state->shared->next_bucket, 1);
				uint64		sp0 = dbe_cursor_stripes_pruned(state->cursor);
				uint64		gp0 = dbe_cursor_granules_pruned(state->cursor);

				if (b >= state->shared->nbuckets)
				{
					state->done = true;
					return NULL;
				}
				dbe_cursor_rescan(state->cursor);
				dbe_agg_flat_reset(state);
				dbe_agg_fill_flat(state, (int) b, state->shared->nbuckets);
				if (b == 0)
				{
					state->flat_stripes_pruned =
						dbe_cursor_stripes_pruned(state->cursor) - sp0;
					state->flat_granules_pruned =
						dbe_cursor_granules_pruned(state->cursor) - gp0;
				}
			}
			else
				dbe_agg_fill_flat(state, -1, 0);
		}

		if (state->topn_k > 0)
		{
			if (!state->topn_filled)
			{
				dbe_agg_topn_fill(state);
				state->topn_filled = true;
			}
			if (dbe_agg_topn_next(state))
				break;
		}
		else if (dbe_flat_next(state->flatdist != NULL ? state->flatdist
							   : state->flatagg, state->flat_keys,
							   state->flat_keynulls, state->flat_outs,
							   state->flat_outnulls))
			break;
		if (state->shared == NULL)
		{
			state->done = true;
			return NULL;
		}
		state->flat_filled = false;
		state->topn_filled = false;
	}

	ExecClearTuple(slot);
	for (i = 0; i < state->naggs; i++)
	{
		DbeAggState *agg = &state->aggs[i];

		if (agg->is_const)
		{
			slot->tts_values[i] = agg->constvalue;
			slot->tts_isnull[i] = agg->constisnull;
			continue;
		}
		if (agg->is_group)
		{
			slot->tts_values[i] = state->flat_keys[agg->group_idx];
			slot->tts_isnull[i] = state->flat_keynulls[agg->group_idx];
			continue;
		}
		slot->tts_values[i] = state->flat_outs[v];
		slot->tts_isnull[i] = state->flat_outnulls[v];
		v++;
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

			state->stripe_sel = dbe_agg_prefilter(state, state->stripe_nrows);
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

			if (!dbe_agg_row_selected(state, state->stripe_sel, row))
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

/* Prepara los atajos de count(DISTINCT) para el stripe cargado. */
static void
dbe_agg_distinct_stripe(DbeAggState *agg, uint64 nrows)
{
	int32_t		is_dict = 0;
	uint32_t	n = 0;

	agg->dist_has_prev = false;
	agg->dist_dict = false;
	if (!agg->has_vector || agg->vec.reader == NULL || agg->inputtyplen != -1)
		return;
	{
		DbeStatus	st = dbe_segreader_dict(agg->vec.reader, &is_dict, &n);

		dbe_error_if_not_ok(&st, "consultando el diccionario del segmento");
	}
	if (!is_dict || n == 0)
		return;

	if (agg->dist_cap < nrows)
	{
		if (agg->dist_codes != NULL)
			pfree(agg->dist_codes);
		agg->dist_codes = (uint32 *) MemoryContextAlloc(agg->dist_cxt->parent,
														sizeof(uint32) * nrows);
		agg->dist_cap = (uint32) nrows;
	}
	if (agg->dist_seen_cap < n)
	{
		if (agg->dist_seen != NULL)
			pfree(agg->dist_seen);
		agg->dist_seen = (uint8 *) MemoryContextAlloc(agg->dist_cxt->parent, n);
		agg->dist_seen_cap = n;
	}
	{
		DbeStatus	st = dbe_segreader_codes(agg->vec.reader, 0, (uint32) nrows,
											 agg->dist_codes);

		dbe_error_if_not_ok(&st, "leyendo los códigos de diccionario");
	}
	memset(agg->dist_seen, 0, n);
	agg->dist_dict = true;
}

/*
 * ¿Ya se buscó este valor en el stripe?  Si no, lo marca.  Con diccionario
 * no hace falta ni leer el valor para saberlo.
 */
static pg_attribute_always_inline bool
dbe_agg_distinct_seen(DbeAggState *agg, uint64 row, Datum value, bool isnull)
{
	if (agg->dist_dict)
	{
		uint32		c = agg->dist_codes[row];

		if (c == DBE_NULL_CODE || agg->dist_seen[c])
			return true;
		agg->dist_seen[c] = 1;
		return false;
	}
	if (isnull)
		return true;
	if (agg->dist_has_prev &&
		datumIsEqual(value, agg->dist_prev, agg->inputtypbyval, agg->inputtyplen))
		return true;
	agg->dist_prev = value;
	agg->dist_has_prev = true;
	return false;
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
	{
		if (state->flat)
			return dbe_agg_exec_flat(node);
		return state->hashed ? dbe_agg_exec_hashed(node)
			: dbe_agg_exec_grouped(node);
	}

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

		/*
		 * Sin WHERE, un stripe visible entero puede contestarse desde el
		 * catálogo: si todos los agregados salen de ahí, ni se lee.  Para
		 * eso hay que decidir ANTES de cargarlo, así que el índice del
		 * stripe se reserva acá y no adentro de dbe_agg_take_stripe().
		 */
		if (state->sma_any && state->qual == NULL)
		{
			int			idx = dbe_agg_claim_index(state);

			if (idx < 0)
				break;
			if (dbe_agg_stripe_sma(state, idx))
				continue;
			take = dbe_cursor_set_stripe(state->cursor, idx) &&
				dbe_cursor_stripe_has_pending(state->cursor)
				? DBE_TAKE_STRIPE : DBE_TAKE_SKIP;
		}
		else
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

			/* Los que ya avanzó el SMA no necesitan la columna. */
			agg->has_vector = !agg->is_count_star && !agg->sma_now &&
				dbe_cursor_column(state->cursor, agg->attnum, &agg->vec);
			if (agg->is_distinct)
				dbe_agg_distinct_stripe(agg, nrows);
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
		state->stripe_sel = dbe_agg_prefilter(state, nrows);
		for (row = dbe_cursor_next_live_row(state->cursor, 0); row < nrows;
			 row = dbe_cursor_next_live_row(state->cursor, row + 1))
		{
			if (!all_visible && !dbe_cursor_row_is_visible(state->cursor, row))
				continue;

			if (!dbe_agg_row_selected(state, state->stripe_sel, row))
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
				if (agg->sma_now)
					continue;
				if (agg->argexpr != NULL)
				{
					dbe_agg_value(state, agg, row, &value, &isnull);
					dbe_agg_advance(agg, value, isnull, aggcxt,
									state->agg_context);
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

				if (agg->is_distinct)
				{
					bool		isnew;

					/*
					 * count() no cuenta nulos, con DISTINCT tampoco.  Con
					 * diccionario el código dice si es nulo o si ya se vio
					 * sin leer el valor.
					 */
					if (agg->dist_dict)
					{
						if (dbe_agg_distinct_seen(agg, row, (Datum) 0, false))
							continue;
						dbe_vec_value(&agg->vec, row, agg->inputtypbyval,
									  agg->inputtyplen, &value, &isnull);
					}
					else
					{
						dbe_vec_value(&agg->vec, row, agg->inputtypbyval,
									  agg->inputtyplen, &value, &isnull);
						if (dbe_agg_distinct_seen(agg, row, value, isnull))
							continue;
					}

					/*
					 * LookupTupleHashEntry() arma el hash en el tempcxt y no
					 * lo resetea; es del que llama.  Cada 1024 búsquedas
					 * alcanza.
					 */
					if ((++agg->dist_since_reset & 1023) == 0)
						MemoryContextReset(agg->dist_tempcxt);

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

				dbe_vec_value(&agg->vec, row, agg->inputtypbyval,
							  agg->inputtyplen, &value, &isnull);
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

	/*
	 * Con un LIMIT el executor deja de pedir antes de que se vacíe la
	 * tabla, y quedan particiones abiertas.  Cerrarlas acá: si no, las
	 * cierra el resource owner al terminar la transacción, con un WARNING
	 * por archivo.
	 */
	if (state->flatagg != NULL)
		dbe_agg_flat_reset(state);

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

	if (state->flat && state->flatagg != NULL)
	{
		dbe_agg_flat_reset(state);
		state->flat_filled = false;
		state->topn_filled = false;
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
	state->stripes_sma = 0;
	state->next_index = 0;
	state->published = false;
	state->totals_ready = false;
	dbe_cursor_rescan(state->cursor);
	dbe_dictqual_reset(state->qual);
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
	pg_atomic_init_u64(&shared->stripes_sma, 0);

	/* Un balde por participante planeado: los workers y el líder. */
	pg_atomic_init_u32(&shared->next_bucket, 0);
	shared->nbuckets = (uint32) pcxt->nworkers + 1;
	pg_atomic_init_u64(&shared->flat_groups, 0);
	pg_atomic_init_u64(&shared->flat_spilled, 0);
	pg_atomic_init_u64(&shared->flat_parts, 0);
	pg_atomic_init_u64(&shared->flat_peak, 0);
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
	pg_atomic_fetch_add_u64(&state->shared->stripes_sma, state->stripes_sma);
	if (state->flat)
	{
		pg_atomic_fetch_add_u64(&state->shared->stripes_pruned,
								state->flat_stripes_pruned);
		pg_atomic_fetch_add_u64(&state->shared->granules_pruned,
								state->flat_granules_pruned);
		if (state->flatagg != NULL)
		{
			uint64		groups;
			int			parts;
			uint64		spilled;
			Size		peak;
			uint64		cur;

			dbe_agg_flat_stats(state, &groups, &parts, &spilled, &peak);
			pg_atomic_fetch_add_u64(&state->shared->flat_groups, groups);
			pg_atomic_fetch_add_u64(&state->shared->flat_spilled, spilled);
			pg_atomic_fetch_add_u64(&state->shared->flat_parts, (uint64) parts);
			cur = pg_atomic_read_u64(&state->shared->flat_peak);
			while (cur < (uint64) peak &&
				   !pg_atomic_compare_exchange_u64(&state->shared->flat_peak,
												   &cur, (uint64) peak))
				;
		}
	}
	else if (state->cursor != NULL)
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
		state->stripes_sma = pg_atomic_read_u64(&state->shared->stripes_sma);
		state->flat_groups_total = pg_atomic_read_u64(&state->shared->flat_groups);
		state->flat_spilled_total = pg_atomic_read_u64(&state->shared->flat_spilled);
		state->flat_parts_total = (int) pg_atomic_read_u64(&state->shared->flat_parts);
		state->flat_peak_total = (Size) pg_atomic_read_u64(&state->shared->flat_peak);
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
	pg_atomic_write_u64(&shared->stripes_sma, 0);
	pg_atomic_write_u32(&shared->next_bucket, 0);
	pg_atomic_write_u64(&shared->flat_groups, 0);
	pg_atomic_write_u64(&shared->flat_spilled, 0);
	pg_atomic_write_u64(&shared->flat_parts, 0);
	pg_atomic_write_u64(&shared->flat_peak, 0);
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
	if (state->flat)
	{
		List	   *cp = ((CustomScan *) node->ss.ps.plan)->custom_private;
		List	   *topn = list_length(cp) > 8 ? (List *) list_nth(cp, 8) : NIL;

		ExplainPropertyText("Agrupación", "tabla de hash propia", es);
		if (topn != NIL)
			ExplainPropertyInteger("Top-N", NULL, intVal(linitial(topn)), es);
		if (node->ss.ps.plan->parallel_aware)
			ExplainPropertyText("Reparto", "grupos por hash entre participantes",
								es);
		if (es->analyze && state->flatagg != NULL)
		{
			uint64		groups;
			int			parts;
			uint64		spilled;
			Size		peak;

			dbe_agg_flat_stats(state, &groups, &parts, &spilled, &peak);
			if (state->shared != NULL && state->totals_ready)
			{
				groups = state->flat_groups_total;
				parts = state->flat_parts_total;
				spilled = state->flat_spilled_total;
				peak = state->flat_peak_total;
			}
			ExplainPropertyInteger("Grupos", NULL, (int64) groups, es);
			ExplainPropertyInteger("Memoria de la tabla", "kB",
								   (int64) ((peak + 1023) / 1024), es);
			if (parts > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			{
				ExplainPropertyInteger("Particiones volcadas", NULL, parts, es);
				ExplainPropertyInteger("Filas volcadas", NULL,
									   (int64) spilled, es);
			}
		}
	}

	if (quals != NIL)
	{
		List	   *context;
		char	   *str;
		Index		relid = (Index) intVal(lthird(cscan->custom_private));

		/*
		 * setrefs.c le suma a scanrelid el offset del range table aplanado,
		 * pero no toca custom_private: dentro de una subconsulta los Var de
		 * los quals siguen apuntando al índice viejo, que ahora es otra
		 * relación ("invalid attnum 2 for relation v").  Para ejecutar da
		 * igual —se evalúan contra el slot—, pero para imprimirlos hay que
		 * llevarlos al índice nuevo.
		 */
		if (cscan->scan.scanrelid != relid)
		{
			quals = copyObject(quals);
			OffsetVarNodes((Node *) quals,
						   (int) cscan->scan.scanrelid - (int) relid, 0);
		}

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
		if (state->stripes_sma > 0 || (state->sma_any && es->format != EXPLAIN_FORMAT_TEXT))
			ExplainPropertyInteger("Stripes desde metadatos", NULL,
								   (int64) state->stripes_sma, es);
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
