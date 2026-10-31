/*-------------------------------------------------------------------------
 *
 * flathash.h
 *	  GROUP BY por hash con tabla propia y volcado a disco.
 *
 * El HashAggregate de Postgres guarda cada grupo como una MinimalTuple con
 * los estados de transición colgando, y avanza cada agregado con una
 * llamada por fmgr.  Con muchos grupos eso es lo que pesa, no el disco: en
 * ClickBench `GROUP BY WatchID, ClientIP` —10M grupos en 10M filas— tarda
 * lo mismo con work_mem de 4 MB que con 2 GB, unos 175 bytes y más de un
 * microsegundo por grupo.
 *
 * Esta tabla sirve para claves cuya igualdad es la binaria —enteros,
 * fechas, timestamps, y texto con collation determinista— y para los
 * agregados cuyo estado es un par de enteros: count, sum y avg de
 * smallint/integer/bigint, min y max de enteros.  Una entrada es un arreglo de
 * uint64 —hash, máscara de nulos, claves, estados— en un arreglo denso, con
 * un directorio de direccionamiento abierto encima; un texto es un puntero a
 * una copia en un arena de la tabla.
 *
 * La memoria la acota hash_mem.  Cuando la tabla se llena, los grupos que
 * ya están se siguen agregando en memoria y las filas de grupos nuevos se
 * vuelcan a particiones en archivos temporales, elegidas por el hash.  Al
 * terminar la entrada se emite la tabla, y cada partición se procesa
 * después igual, con otros bits del hash y volcando de nuevo si hace falta.
 * Es el hash híbrido de nodeAgg desde PG 13.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_FLATHASH_H
#define DBENGINE_FLATHASH_H

#include "postgres.h"

#define DBE_FLAT_MAX_KEYS	8

typedef enum DbeFlatKind
{
	DBE_FLAT_COUNT_STAR,		/* count(*) */
	DBE_FLAT_COUNT,				/* count(col): los no nulos */
	DBE_FLAT_SUM,				/* sum(int2/int4) → int8 */
	DBE_FLAT_AVG,				/* avg(int2/int4) → numeric, con int8_avg() */
	DBE_FLAT_MIN,				/* min de un entero, mismo tipo */
	DBE_FLAT_MAX,
	DBE_FLAT_SUM8,				/* sum(int8) → numeric, suma en int128 */
	DBE_FLAT_AVG8				/* avg(int8) → numeric */
} DbeFlatKind;

typedef struct DbeFlatAggDef
{
	DbeFlatKind kind;
	int16		vallen;			/* ancho del argumento (por valor); 0 sin argumento */
} DbeFlatAggDef;

typedef struct DbeFlatAgg DbeFlatAgg;

/*
 * `keylen`: ancho de cada clave (por valor: 1, 2, 4 u 8), o -1 si es texto
 * (varlena comparado por bytes).  `budget`: bytes
 * de memoria para la tabla y los buffers de las particiones.
 * `est_groups`: la estimación del planner, solo para el tamaño inicial de la
 * tabla; si es mala, la tabla crece hasta el presupuesto.
 */
extern DbeFlatAgg *dbe_flat_create(int nkeys, const int16 *keylen,
								   int naggs, const DbeFlatAggDef *defs,
								   Size budget, double est_groups);

/* Una fila.  `vals` y `valnulls` van en el orden de `defs`. */
extern void dbe_flat_add(DbeFlatAgg *fa, const Datum *keys,
						 const bool *keynulls, const Datum *vals,
						 const bool *valnulls);

/*
 * Para quien agrupa por un código de diccionario y busca el grupo una vez
 * por código:
 *
 * dbe_flat_find() devuelve el índice del grupo, creándolo si no está, o -1
 * si no está y no hay lugar.  No agrega nada.  El índice vale mientras dure
 * la entrada (hasta dbe_flat_input_done()), y las filas del grupo se suman
 * con dbe_flat_add_to().
 *
 * Con -1, en vez de volcar fila por fila, quien llama puede llevar un
 * estado parcial propio de dbe_flat_nstates() palabras, en cero, sumarle
 * las filas con dbe_flat_accum() y entregarlo una vez con
 * dbe_flat_add_states(): un registro por código y no uno por fila.
 *
 * dbe_flat_add_states() combina estados con los del grupo, como un
 * Finalize, o los vuelca si el grupo no entra.  Los estados de cada
 * agregado van seguidos, dbe_flat_state_words() palabras cada uno.
 */
extern int64 dbe_flat_find(DbeFlatAgg *fa, const Datum *keys,
						   const bool *keynulls);
extern void dbe_flat_add_to(DbeFlatAgg *fa, int64 idx, const Datum *vals,
							const bool *valnulls);
extern int	dbe_flat_nstates(DbeFlatAgg *fa);
extern void dbe_flat_accum(DbeFlatAgg *fa, uint64 *states, const Datum *vals,
						   const bool *valnulls);
extern void dbe_flat_add_states(DbeFlatAgg *fa, const Datum *keys,
								const bool *keynulls, const uint64 *states);
extern int	dbe_flat_state_words(DbeFlatKind kind);

/*
 * En paralelo los participantes no se reparten stripes sino grupos: cada
 * uno toma un balde y agrega solo las filas cuya clave cae en él, así que
 * emite grupos completos.  El balde de una clave es el mismo en todos.
 */
extern uint32 dbe_flat_bucket(DbeFlatAgg *fa, const Datum *keys,
							  const bool *keynulls, uint32 nbuckets);

/*
 * Para quien agrupa por varias claves con alguna de texto: el hash de un
 * texto se puede calcular una vez por código de diccionario con
 * dbe_flat_text_hash() y pasarlo en `texthash[i]`, con el bit i de `have`
 * prendido; las claves sin el bit se hashean ahí.  El hash que devuelve
 * dbe_flat_hash_keys() sirve para dbe_flat_bucket_of() y para
 * dbe_flat_add_hashed(), que es dbe_flat_add() sin volver a calcularlo.
 */
extern uint64 dbe_flat_text_hash(Datum d);
extern uint64 dbe_flat_hash_keys(DbeFlatAgg *fa, const Datum *keys,
								 const bool *keynulls, const uint64 *texthash,
								 uint32 have);
extern uint32 dbe_flat_bucket_of(uint64 hash, uint32 nbuckets);
extern void dbe_flat_add_hashed(DbeFlatAgg *fa, uint64 hash,
								const Datum *keys, const bool *keynulls,
								const Datum *vals, const bool *valnulls);

/* No hay más filas de entrada: lo que sigue es emitir. */
extern void dbe_flat_input_done(DbeFlatAgg *fa);

/*
 * El próximo grupo.  Los valores por referencia (el numeric de avg) valen
 * hasta la llamada siguiente.  false cuando no quedan.
 */
extern bool dbe_flat_next(DbeFlatAgg *fa, Datum *keys, bool *keynulls,
						  Datum *outs, bool *outnulls);

/*
 * Como dbe_flat_next(), pero con los estados parciales que espera el
 * Finalize de Postgres sobre un Partial Agg (AGGSPLIT_INITIAL_SERIAL).
 */
extern bool dbe_flat_next_partial(DbeFlatAgg *fa, Datum *keys, bool *keynulls,
								  Datum *outs, bool *outnulls);

/*
 * Como dbe_flat_next(), pero sin terminar los agregados: `*states` apunta a
 * los estados del grupo, que valen hasta la llamada siguiente.  Sirve para
 * pasárselos a otra tabla con dbe_flat_add_states().
 */
extern bool dbe_flat_next_states(DbeFlatAgg *fa, Datum *keys, bool *keynulls,
								 const uint64 **states);

/*
 * El resultado del agregado `i` a partir de los estados de un grupo (los de
 * dbe_flat_next_states()).  Un valor por referencia (el numeric de avg) se
 * arma en el contexto de quien llama.
 */
extern Datum dbe_flat_final(DbeFlatAgg *fa, int i, const uint64 *states,
							bool *isnull);

/* ¿La entrada ya empezó a volcarse a particiones? */
extern bool dbe_flat_spilling(DbeFlatAgg *fa);

/* Vacía todo para volver a empezar: rescan. */
extern void dbe_flat_reset(DbeFlatAgg *fa);

/* Para el EXPLAIN. */
extern void dbe_flat_stats(DbeFlatAgg *fa, uint64 *groups, int *partitions,
						   uint64 *spilled_rows, Size *peak_bytes);

#endif							/* DBENGINE_FLATHASH_H */
