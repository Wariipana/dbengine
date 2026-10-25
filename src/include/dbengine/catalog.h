/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  Acceso a los catálogos internos dbengine.stripe y
 *	  dbengine.stripe_column.
 *
 * Son tablas heap normales: MVCC, WAL y limpieza vienen de Postgres.  Pero
 * la VISIBILIDAD de un stripe no se delega en el MVCC de su fila: se
 * decide con insert_xid/insert_cid.  Ver doc/adr-002-visibilidad-de-stripes.md.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_CATALOG_H
#define DBENGINE_CATALOG_H

#include "postgres.h"

#include "access/htup.h"
#include "access/transam.h"
#include "common/relpath.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "datatype/timestamp.h"

typedef struct DbeStripeDesc
{
	uint64		stripe_id;
	uint64		first_row_number;
	uint64		row_count;
	uint64		data_offset;
	uint64		data_length;
	FullTransactionId insert_xid;
	CommandId	insert_cid;		/* mínimo */
	CommandId	insert_cid_max;

	/*
	 * Momento del commit, para el time travel (F7).  Solo se guarda cuando
	 * el stripe es producto de una fusión: ahí el xid original se pierde y
	 * con él la posibilidad de preguntárselo a pg_commit_ts.
	 */
	bool		has_commit_ts;
	TimestampTz commit_ts;
} DbeStripeDesc;

/*
 * attnum del segmento de command ids.  Los attnum reales arrancan en 1, así
 * que el 0 queda libre para metadatos por fila.
 */
#define DBE_ATTNUM_CID	0

/*
 * Un stripe junta filas de una sola transacción pero de varios comandos, así
 * que la visibilidad puede ser parcial: hay que mirar el cid de cada fila.
 */
typedef enum DbeStripeVisibility
{
	DBE_STRIPE_NONE = 0,		/* ninguna fila visible */
	DBE_STRIPE_ALL,				/* todas visibles */
	DBE_STRIPE_PARTIAL			/* hay que filtrar por command id */
} DbeStripeVisibility;

typedef struct DbeSegDesc
{
	AttrNumber	attnum;
	uint64		seg_offset;		/* relativo a DbeStripeDesc.data_offset */
	uint64		seg_length;
	uint64		null_count;
	int16		encoding;

	/*
	 * Zone map (R7): mínimo y máximo del segmento en la representación
	 * binaria del tipo.  has_minmax en false si el tipo no tiene orden
	 * btree, si todo el segmento es nulo, o si los valores no entran en el
	 * presupuesto de DBE_ZONEMAP_MAX_BYTES.
	 */
	bool		has_minmax;
	const char *min_value;
	uint32		min_length;
	const char *max_value;
	uint32		max_length;

	/*
	 * Filtro de Bloom del segmento (F5).  Solo se carga si quien pide los
	 * segmentos lo pide explícitamente: es un bytea grande y detoastearlo
	 * en cada scan costaría en todas las consultas para servirle a unas
	 * pocas.
	 */
	const char *bloom;
	uint32		bloom_length;

	/*
	 * Zone map por gránulo: min/max de cada tramo de DBE_GRANULE_ROWS filas
	 * dentro del segmento.  Sirve para saltear filas en un stripe que el
	 * zone map de stripe ya dejó pasar.  NULL si no se construyó.  Se carga
	 * con las mismas reglas que el Bloom, o sea solo si lo piden.
	 */
	const char *granules;
	uint32		granules_length;

	/*
	 * Agregado precalculado (SMA): la suma exacta de los no nulos, como
	 * numeric.  Solo para smallint, integer y bigint; con eso y null_count
	 * salen sum, avg y count(col) del stripe sin leer datos.
	 */
	bool		has_sum;
	Datum		sum_value;
} DbeSegDesc;

/*
 * Un zone map sirve para descartar bloques, no para guardar datos: si el
 * valor no entra acá, guardarlo cuesta más de lo que puede ahorrar.
 */
#define DBE_ZONEMAP_MAX_BYTES	128

/*
 * Granularidad de descarte anidada.
 *
 * El stripe es la unidad de ESCRITURA: 65.536 filas, elegido para que el
 * segmento comprima bien y el I/O sea secuencial.  Achicarlo para descartar
 * más fino sale caro — medido, con stripes de 8.192 el scan completo cae a
 * 0,41x — porque paga más catálogo, peor compresión y más seeks.
 *
 * Así que la unidad de DESCARTE se separa de la de escritura, que es lo que
 * hacen ORC (row groups / row index), Parquet (row group / page) y
 * ClickHouse (part / granule).  Adentro de cada stripe se guarda el min/max
 * de cada tramo de DBE_GRANULE_ROWS filas, y un stripe que pasó el filtro
 * puede igual saltearse la mayoría de sus filas.
 *
 * Medido sobre ClickBench, dentro de los stripes que el zone map deja
 * pasar: `UserID = k` descarta el 70,4% de los gránulos, `EventDate` en dos
 * días el 24,1%, `CounterID = 62` el 13,6%.  Sobre columnas de hash
 * (RefererHash, URLHash) no descarta nada, que es lo esperable.
 *
 * No ahorra I/O: el segmento se lee entero igual.  Ahorra decodificar y
 * evaluar el predicado.  Por eso el número es casi el mismo en frío que en
 * caliente: 1,43x y 1,37x en un lookup por UserID, 1,39x y 1,41x en un
 * rango de UserID, 1,25x y 1,22x en un rango de una hora de EventTime.
 * Donde no puede descartar no cuesta nada — 0,99x a 1,01x —, y el bloque
 * ocupa 776 kB de catálogo para una tabla de 1,8 GB.
 */
#define DBE_GRANULE_ROWS		8192

/*
 * Layout de DbeSegDesc.granules, en bytes:
 *
 *   uint8   version (1)
 *   uint8   width            ancho del tipo: 1, 2, 4 u 8
 *   uint16  granule_rows
 *   uint16  count            cantidad de gránulos
 *   uint16  reserved
 *   uint8   flags[count]     rellenado a múltiplo de 8
 *   valor   min[count]
 *   valor   max[count]
 *
 * Las flags van agrupadas y no intercaladas con los valores a propósito:
 * así cada min/max queda alineado a su propio ancho y se puede leer con
 * fetch_att() sin copiar.
 *
 * flags bit 0 = el gránulo tiene al menos un valor no nulo.  Sin eso el
 * min/max no significa nada y el gránulo no se puede descartar.
 *
 * Solo para tipos por valor de 1, 2, 4 u 8 bytes: guardar min/max de largo
 * variable por gránulo multiplicaría el catálogo por la cantidad de
 * gránulos, y son justamente los tipos donde el descarte por rango sirve
 * menos.
 */
#define DBE_GRANULE_VERSION		1
#define DBE_GRANULE_HDR_BYTES	8

/*
 * Offsets dentro del bloque.  Las flags van todas juntas y rellenadas a
 * múltiplo de 8 para que los valores queden alineados a su propio ancho:
 * los tipos por valor miden 1, 2, 4 u 8 bytes, y fetch_att() los lee con
 * un acceso del tamaño del tipo.
 */
#define DBE_GRANULE_FLAGS_OFF		DBE_GRANULE_HDR_BYTES
#define DBE_GRANULE_FLAGS_BYTES(n)	(((n) + 7) & ~7)
#define DBE_GRANULE_MIN_OFF(n)		\
	(DBE_GRANULE_FLAGS_OFF + DBE_GRANULE_FLAGS_BYTES(n))
#define DBE_GRANULE_MAX_OFF(n, w)	\
	(DBE_GRANULE_MIN_OFF(n) + (Size) (n) * (w))
#define DBE_GRANULE_BYTES(n, w)		\
	(DBE_GRANULE_MAX_OFF(n, w) + (Size) (n) * (w))
#define DBE_GRANULE_FLAG_SEEN		0x01

/* OIDs de los catálogos internos. */
extern Oid	dbe_stripe_relid(void);
extern Oid	dbe_stripe_pkey_relid(void);
extern Oid	dbe_stripe_column_relid(void);
extern Oid	dbe_stripe_column_pkey_relid(void);

/* Escritura de metadatos. */
extern void dbe_catalog_insert_stripe(RelFileNumber relnumber, const DbeStripeDesc *stripe);
extern void dbe_catalog_insert_segment(RelFileNumber relnumber, uint64 stripe_id,
									   const DbeSegDesc *seg);

/*
 * Stripes de `relid` visibles bajo `snapshot`, ordenados por stripe_id
 * (que crece igual que first_row_number).  Devuelve una List de
 * DbeStripeDesc * palloc'ados en el contexto actual.
 */
extern List *dbe_catalog_list_stripes(RelFileNumber relnumber, Snapshot snapshot);

/*
 * Segmentos de un stripe.  `wanted` es un bitmap de attnums pedidos (o
 * NULL para todos): esto es el column pruning.  Llena `out`, indexado por
 * attnum-1, y marca en `found` cuáles se encontraron.
 */
extern void dbe_catalog_load_segments(RelFileNumber relnumber, uint64 stripe_id,
									  int natts, const bool *wanted,
									  DbeSegDesc *out, bool *found);

/*
 * Igual, pero además carga el filtro de Bloom de las columnas marcadas en
 * `want_zone` (NULL = ninguna).  Se separa del anterior porque el filtro
 * es grande: solo lo pide quien tiene un predicado de igualdad que podría
 * usarlo.
 *
 * `fallback` (puede ser NULL) es un snapshot MVCC tomado antes de listar
 * los stripes.  Se usa si los metadatos del stripe ya no están para
 * SnapshotSelf porque una fusión concurrente los borró; ver catalog.c.
 */
extern void dbe_catalog_load_segments_ext(RelFileNumber relnumber,
										  uint64 stripe_id, int natts,
										  const bool *wanted,
										  const bool *want_zone,
										  Snapshot fallback,
										  DbeSegDesc *out, bool *found);

/* Copia los metadatos a otro relfilenode (ALTER TABLE ... SET TABLESPACE). */
extern void dbe_catalog_copy_relation(RelFileNumber from, RelFileNumber to);

/*
 * Borra los metadatos de UN stripe: su fila y las de sus segmentos.  Lo usa
 * la fusión de F6, que reemplaza varios stripes por uno.
 */
extern void dbe_catalog_delete_stripe(RelFileNumber relnumber, uint64 stripe_id);

/* Borra todos los metadatos de un relfilenode (rewrite / TRUNCATE). */
extern void dbe_catalog_drop_relation(RelFileNumber relnumber);

/*
 * Reservas de números de fila de un buffer de escritura todavía no
 * flusheado.  Hacen que el chequeo de unicidad de otra sesión espere a la
 * transacción dueña en vez de no ver la fila.  Ver catalog.c.
 */
extern void dbe_catalog_insert_reservation(RelFileNumber relnumber,
										   uint64 first_row, uint64 row_count);
extern void dbe_catalog_delete_reservation(RelFileNumber relnumber,
										   uint64 first_row);
extern TransactionId dbe_catalog_reservation_in_progress(RelFileNumber relnumber,
														 uint64 rownum);
extern void dbe_catalog_drop_reservations(RelFileNumber relnumber);

/*
 * DROP TABLE no pasa por el Table AM, así que la limpieza del catálogo se
 * cuelga de object_access_hook.  Se instala desde _PG_init.
 */
extern void dbe_catalog_drop_hook_init(void);

/*
 * Orden físico.  dbe_catalog_set_order() la escribe CLUSTER cuando terminó
 * de volcar los stripes ya ordenados; dbe_catalog_clear_order() la borra
 * cualquier escritura posterior.  dbe_catalog_get_order() devuelve
 * InvalidOid si las filas no están ordenadas o si no se sabe.
 */
extern void dbe_catalog_set_order(RelFileNumber relnumber, Oid indexrelid);
extern void dbe_catalog_clear_order(RelFileNumber relnumber);
extern Oid	dbe_catalog_get_order(RelFileNumber relnumber);

/*
 * Bytes de los segmentos de las columnas pedidas, y filas visibles.  Es lo
 * que hace que el costo del Custom Scan sea el I/O real y no una fracción
 * inventada del tamaño de la tabla.
 */
extern void dbe_catalog_column_bytes(RelFileNumber relnumber, Snapshot snapshot,
									 const bool *wanted, int natts,
									 uint64 *nbytes, uint64 *nrows);

/* Cuenta filas y bytes visibles; para relation_estimate_size y ANALYZE. */
extern void dbe_catalog_summarize(RelFileNumber relnumber, Snapshot snapshot,
								  uint64 *nrows, uint64 *nbytes,
								  uint64 *nstripes);

/*
 * Cuándo commiteó el stripe.  Sale de commit_ts si está guardado, y si no
 * de pg_commit_ts.  Devuelve false si no se puede saber — porque
 * track_commit_timestamp está apagado, o porque el registro ya se truncó.
 */
extern bool dbe_stripe_commit_ts(const DbeStripeDesc *stripe, TimestampTz *out);

/* Regla de visibilidad de stripe (ADR-002). */
extern DbeStripeVisibility dbe_stripe_visible(const DbeStripeDesc *stripe,
											  Snapshot snapshot);

/* Ubicación del segmento de command ids; false si el stripe no tiene. */
extern bool dbe_catalog_load_cid_segment(RelFileNumber relnumber,
										 uint64 stripe_id, DbeSegDesc *out);

#endif							/* DBENGINE_CATALOG_H */
