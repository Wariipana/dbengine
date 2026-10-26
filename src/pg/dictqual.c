/*-------------------------------------------------------------------------
 *
 * dictqual.c
 *	  Predicados evaluados una vez por entrada del diccionario.  Ver
 *	  dictqual.h.
 *
 * Por cada cláusula que califica se guarda, para el stripe cargado, un byte
 * por entrada del diccionario: sin evaluar, pasa o no pasa.  La primera
 * fila con un código evalúa la cláusula de verdad, contra el valor de esa
 * entrada, y las siguientes leen el byte.  Al cambiar de stripe el
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

#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "access/nbtree.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "dbengine/dbengine.h"
#include "dbengine/dictqual.h"

#define DQ_UNKNOWN	0
#define DQ_PASS		1
#define DQ_FAIL		2

/*
 * `col LIKE 'patrón'` con un patrón hecho solo de literales y `%`: las
 * piezas literales en orden, y si el patrón está anclado al principio o al
 * final (no empieza o no termina con `%`).
 */
typedef struct DbeLikeMatch
{
	bool		negate;			/* NOT LIKE */
	bool		anchor_start;
	bool		anchor_end;
	int			npieces;
	const char **pieces;
	int		   *lens;
} DbeLikeMatch;

typedef struct DbeDictClause
{
	AttrNumber	attnum;
	ExprState  *qual;
	bool		lazy;			/* el slot no la trae: pedirla con fill */
	DbeLikeMatch *like;			/* se evalúa sobre los bytes, o NULL */

	uint64		generation;		/* de dbe_cursor_reader(); 0 = nunca */
	bool		is_dict;
	uint32		ndict;
	uint8	   *result;			/* ndict bytes: DQ_* por código */
	uint32		capacity;
	uint8		null_result;
} DbeDictClause;

/*
 * `col op const` sobre un entero o una fecha, con op del btree del tipo (o
 * su negador `<>`): el prefiltro la evalúa sobre el arreglo de la columna.
 * Sigue también en `rest`, que es lo que se evalúa fila por fila.
 */
typedef struct DbeVecClause
{
	AttrNumber	attnum;
	int			strategy;		/* BT*StrategyNumber, o 0 para <> */
	int64		value;
} DbeVecClause;

struct DbeDictQual
{
	int			nclauses;
	DbeDictClause *clauses;
	ExprState  *rest;			/* las que no califican; NULL si ninguna */
	Bitmapset  *rest_cols;		/* columnas que miran las de `rest` */
	int			nvec;
	DbeVecClause *vec;
	bool		rest_all_vec;	/* todas las de `rest` están en `vec` */
	MemoryContext cxt;			/* el del nodo: dura lo que dura el scan */
	uint32	   *codes;			/* los de un stripe, para el prefiltro */
	uint32		codes_cap;
};

/* ¿Entero (o fecha) que se lee como int64 de su ancho? */
static bool
dbe_vec_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID ||
		typid == DATEOID;
}

static bool
dbe_vec_const(Const *c, int64 *value)
{
	if (c->constisnull)
		return false;
	switch (c->consttype)
	{
		case INT2OID:
			*value = DatumGetInt16(c->constvalue);
			return true;
		case INT4OID:
			*value = DatumGetInt32(c->constvalue);
			return true;
		case INT8OID:
			*value = DatumGetInt64(c->constvalue);
			return true;
		case DATEOID:
			*value = DatumGetDateADT(c->constvalue);
			return true;
		default:
			return false;
	}
}

/*
 * ¿La cláusula es `col op const` (o `const op col`) que el prefiltro sabe
 * evaluar?  El operador tiene que estar en la familia btree por omisión del
 * tipo de la columna —integer_ops cruza int2, int4 e int8—, así que
 * compara los valores como enteros.
 */
static bool
dbe_vec_clause(Node *clause, Index varno, DbeVecClause *out)
{
	OpExpr	   *op = (OpExpr *) clause;
	Node	   *l;
	Node	   *r;
	Var		   *var;
	Const	   *c;
	bool		commuted = false;
	Oid			opfamily;
	Oid			opclass;
	int			strategy;

	if (!IsA(op, OpExpr) || list_length(op->args) != 2)
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	if (IsA(l, Var) && IsA(r, Const))
	{
		var = (Var *) l;
		c = (Const *) r;
	}
	else if (IsA(l, Const) && IsA(r, Var))
	{
		var = (Var *) r;
		c = (Const *) l;
		commuted = true;
	}
	else
		return false;
	if (var->varno != (int) varno || var->varattno <= 0 ||
		var->varlevelsup != 0 || !dbe_vec_type(var->vartype) ||
		!dbe_vec_const(c, &out->value))
		return false;

	opclass = GetDefaultOpClass(var->vartype, BTREE_AM_OID);
	if (!OidIsValid(opclass))
		return false;
	opfamily = get_opclass_family(opclass);
	strategy = get_op_opfamily_strategy(op->opno, opfamily);
	if (strategy == 0)
	{
		Oid			neg = get_negator(op->opno);

		if (!OidIsValid(neg) ||
			get_op_opfamily_strategy(neg, opfamily) != BTEqualStrategyNumber)
			return false;
	}
	else if (commuted)
		strategy = BTCommuteStrategyNumber(strategy);
	out->attnum = var->varattno;
	out->strategy = strategy;
	return true;
}

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

/*
 * ¿Es `col LIKE 'patrón'` o `col NOT LIKE 'patrón'` sobre la columna, con
 * un patrón que se puede comparar byte a byte?  Es la forma de q21 a q24
 * de ClickBench: `URL LIKE '%google%'`.
 *
 * textlike() hace lo mismo, pero por cada valor paga la llamada por fmgr,
 * la consulta de la collation y, en el Custom Scan, cargar el valor en el
 * slot.  Acá se busca cada pieza literal con memmem() sobre los bytes que
 * ya devolvió el lector del segmento.
 *
 * Coincide con textlike() solo bajo estas condiciones, y fuera de ellas se
 * evalúa la expresión:
 *
 *   - Sin `_` ni `\`: `_` es un carácter, no un byte, y el escape cambia
 *     qué es literal.  Un ESCAPE explícito llega ya plegado a un patrón con
 *     `\`, así que también queda afuera.
 *   - Collation determinista: con una no determinista LIKE compara según
 *     la collation (PG 18) y no por bytes.
 *   - UTF-8 o una codificación de un byte.  En UTF-8 una secuencia válida
 *     no puede aparecer empezando a mitad de otro carácter, así que buscar
 *     bytes es buscar caracteres; en SJIS, por ejemplo, no vale.
 */
static DbeLikeMatch *
dbe_like_compile(Node *clause, AttrNumber attnum)
{
	OpExpr	   *op = (OpExpr *) clause;
	Node	   *l;
	Const	   *k;
	text	   *pat;
	const char *p;
	int			plen;
	DbeLikeMatch *m;
	int			start;

	if (!IsA(clause, OpExpr) || list_length(op->args) != 2)
		return NULL;
	if (op->opfuncid != F_TEXTLIKE && op->opfuncid != F_TEXTNLIKE)
		return NULL;
	l = (Node *) linitial(op->args);
	while (IsA(l, RelabelType))
		l = (Node *) ((RelabelType *) l)->arg;
	if (!IsA(l, Var) || ((Var *) l)->varattno != attnum)
		return NULL;
	if (!IsA(lsecond(op->args), Const))
		return NULL;
	k = (Const *) lsecond(op->args);
	if (k->constisnull || k->consttype != TEXTOID)
		return NULL;
	if (!OidIsValid(op->inputcollid) ||
		!get_collation_isdeterministic(op->inputcollid))
		return NULL;
	if (GetDatabaseEncoding() != PG_UTF8 &&
		pg_database_encoding_max_length() != 1)
		return NULL;

	pat = DatumGetTextPP(k->constvalue);
	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);
	for (int i = 0; i < plen; i++)
		if (p[i] == '_' || p[i] == '\\')
			return NULL;

	m = (DbeLikeMatch *) palloc0(sizeof(DbeLikeMatch));
	m->negate = (op->opfuncid == F_TEXTNLIKE);
	m->anchor_start = (plen == 0 || p[0] != '%');
	m->anchor_end = (plen == 0 || p[plen - 1] != '%');
	m->pieces = (const char **) palloc(sizeof(char *) * (plen / 2 + 1));
	m->lens = (int *) palloc(sizeof(int) * (plen / 2 + 1));
	start = 0;
	for (int i = 0; i <= plen; i++)
	{
		if (i < plen && p[i] != '%')
			continue;
		if (i > start)
		{
			m->pieces[m->npieces] = pnstrdup(p + start, i - start);
			m->lens[m->npieces] = i - start;
			m->npieces++;
		}
		start = i + 1;
	}
	return m;
}

/* ¿Calza `s` con el patrón?  Sin contar el NOT. */
static bool
dbe_like_matches(const DbeLikeMatch *m, const char *s, Size len)
{
	Size		pos = 0;
	Size		end = len;
	int			first = 0;
	int			last = m->npieces;

	/* Sin `%`: es una igualdad. */
	if (m->anchor_start && m->anchor_end && m->npieces <= 1)
		return m->npieces == 0 ? len == 0
			: (len == (Size) m->lens[0] && memcmp(s, m->pieces[0], len) == 0);

	if (m->anchor_start && m->npieces > 0)
	{
		if (len < (Size) m->lens[0] || memcmp(s, m->pieces[0], m->lens[0]) != 0)
			return false;
		pos = m->lens[0];
		first = 1;
	}
	if (m->anchor_end && last > first)
	{
		Size		l = m->lens[last - 1];

		if (end - pos < l || memcmp(s + end - l, m->pieces[last - 1], l) != 0)
			return false;
		end -= l;
		last--;
	}

	/* Las del medio, la primera aparición de cada una: es la que deja más. */
	for (int i = first; i < last; i++)
	{
		const char *hit = memmem(s + pos, end - pos, m->pieces[i], m->lens[i]);

		if (hit == NULL)
			return false;
		pos = (hit - s) + m->lens[i];
	}
	return true;
}

/*
 * Para dbe_segreader_dict_contains(): cuántos bytes de cabecera tiene un
 * varlena guardado según su primer byte.  Se arma con las macros de
 * varlena y no a mano, así vale en cualquier endianness.  255 para las
 * formas que no se guardan (TOAST externo, comprimido inline): ahí el
 * motor contesta "no sé" y se lee el valor.
 */
static const uint8 *
dbe_varlena_hdr_table(void)
{
	static uint8 table[256];
	static bool done = false;

	if (!done)
	{
		for (int b = 0; b < 256; b++)
		{
			union
			{
				uint32		align;
				uint8		bytes[4];
			}			fake;

			memset(&fake, 0, sizeof(fake));
			fake.bytes[0] = (uint8) b;
			if (VARATT_IS_1B_E(&fake))
				table[b] = 255;
			else if (VARATT_IS_1B(&fake))
				table[b] = 1;
			else if (VARATT_IS_4B_U(&fake))
				table[b] = 4;
			else
				table[b] = 255;
		}
		done = true;
	}
	return table;
}

/*
 * El resultado de la cláusula para un valor guardado, o -1 si no se puede
 * decidir sobre los bytes y hay que evaluar la expresión.
 */
static int
dbe_like_eval_stored(const DbeLikeMatch *m, const void *data)
{
	const struct varlena *v = (const struct varlena *) data;

	/* Se guarda sin comprimir (columnar_write.c), pero por las dudas. */
	if (VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v))
		return -1;
	return dbe_like_matches(m, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v)) != m->negate;
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
			DbeVecClause vc;

			rest = lappend(rest, clause);
			pull_varattnos(clause, varno, &dq->rest_cols);
			if (dbe_enable_dict_quals && dbe_vec_clause(clause, varno, &vc))
			{
				if (dq->vec == NULL)
					dq->vec = (DbeVecClause *)
						palloc(sizeof(DbeVecClause) * list_length(quals));
				dq->vec[dq->nvec++] = vc;
			}
			continue;
		}

		dq->clauses[dq->nclauses].attnum = attnum;
		dq->clauses[dq->nclauses].like = dbe_like_compile(clause, attnum);
		dq->clauses[dq->nclauses].qual =
			dbe_dictqual_compile(list_make1(clause), parent, estate);
		dq->nclauses++;
	}

	if (rest != NIL)
		dq->rest = dbe_dictqual_compile(rest, parent, estate);
	dq->rest_all_vec = (dq->nvec == list_length(rest));
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

/*
 * El resultado de la cláusula para un código (o para NULL), calculado y
 * guardado.  El valor sale del diccionario y se pone en el slot solo
 * mientras se evalúa: no hace falta saber de qué fila vino, y el slot
 * queda como estaba.
 */
static bool
dbe_dictqual_resolve(DbeDictClause *c, DbeSegReader *reader, uint32 code,
					 bool isnull, ExprContext *econtext)
{
	uint8	   *slot_result = isnull ? &c->null_result : &c->result[code];
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	int			a = c->attnum - 1;
	Datum		saved_value;
	bool		saved_null;
	bool		pass;

	if (*slot_result != DQ_UNKNOWN)
		return *slot_result == DQ_PASS;

	if (c->like != NULL)
	{
		int			r = 0;

		if (isnull)
			r = 0;				/* LIKE es estricto: NULL no pasa */
		else
		{
			const DbeLikeMatch *m = c->like;
			int32		found = -1;

			/*
			 * `%aguja%`: sobre FSST se busca en los códigos comprimidos, sin
			 * descomprimir la entrada (q21 a q24).
			 */
			if (m->npieces == 1 && !m->anchor_start && !m->anchor_end)
			{
				DbeStatus	st = dbe_segreader_dict_contains(reader, code,
															 m->pieces[0],
															 m->lens[0],
															 dbe_varlena_hdr_table(),
															 &found);

				dbe_error_if_not_ok(&st, "buscando en el diccionario");
			}
			if (found >= 0)
				r = (found != 0) != m->negate;
			else
			{
				const void *data;
				uint32		len;
				DbeStatus	st = dbe_segreader_dict_value(reader, code, &data, &len);

				dbe_error_if_not_ok(&st, "leyendo una entrada del diccionario");
				r = dbe_like_eval_stored(c->like, data);
			}
		}
		if (r >= 0)
		{
			*slot_result = r ? DQ_PASS : DQ_FAIL;
			return r;
		}
	}

	saved_value = slot->tts_values[a];
	saved_null = slot->tts_isnull[a];
	if (isnull)
	{
		slot->tts_values[a] = (Datum) 0;
		slot->tts_isnull[a] = true;
	}
	else
	{
		const void *data;
		uint32		len;
		DbeStatus	st = dbe_segreader_dict_value(reader, code, &data, &len);

		dbe_error_if_not_ok(&st, "leyendo una entrada del diccionario");
		slot->tts_values[a] =
			dbe_varlena_datum(data,
							  TupleDescAttr(slot->tts_tupleDescriptor, a)->attalign,
							  econtext->ecxt_per_tuple_memory);
		slot->tts_isnull[a] = false;
	}
	pass = ExecQual(c->qual, econtext);
	ResetExprContext(econtext);
	slot->tts_values[a] = saved_value;
	slot->tts_isnull[a] = saved_null;
	*slot_result = pass ? DQ_PASS : DQ_FAIL;
	return pass;
}

static bool
dbe_dictqual_eval_clause(DbeDictQual *dq, DbeDictClause *c, DbeRowCursor *cur,
						 uint64 row, ExprContext *econtext,
						 DbeDictFillFn fill, void *fill_arg)
{
	uint64		generation;
	DbeSegReader *reader = dbe_cursor_reader(cur, c->attnum, &generation);
	uint32		code = 0;
	int32		isnull = 0;
	bool		pass;

	if (generation != c->generation)
		dbe_dictqual_sync(dq, c, reader, generation);

	if (!c->is_dict)
	{
		if (c->like != NULL && reader != NULL)
		{
			const void *data;
			uint32		len;
			int32		vnull;
			int			r;
			DbeStatus	st = dbe_segreader_get(reader, (uint32) row, &data,
											   &len, &vnull);

			dbe_error_if_not_ok(&st, "leyendo un valor de la columna");
			if (vnull)
				return false;	/* LIKE es estricto: NULL no pasa */
			r = dbe_like_eval_stored(c->like, data);
			if (r >= 0)
				return r;
		}
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
	return dbe_dictqual_resolve(c, reader, code, isnull != 0, econtext);
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

/*
 * Una cláusula de arreglo sobre todas las filas del stripe: las que no la
 * pasan, o tienen la columna nula, quedan en DBE_SEL_FAIL.  false si la
 * columna no tiene arreglo de ancho fijo en este stripe.
 */
#define DBE_VEC_LOOP(T, CMP) \
	do { \
		const T    *d = (const T *) vec.data; \
		for (uint32 r = 0; r < nrows; r++) \
		{ \
			int64		x = d[r]; \
			if (!(CMP)) \
				sel[r] = DBE_SEL_FAIL; \
		} \
	} while (0)

#define DBE_VEC_BY_WIDTH(CMP) \
	do { \
		switch (vec.width) \
		{ \
			case 2: DBE_VEC_LOOP(int16, CMP); break; \
			case 4: DBE_VEC_LOOP(int32, CMP); break; \
			default: DBE_VEC_LOOP(int64, CMP); break; \
		} \
	} while (0)

static bool
dbe_vec_prefilter(const DbeVecClause *vc, DbeRowCursor *cur, uint32 nrows,
				  uint8 *sel)
{
	DbeColumnVector vec;
	int64		k = vc->value;

	if (!dbe_cursor_column(cur, vc->attnum, &vec) || !vec.contiguous ||
		(vec.width != 2 && vec.width != 4 && vec.width != 8))
		return false;

	switch (vc->strategy)
	{
		case BTLessStrategyNumber:
			DBE_VEC_BY_WIDTH(x < k);
			break;
		case BTLessEqualStrategyNumber:
			DBE_VEC_BY_WIDTH(x <= k);
			break;
		case BTEqualStrategyNumber:
			DBE_VEC_BY_WIDTH(x == k);
			break;
		case BTGreaterEqualStrategyNumber:
			DBE_VEC_BY_WIDTH(x >= k);
			break;
		case BTGreaterStrategyNumber:
			DBE_VEC_BY_WIDTH(x > k);
			break;
		default:
			DBE_VEC_BY_WIDTH(x != k);
			break;
	}

	/* Una comparación con NULL no es verdadera: no pasa. */
	if (vec.nulls != NULL)
		for (uint32 r = 0; r < nrows; r++)
			if ((vec.nulls[r / 8] & (1 << (r % 8))) == 0)
				sel[r] = DBE_SEL_FAIL;
	return true;
}

bool
dbe_dictqual_prefilter(DbeDictQual *dq, DbeRowCursor *cur, uint32 nrows,
					   uint8 *sel, ExprContext *econtext)
{
	bool		any = false;

	/* Sin cláusulas de diccionario ni de arreglo no hay nada que decidir. */
	if (dq->nclauses == 0 && dq->nvec == 0)
		return false;
	memset(sel, dq->rest != NULL && !dq->rest_all_vec ? DBE_SEL_CHECK
		   : DBE_SEL_PASS, nrows);
	for (int i = 0; i < dq->nvec; i++)
	{
		if (!dbe_vec_prefilter(&dq->vec[i], cur, nrows, sel))
		{
			/* Esa columna no tiene arreglo en este stripe: fila por fila. */
			for (uint32 r = 0; r < nrows; r++)
				if (sel[r] == DBE_SEL_PASS)
					sel[r] = DBE_SEL_CHECK;
			continue;
		}
		any = true;
	}
	for (int i = 0; i < dq->nclauses; i++)
	{
		DbeDictClause *c = &dq->clauses[i];
		uint64		generation;
		DbeSegReader *reader = dbe_cursor_reader(cur, c->attnum, &generation);

		if (generation != c->generation)
			dbe_dictqual_sync(dq, c, reader, generation);
		if (!c->is_dict)
		{
			/* Sin diccionario en este stripe: fila por fila. */
			for (uint32 r = 0; r < nrows; r++)
				if (sel[r] == DBE_SEL_PASS)
					sel[r] = DBE_SEL_CHECK;
			continue;
		}

		if (dq->codes_cap < nrows)
		{
			if (dq->codes != NULL)
				pfree(dq->codes);
			dq->codes = (uint32 *) MemoryContextAlloc(dq->cxt,
													  sizeof(uint32) * nrows);
			dq->codes_cap = nrows;
		}
		{
			DbeStatus	st = dbe_segreader_codes(reader, 0, nrows, dq->codes);

			dbe_error_if_not_ok(&st, "leyendo los códigos de diccionario");
		}
		any = true;

		/*
		 * Solo las filas que el cursor va a visitar: las de un gránulo que
		 * el zone map descarta no se piden, y resolver sus códigos podía
		 * costar un LIKE por entrada que nadie iba a mirar.
		 */
		for (uint64 r = dbe_cursor_next_live_row(cur, 0); r < nrows;
			 r = dbe_cursor_next_live_row(cur, r + 1))
		{
			uint32		code = dq->codes[r];
			bool		isnull = (code == DBE_NULL_CODE);
			uint8		res;

			if (sel[r] == DBE_SEL_FAIL)
				continue;
			res = isnull ? c->null_result : c->result[code];
			if (res == DQ_UNKNOWN)
				res = dbe_dictqual_resolve(c, reader, code, isnull, econtext)
					? DQ_PASS : DQ_FAIL;
			if (res == DQ_FAIL)
				sel[r] = DBE_SEL_FAIL;
		}
	}
	return any;
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

/* ------------------------------------------------------------------------
 * Expresiones de la salida
 *
 * Igual que las cláusulas, pero lo que se guarda por código es el valor.
 * Los valores de varlena se copian a un contexto propio que se vacía al
 * cambiar de stripe: un valor devuelto vale lo que dura el stripe, que es
 * más de lo que el slot de salida necesita (hasta la próxima fila).
 * ------------------------------------------------------------------------ */

#define DE_UNKNOWN	0
#define DE_VALUE	1
#define DE_NULL		2

struct DbeDictExpr
{
	AttrNumber	attnum;
	ExprState  *expr;
	int16		typlen;
	bool		typbyval;

	uint64		generation;
	bool		is_dict;
	uint32		capacity;
	uint8	   *state;			/* DE_* por código */
	Datum	   *values;
	uint8		null_state;		/* lo mismo para la fila con la columna en NULL */
	Datum		null_value;
	MemoryContext values_cxt;	/* copias de los valores del stripe */
};

DbeDictExpr *
dbe_dictexpr_build(Expr *expr, Index varno, TupleDesc desc, PlanState *parent)
{
	DbeDictExpr *de;
	Node	   *bare = (Node *) expr;
	AttrNumber	attnum;

	if (!dbe_enable_dict_quals)
		return NULL;

	/* La columna sola no tiene nada que calcular. */
	while (IsA(bare, RelabelType))
		bare = (Node *) ((RelabelType *) bare)->arg;
	if (IsA(bare, Var) || expression_returns_set((Node *) expr))
		return NULL;

	attnum = dbe_dictqual_column((Node *) expr, varno, desc);
	if (attnum == InvalidAttrNumber)
		return NULL;

	de = (DbeDictExpr *) palloc0(sizeof(DbeDictExpr));
	de->attnum = attnum;
	de->expr = ExecInitExpr(expr, parent);
	get_typlenbyval(exprType((Node *) expr), &de->typlen, &de->typbyval);
	de->values_cxt = AllocSetContextCreate(CurrentMemoryContext,
										   "dbengine valores por diccionario",
										   ALLOCSET_DEFAULT_SIZES);
	return de;
}

static void
dbe_dictexpr_sync(DbeDictExpr *de, DbeSegReader *reader, uint64 generation)
{
	int32		is_dict = 0;
	uint32		ndict = 0;

	de->generation = generation;
	de->null_state = DE_UNKNOWN;
	de->is_dict = false;
	MemoryContextReset(de->values_cxt);

	if (reader == NULL)
		return;

	{
		DbeStatus	st = dbe_segreader_dict(reader, &is_dict, &ndict);

		dbe_error_if_not_ok(&st, "consultando el diccionario de una columna");
	}
	if (!is_dict || ndict == 0)
		return;

	if (de->capacity < ndict)
	{
		MemoryContext cxt = GetMemoryChunkContext(de);

		if (de->state != NULL)
		{
			pfree(de->state);
			pfree(de->values);
		}
		de->state = (uint8 *) MemoryContextAlloc(cxt, ndict);
		de->values = (Datum *) MemoryContextAlloc(cxt, sizeof(Datum) * ndict);
		de->capacity = ndict;
	}
	memset(de->state, DE_UNKNOWN, ndict);
	de->is_dict = true;
}

Datum
dbe_dictexpr_eval(DbeDictExpr *de, DbeRowCursor *cur, uint64 row,
				  ExprContext *econtext, bool *isnull)
{
	uint64		generation;
	DbeSegReader *reader = dbe_cursor_reader(cur, de->attnum, &generation);
	uint32		code = 0;
	int32		colnull = 0;
	uint8	   *st;
	Datum	   *val;
	Datum		v;
	MemoryContext old;

	if (generation != de->generation)
		dbe_dictexpr_sync(de, reader, generation);

	if (!de->is_dict)
		return ExecEvalExprSwitchContext(de->expr, econtext, isnull);

	{
		DbeStatus	s = dbe_segreader_code(reader, (uint32) row, &code, &colnull);

		dbe_error_if_not_ok(&s, "leyendo un código de diccionario");
	}

	st = colnull ? &de->null_state : &de->state[code];
	val = colnull ? &de->null_value : &de->values[code];
	if (*st != DE_UNKNOWN)
	{
		*isnull = (*st == DE_NULL);
		return *val;
	}

	v = ExecEvalExprSwitchContext(de->expr, econtext, isnull);
	if (*isnull)
	{
		*st = DE_NULL;
		*val = (Datum) 0;
		return (Datum) 0;
	}

	old = MemoryContextSwitchTo(de->values_cxt);
	*val = datumCopy(v, de->typbyval, de->typlen);
	MemoryContextSwitchTo(old);
	*st = DE_VALUE;
	return *val;
}

void
dbe_dictexpr_reset(DbeDictExpr *de)
{
	if (de != NULL)
		de->generation = 0;
}
