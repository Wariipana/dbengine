/*-------------------------------------------------------------------------
 *
 * zonemap.c
 *	  Descarte de stripes por zone map.  Ver zonemap.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "access/tupmacs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "dbengine/bridge.h"
#include "dbengine/dbengine.h"
#include "dbengine/zonemap.h"

static bool dbe_extract_filter(Expr *clause, Index relid, AttrNumber *attnum,
							   StrategyNumber *strategy, Const **konst,
							   Oid *opfamily, Oid *opno_out);
static bool dbe_lookup_strategy_op(Oid opfamily, Oid coltype, Oid consttype,
								   StrategyNumber strategy, FmgrInfo *out);

/*
 * ¿Es `columna OP constante` con OP en la familia btree del tipo?
 *
 * Solo se aceptan comparaciones del mismo tipo de los dos lados: con tipos
 * cruzados la función de comparación del tipo no aplica, y forzarla daría
 * un orden equivocado — o sea, descartaría stripes que sí tenían filas.
 */
static bool
dbe_extract_filter(Expr *clause, Index relid, AttrNumber *attnum,
				   StrategyNumber *strategy, Const **konst, Oid *opfamily,
				   Oid *opno_out)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Var		   *var;
	Oid			opno;
	TypeCacheEntry *tc;
	Oid			lefttype;
	Oid			righttype;
	int			strat;

	if (!IsA(clause, OpExpr))
		return false;

	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);
	opno = op->opno;

	if (IsA(left, Var) && IsA(right, Const))
		 /* como está */ ;
	else if (IsA(right, Var) && IsA(left, Const))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
		opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return false;
	}
	else
		return false;

	var = (Var *) left;
	if (var->varno != relid || var->varattno <= 0 || var->varlevelsup != 0)
		return false;
	if (((Const *) right)->constisnull)
		return false;

	tc = lookup_type_cache(var->vartype,
						   TYPECACHE_BTREE_OPFAMILY | TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(tc->btree_opf) || tc->cmp_proc_finfo.fn_oid == InvalidOid)
		return false;
	if (!op_in_opfamily(opno, tc->btree_opf))
		return false;

	/*
	 * strategy es un uint16 y get_op_opfamily_properties() escribe un int:
	 * hay que pasarle una variable del tamaño correcto, no un cast.
	 */
	get_op_opfamily_properties(opno, tc->btree_opf, false,
							   &strat, &lefttype, &righttype);
	*strategy = (StrategyNumber) strat;

	/*
	 * El lado izquierdo tiene que ser la columna; el derecho puede ser de
	 * otro tipo, que es lo habitual con literales enteros.
	 */
	if (lefttype != var->vartype)
		return false;
	(void) righttype;

	*attnum = var->varattno;
	*konst = (Const *) right;
	*opfamily = tc->btree_opf;
	if (opno_out != NULL)
		*opno_out = opno;
	return true;
}

/*
 * Busca en la familia btree el operador (coltype, consttype) de la
 * estrategia pedida y lo deja listo para llamar.  Si no existe, el
 * predicado simplemente no se usa para podar.
 */
static bool
dbe_lookup_strategy_op(Oid opfamily, Oid coltype, Oid consttype,
					   StrategyNumber strategy, FmgrInfo *out)
{
	Oid			opno = get_opfamily_member(opfamily, coltype, consttype,
										   strategy);

	if (!OidIsValid(opno))
		return false;

	fmgr_info(get_opcode(opno), out);
	return true;
}

/* Reconstruye un Datum a partir de los bytes guardados en el zone map. */
static Datum
dbe_zonemap_datum(const DbeStripeFilter *f, const char *bytes)
{
	if (f->typlen > 0)
		return fetch_att(bytes, f->typbyval, f->typlen);
	return PointerGetDatum(bytes);
}

/* Evalúa `bytes OP valor` con el operador de la familia btree. */
static bool
dbe_zonemap_test(FmgrInfo *op, const DbeStripeFilter *f, const char *bytes)
{
	return DatumGetBool(FunctionCall2Coll(op, f->collation,
										  dbe_zonemap_datum(f, bytes),
										  f->value));
}

/*
 * ¿Puede este stripe tener alguna fila que cumpla los predicados?
 *
 * Conservador por construcción: solo devuelve false cuando el rango
 * [min, max] del segmento excluye con certeza al valor buscado.  Sin zone
 * map, o con un tipo que no se puede comparar, se lee el stripe.
 *
 * Los NULL no complican nada: ninguno de estos operadores es verdadero para
 * NULL, así que acotar con el rango de los valores NO nulos alcanza.
 */
bool
dbe_filterset_passes(void *arg, const DbeSegDesc *segs,
						  const bool *found, int natts)
{
	DbeFilterSet *fs = (DbeFilterSet *) arg;
	int			i;

	for (i = 0; i < fs->nfilters; i++)
	{
		DbeStripeFilter *f = &fs->filters[i];
		const DbeSegDesc *seg;

		if (f->attnum > natts || !found[f->attnum - 1])
			continue;

		seg = &segs[f->attnum - 1];
		if (!seg->has_minmax && seg->bloom == NULL)
			continue;

		/*
		 * El Bloom primero: descarta más y no necesita comparar valores.
		 * Su error es de un solo lado, así que un false acá es definitivo.
		 */
		if (f->test_bloom && seg->bloom != NULL)
		{
			int32		maybe = 1;
			DbeStatus	st;

			st = dbe_bloom_contains(seg->bloom, seg->bloom_length, f->hash,
									&maybe);
			dbe_error_if_not_ok(&st, "consultando el filtro de Bloom");
			if (!maybe)
				return false;
		}

		if (!seg->has_minmax)
			continue;			/* solo había filtro de Bloom */

		if (f->test_min && !dbe_zonemap_test(&f->min_op, f, seg->min_value))
			return false;
		if (f->test_max && !dbe_zonemap_test(&f->max_op, f, seg->max_value))
			return false;
	}

	return true;
}

/*
 * Qué gránulos de este stripe pueden tener filas que pasen el filtro.
 *
 * El stripe ya pasó el zone map de stripe; esto mira más fino, tramo por
 * tramo de DBE_GRANULE_ROWS filas.  Es la misma comparación, con los mismos
 * operadores btree — lo único que cambia es de dónde salen los bytes.
 *
 * Conservador igual que el nivel de arriba: un gránulo sin ningún valor no
 * nulo no se descarta, aunque ningún operador de comparación sea verdadero
 * para NULL.  Descartar de más devolvería filas de menos.
 */
bool
dbe_filterset_granules(void *arg, const DbeSegDesc *segs, const bool *found,
					   int natts, uint64 nrows, uint8 *mask)
{
	DbeFilterSet *fs = (DbeFilterSet *) arg;
	uint32		ngran = (uint32) ((nrows + DBE_GRANULE_ROWS - 1) /
								  DBE_GRANULE_ROWS);
	bool		pruned = false;
	uint32		g;
	int			i;

	for (g = 0; g < ngran; g++)
		mask[g >> 3] |= (uint8) (1 << (g & 7));

	for (i = 0; i < fs->nfilters; i++)
	{
		DbeStripeFilter *f = &fs->filters[i];
		const DbeSegDesc *seg;
		const char *blk;
		uint16		count;
		uint8		w;

		if (f->attnum > natts || !found[f->attnum - 1])
			continue;

		seg = &segs[f->attnum - 1];
		if (seg->granules == NULL ||
			seg->granules_length < DBE_GRANULE_HDR_BYTES)
			continue;

		blk = seg->granules;
		if ((uint8) blk[0] != DBE_GRANULE_VERSION)
			continue;			/* bloque de otra versión: se ignora */

		w = (uint8) blk[1];
		memcpy(&count, blk + 4, sizeof(count));

		/*
		 * El bloque guarda los valores en la representación del tipo con el
		 * que se escribió.  Si no coincide con el del filtro, compararlos
		 * sería leer bytes ajenos.
		 */
		if (!f->typbyval || f->typlen != (int16) w)
			continue;
		if (count > ngran ||
			seg->granules_length < DBE_GRANULE_BYTES(count, w))
			continue;

		for (g = 0; g < count; g++)
		{
			const char *mn;
			const char *mx;

			if (!(mask[g >> 3] & (1 << (g & 7))))
				continue;		/* ya lo descartó otro filtro */
			if (!(blk[DBE_GRANULE_FLAGS_OFF + g] & DBE_GRANULE_FLAG_SEEN))
				continue;		/* todo nulo: no se puede afirmar nada */

			mn = blk + DBE_GRANULE_MIN_OFF(count) + (Size) g * w;
			mx = blk + DBE_GRANULE_MAX_OFF(count, w) + (Size) g * w;

			if ((f->test_min && !dbe_zonemap_test(&f->min_op, f, mn)) ||
				(f->test_max && !dbe_zonemap_test(&f->max_op, f, mx)))
			{
				mask[g >> 3] &= (uint8) ~(1 << (g & 7));
				pruned = true;
			}
		}
	}

	return pruned;
}

bool
dbe_filter_column(Expr *clause, Index relid, AttrNumber *attnum,
				  StrategyNumber *strategy)
{
	Const	   *konst;
	Oid			opfamily;

	return dbe_extract_filter(clause, relid, attnum, strategy, &konst,
							  &opfamily, NULL);
}

bool
dbe_filter_usable(Expr *clause, Index relid)
{
	AttrNumber	attnum;
	StrategyNumber strategy;

	return dbe_filter_column(clause, relid, &attnum, &strategy);
}

/*
 * Traduce cada cláusula a una prueba contra el zone map.
 *
 * Una cláusula puede necesitar dos pruebas: `col = v` acota por los dos
 * lados.  Las que no se pueden traducir simplemente no aportan descarte.
 */
DbeFilterSet *
dbe_filterset_build(List *clauses, Index relid, TupleDesc desc)
{
	DbeFilterSet *fs = (DbeFilterSet *) palloc0(sizeof(DbeFilterSet));
	ListCell   *lc;

	fs->filters = (DbeStripeFilter *)
		palloc0(sizeof(DbeStripeFilter) * Max(list_length(clauses), 1));

	/*
	 * Único punto donde se decide si hay descarte, para los dos nodos.  Con
	 * el conjunto vacío nadie instala el filtro en el cursor y se leen
	 * todos los stripes — que es exactamente lo que la GUC promete.
	 */
	if (!dbe_enable_zonemap_pruning)
		return fs;

	foreach(lc, clauses)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		DbeStripeFilter *f = &fs->filters[fs->nfilters];
		Const	   *konst;
		Form_pg_attribute att;
		StrategyNumber strategy;
		Oid			opfamily;
		Oid			opno = InvalidOid;
		Oid			coltype;
		bool		ok = false;

		memset(f, 0, sizeof(*f));

		if (!dbe_extract_filter(clause, relid, &f->attnum, &strategy, &konst,
								&opfamily, &opno))
			continue;
		if (f->attnum > desc->natts)
			continue;

		att = TupleDescAttr(desc, f->attnum - 1);
		coltype = att->atttypid;

		switch (strategy)
		{
			case BTLessStrategyNumber:
			case BTLessEqualStrategyNumber:
				f->test_min = true;
				ok = dbe_lookup_strategy_op(opfamily, coltype,
											konst->consttype, strategy,
											&f->min_op);
				break;
			case BTGreaterStrategyNumber:
			case BTGreaterEqualStrategyNumber:
				f->test_max = true;
				ok = dbe_lookup_strategy_op(opfamily, coltype,
											konst->consttype, strategy,
											&f->max_op);
				break;
			case BTEqualStrategyNumber:
				f->test_min = true;
				f->test_max = true;
				ok = dbe_lookup_strategy_op(opfamily, coltype,
											konst->consttype,
											BTLessEqualStrategyNumber,
											&f->min_op) &&
					dbe_lookup_strategy_op(opfamily, coltype,
										   konst->consttype,
										   BTGreaterEqualStrategyNumber,
										   &f->max_op);
				break;
			default:
				break;
		}

		if (!ok)
			continue;

		f->value = konst->constvalue;
		f->collation = att->attcollation;
		f->typlen = att->attlen;
		f->typbyval = att->attbyval;

		/*
		 * Filtro de Bloom, solo para igualdad.
		 *
		 * El hash del valor buscado se saca con la función que el propio
		 * operador de igualdad declara para el lado derecho.  Es lo que
		 * hace que ande con tipos cruzados: las familias hash de Postgres
		 * exigen que dos valores iguales de tipos distintos hasheen igual,
		 * así que el integer de `id = 5000` da el mismo hash que el bigint
		 * que se guardó al escribir.  Hashear con el tipo de la columna
		 * daría otro número y el filtro descartaría stripes que sí tenían
		 * la fila — un resultado incorrecto, no una lentitud.
		 */
		if (strategy == BTEqualStrategyNumber && OidIsValid(opno))
		{
			Oid			lhs_hash;
			Oid			rhs_hash;

			if (get_op_hash_functions(opno, &lhs_hash, &rhs_hash) &&
				OidIsValid(rhs_hash))
			{
				FmgrInfo	hf;
				uint32		h;

				fmgr_info(rhs_hash, &hf);
				h = DatumGetUInt32(FunctionCall1Coll(&hf, konst->constcollid,
													 konst->constvalue));
				f->hash = ((uint64) h) |
					(((uint64) (h * 0x9e3779b9u)) << 32);
				f->test_bloom = true;
			}
		}

		fs->nfilters++;
	}

	return fs;
}

bool *
dbe_filterset_zone_columns(DbeFilterSet *fs, int natts)
{
	bool	   *cols = NULL;
	int			i;

	for (i = 0; i < fs->nfilters; i++)
	{
		DbeStripeFilter *f = &fs->filters[i];

		if (f->attnum < 1 || f->attnum > natts)
			continue;
		if (cols == NULL)
			cols = (bool *) palloc0(sizeof(bool) * natts);
		cols[f->attnum - 1] = true;
	}
	return cols;
}
