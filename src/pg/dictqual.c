/*-------------------------------------------------------------------------
 *
 * dictqual.c
 *	  Predicados evaluados una vez por entrada del diccionario.  Ver
 *	  dictqual.h.
 *
 * Por cada cláusula que califica se guarda, para el stripe cargado, un byte
 * por entrada del diccionario: sin evaluar, pasa o no pasa.  La primera
 * fila con un código evalúa la cláusula de verdad, contra el valor que ya
 * está en el slot, y las siguientes leen el byte.  Al cambiar de stripe el
 * diccionario es otro y la tabla se vacía.
 *
 * Es correcto porque una cláusula sin funciones volátiles que mira una sola
 * columna es una función del valor de esa columna: mismo valor, mismo
 * resultado.  Las funciones STABLE (now(), un LIKE con collation) no
 * cambian dentro de una ejecución, y los Params solo cambian entre rescans,
 * que vacían la tabla.  Un NULL se recuerda aparte: no tiene código.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"

#include "dbengine/dbengine.h"
#include "dbengine/dictqual.h"

#define DQ_UNKNOWN	0
#define DQ_PASS		1
#define DQ_FAIL		2

typedef struct DbeDictClause
{
	AttrNumber	attnum;
	ExprState  *qual;
	bool		lazy;			/* el slot no la trae: pedirla con fill */

	uint64		generation;		/* de dbe_cursor_reader(); 0 = nunca */
	bool		is_dict;
	uint32		ndict;
	uint8	   *result;			/* ndict bytes: DQ_* por código */
	uint32		capacity;
	uint8		null_result;
} DbeDictClause;

struct DbeDictQual
{
	int			nclauses;
	DbeDictClause *clauses;
	ExprState  *rest;			/* las que no califican; NULL si ninguna */
	Bitmapset  *rest_cols;		/* columnas que miran las de `rest` */
	MemoryContext cxt;			/* el del nodo: dura lo que dura el scan */
};

/*
 * ¿La cláusula depende solo de una columna de largo variable de esta
 * relación?  Devuelve el attnum, o InvalidAttrNumber.
 *
 * Solo largo variable porque es lo único que puede tener diccionario.
 */
static AttrNumber
dbe_dictqual_column(Node *clause, Index varno, TupleDesc desc)
{
	List	   *vars;
	ListCell   *lc;
	AttrNumber	attnum = InvalidAttrNumber;

	if (contain_volatile_functions(clause) || contain_subplans(clause))
		return InvalidAttrNumber;

	vars = pull_var_clause(clause, PVC_RECURSE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varno != (int) varno ||
			var->varlevelsup != 0 || var->varattno < 1 ||
			(attnum != InvalidAttrNumber && var->varattno != attnum))
		{
			attnum = InvalidAttrNumber;
			break;
		}
		attnum = var->varattno;
	}
	list_free(vars);

	if (attnum == InvalidAttrNumber || attnum > desc->natts ||
		TupleDescAttr(desc, attnum - 1)->attlen != -1)
		return InvalidAttrNumber;
	return attnum;
}

static ExprState *
dbe_dictqual_compile(List *quals, PlanState *parent, EState *estate)
{
	if (parent == NULL)
		return ExecPrepareQual(quals, estate);
	return ExecInitQual(quals, parent);
}

DbeDictQual *
dbe_dictqual_build(List *quals, Index varno, TupleDesc desc,
				   PlanState *parent, EState *estate)
{
	DbeDictQual *dq;
	List	   *rest = NIL;
	ListCell   *lc;

	if (quals == NIL)
		return NULL;

	dq = (DbeDictQual *) palloc0(sizeof(DbeDictQual));
	dq->cxt = CurrentMemoryContext;
	dq->clauses = (DbeDictClause *)
		palloc0(sizeof(DbeDictClause) * list_length(quals));

	foreach(lc, quals)
	{
		Node	   *clause = (Node *) lfirst(lc);
		AttrNumber	attnum = dbe_enable_dict_quals
			? dbe_dictqual_column(clause, varno, desc)
			: InvalidAttrNumber;

		if (attnum == InvalidAttrNumber)
		{
			rest = lappend(rest, clause);
			pull_varattnos(clause, varno, &dq->rest_cols);
			continue;
		}

		dq->clauses[dq->nclauses].attnum = attnum;
		dq->clauses[dq->nclauses].qual =
			dbe_dictqual_compile(list_make1(clause), parent, estate);
		dq->nclauses++;
	}

	if (rest != NIL)
		dq->rest = dbe_dictqual_compile(rest, parent, estate);
	for (int i = 0; i < dq->nclauses; i++)
		dq->clauses[i].lazy =
			!bms_is_member(dq->clauses[i].attnum - FirstLowInvalidHeapAttributeNumber,
						   dq->rest_cols);
	return dq;
}

bool
dbe_dictqual_lazy_column(DbeDictQual *dq, AttrNumber attnum)
{
	int			i;

	if (dq == NULL)
		return false;
	for (i = 0; i < dq->nclauses; i++)
		if (dq->clauses[i].attnum == attnum)
			return dq->clauses[i].lazy;
	return false;
}

/* Prepara la tabla de la cláusula para el stripe cargado. */
static void
dbe_dictqual_sync(DbeDictQual *dq, DbeDictClause *c, DbeSegReader *reader,
				  uint64 generation)
{
	int32		is_dict = 0;
	uint32		ndict = 0;

	c->generation = generation;
	c->null_result = DQ_UNKNOWN;
	c->is_dict = false;

	if (reader == NULL)
		return;

	{
		DbeStatus	st = dbe_segreader_dict(reader, &is_dict, &ndict);

		dbe_error_if_not_ok(&st, "consultando el diccionario de una columna");
	}
	if (!is_dict || ndict == 0)
		return;

	if (c->capacity < ndict)
	{
		if (c->result != NULL)
			pfree(c->result);
		c->result = (uint8 *) MemoryContextAlloc(dq->cxt, ndict);
		c->capacity = ndict;
	}
	memset(c->result, DQ_UNKNOWN, ndict);
	c->ndict = ndict;
	c->is_dict = true;
}

static bool
dbe_dictqual_eval_clause(DbeDictQual *dq, DbeDictClause *c, DbeRowCursor *cur,
						 uint64 row, ExprContext *econtext,
						 DbeDictFillFn fill, void *fill_arg)
{
	uint64		generation;
	DbeSegReader *reader = dbe_cursor_reader(cur, c->attnum, &generation);
	uint8	   *slot_result;
	uint32		code = 0;
	int32		isnull = 0;
	bool		pass;

	if (generation != c->generation)
		dbe_dictqual_sync(dq, c, reader, generation);

	if (!c->is_dict)
	{
		if (c->lazy && fill != NULL)
			fill(fill_arg, c->attnum);
		pass = ExecQual(c->qual, econtext);
		ResetExprContext(econtext);
		return pass;
	}

	{
		DbeStatus	st = dbe_segreader_code(reader, (uint32) row, &code, &isnull);

		dbe_error_if_not_ok(&st, "leyendo un código de diccionario");
	}

	slot_result = isnull ? &c->null_result : &c->result[code];
	if (*slot_result != DQ_UNKNOWN)
		return *slot_result == DQ_PASS;

	if (c->lazy && fill != NULL)
		fill(fill_arg, c->attnum);
	pass = ExecQual(c->qual, econtext);
	ResetExprContext(econtext);
	*slot_result = pass ? DQ_PASS : DQ_FAIL;
	return pass;
}

bool
dbe_dictqual_eval(DbeDictQual *dq, DbeRowCursor *cur, uint64 row,
				  ExprContext *econtext, DbeDictFillFn fill, void *fill_arg)
{
	int			i;
	bool		pass;

	/* Primero las del diccionario: después de la primera fila son un byte. */
	for (i = 0; i < dq->nclauses; i++)
		if (!dbe_dictqual_eval_clause(dq, &dq->clauses[i], cur, row, econtext,
									  fill, fill_arg))
			return false;

	if (dq->rest == NULL)
		return true;

	pass = ExecQual(dq->rest, econtext);
	ResetExprContext(econtext);
	return pass;
}

void
dbe_dictqual_reset(DbeDictQual *dq)
{
	int			i;

	if (dq == NULL)
		return;
	for (i = 0; i < dq->nclauses; i++)
		dq->clauses[i].generation = 0;
}
