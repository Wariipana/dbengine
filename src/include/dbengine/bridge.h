/*-------------------------------------------------------------------------
 *
 * bridge.h
 *	  Airlock C -> C++.  Única superficie por la que la capa de integración
 *	  con Postgres llama al motor.
 *
 * Todas las funciones declaradas acá:
 *   - están definidas en src/pg/bridge.cpp (compilado como C++),
 *   - son extern "C" y nunca dejan escapar una excepción,
 *   - devuelven DbeStatus por valor; el caller (C) decide si eso se
 *     convierte en ereport(ERROR) o se maneja de otra forma.
 *
 * Prohibido: llamar a ereport/elog dentro de bridge.cpp o de src/engine/.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_BRIDGE_H
#define DBENGINE_BRIDGE_H

#include "dbengine/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ciclo de vida del motor (llamado desde _PG_init y desde el fin del backend). */
DbeStatus	dbe_engine_startup(void);
DbeStatus	dbe_engine_shutdown(void);

/* Identificación: version_out debe apuntar a un buffer de al menos len bytes. */
DbeStatus	dbe_engine_version(char *version_out, size_t len);

/*
 * --- Superficie de autotest de la frontera (F0) -------------------------
 *
 * Estas funciones existen para demostrar, de forma ejecutable, que se
 * cumplen las dos invariantes de R2.  No son código de producción pero se
 * quedan en el árbol: son el test de regresión de la frontera.
 */

/* Lanza una excepción de C++ del tipo pedido dentro del motor.  El airlock
 * la tiene que atrapar y devolverla como DbeStatus.  Nunca debe propagarse. */
typedef enum DbeThrowKind
{
	DBE_THROW_NONE = 0,
	DBE_THROW_RUNTIME_ERROR,	/* std::runtime_error */
	DBE_THROW_BAD_ALLOC,		/* std::bad_alloc */
	DBE_THROW_INT,				/* un int pelado -> catch(...) */
	DBE_THROW_NESTED			/* excepción a través de varios frames con destructores */
} DbeThrowKind;

DbeStatus	dbe_selftest_throw(int32_t kind, int32_t *dtors_run_out);

/*
 * Llama, desde C++, a un shim de C que hace ereport(ERROR).  Verifica el
 * sentido inverso: el longjmp se detiene en el shim y el frame de C++
 * ejecuta sus destructores normalmente.
 *
 * dtors_run_out recibe cuántos destructores corrieron durante el unwind.
 * Si un longjmp hubiera cruzado el frame de C++, ese contador quedaría en 0.
 */
DbeStatus	dbe_selftest_pg_error(int32_t *dtors_run_out);

/* Ida y vuelta sin error: C -> C++ -> shim C -> C++ -> C. */
DbeStatus	dbe_selftest_roundtrip(int32_t *dtors_run_out);

/*
 * --- Segmentos de columna (F1) ------------------------------------------
 *
 * Un segmento son los datos de UNA columna para UN stripe.  El motor no
 * conoce los tipos de Postgres: solo distingue "ancho fijo de W bytes" de
 * "largo variable".  La conversión Datum <-> bytes la hace la capa C.
 *
 * Los booleanos cruzan como int32_t y no como bool: el tamaño de bool no
 * está fijado por el estándar en ninguno de los dos lenguajes, y esta
 * frontera no se apoya en suposiciones de ABI.
 */

typedef struct DbeSegBuilder DbeSegBuilder;
typedef struct DbeSegReader DbeSegReader;

/*
 * fixed_width == 0 -> largo variable.  allow_compression en 0 fuerza la
 * codificación cruda; es lo que permite medir la ganancia de F2 sin cambiar
 * ninguna otra cosa.
 */
DbeStatus	dbe_segbuilder_create(uint32_t fixed_width, int32_t allow_compression,
								  DbeSegBuilder **out);

/* Codificación que eligió finish(); ver SegmentEncoding. */
DbeStatus	dbe_segbuilder_encoding(DbeSegBuilder *b, int32_t *encoding_out,
									uint32_t *raw_bytes_out,
									uint32_t *encoded_bytes_out);
DbeStatus	dbe_segbuilder_append(DbeSegBuilder *b, const void *data,
								  uint32_t len, int32_t is_null);
/*
 * Serializa.  El buffer devuelto es propiedad del builder y vive hasta
 * dbe_segbuilder_destroy(); el caller lo copia o lo escribe al storage
 * antes de destruirlo.
 */
DbeStatus	dbe_segbuilder_finish(DbeSegBuilder *b, const void **bytes_out,
								  uint32_t *len_out);
DbeStatus	dbe_segbuilder_rows(DbeSegBuilder *b, uint32_t *out);
void		dbe_segbuilder_destroy(DbeSegBuilder *b);

/*
 * El reader no copia: apunta adentro de `bytes`, que tiene que seguir vivo
 * y alineado a MAXALIGN mientras el reader exista.
 */
DbeStatus	dbe_segreader_open(const void *bytes, uint32_t len,
							   DbeSegReader **out);
DbeStatus	dbe_segreader_rows(DbeSegReader *r, uint32_t *out);
DbeStatus	dbe_segreader_get(DbeSegReader *r, uint32_t row,
							  const void **data_out, uint32_t *len_out,
							  int32_t *is_null_out);
/*
 * Vista del segmento como arreglo contiguo de ancho fijo.  is_vector_out
 * queda en 0 si no la hay (largo variable o diccionario) y entonces el
 * caller tiene que usar dbe_segreader_get() fila por fila.
 *
 * Este es el "back channel" de R4: leer una columna sin pagar una llamada
 * por valor ni construir tuplas.
 */
DbeStatus	dbe_segreader_vector(DbeSegReader *r, int32_t *is_vector_out,
								 const void **data_out, uint32_t *width_out,
								 const void **nulls_out);

/*
 * Acceso al diccionario de un segmento.
 *
 * Sirve para agrupar por el código en vez de por el valor: el string se
 * resuelve una vez por entrada del diccionario, no una por fila.  Con las
 * columnas de texto de un log web eso son entre 5 y 26 veces menos
 * búsquedas de hash.
 */
DbeStatus	dbe_segreader_dict(DbeSegReader *r, int32_t *is_dict_out,
							   uint32_t *size_out);
DbeStatus	dbe_segreader_code(DbeSegReader *r, uint32_t row,
							   uint32_t *code_out, int32_t *isnull_out);
DbeStatus	dbe_segreader_dict_value(DbeSegReader *r, uint32_t idx,
									 const void **data_out, uint32_t *len_out);

void		dbe_segreader_close(DbeSegReader *r);


/*
 * --- PGM-Index (F4) -----------------------------------------------------
 *
 * El modelo se construye sobre las claves DISTINTAS del índice, con la
 * posición = rango de la clave distinta.  Ver src/engine/index/pgm.cpp.
 *
 * dbe_pgm_locate() devuelve una VENTANA, que es una pista: quien la usa
 * tiene que verificar los bordes contra las claves reales y, si no
 * alcanzan, hacer la binaria completa.  El modelo nunca es autoridad.
 */
typedef struct DbePgmBuilder DbePgmBuilder;

/*
 * Las claves se pasan en orden, de a una, y nunca están todas juntas en
 * memoria: el cono decreciente es un algoritmo en línea y lo único que se
 * acumula son los tramos.  Es lo que permite indexar una tabla que no entra
 * en RAM — el build lee las claves de un tuplesort que derrama a disco.
 */
DbeStatus	dbe_pgm_builder_create(uint32_t epsilon, DbePgmBuilder **out);
DbeStatus	dbe_pgm_builder_add(DbePgmBuilder *b, int64_t key);
DbeStatus	dbe_pgm_builder_finish(DbePgmBuilder *b);
/* El buffer vive hasta dbe_pgm_destroy(). */
DbeStatus	dbe_pgm_bytes(DbePgmBuilder *b, const void **bytes_out,
						  uint64_t *len_out);
void		dbe_pgm_destroy(DbePgmBuilder *b);

DbeStatus	dbe_pgm_locate(const void *model, uint64_t model_len, int64_t key,
						   uint64_t *lo_out, uint64_t *hi_out);
DbeStatus	dbe_pgm_stats(const void *model, uint64_t model_len,
						  uint64_t *n_out, uint32_t *epsilon_out,
						  uint32_t *nlevels_out, uint64_t *segments_out);


/*
 * --- Filtro de Bloom por segmento (F5) ----------------------------------
 *
 * Descarta stripes con predicados de igualdad sobre columnas que el zone
 * map min/max no puede acotar.  Error de un solo lado: dbe_bloom_contains()
 * en false significa "seguro que no está"; en true, "puede estar".  Un
 * filtro nulo o ilegible contesta true, así que no saber es leer.
 */
typedef struct DbeBloomBuilder DbeBloomBuilder;

DbeStatus	dbe_bloom_builder_create(uint64_t expected, uint32_t bits_per_item,
									 DbeBloomBuilder **out);
DbeStatus	dbe_bloom_builder_add(DbeBloomBuilder *b, uint64_t hash);
DbeStatus	dbe_bloom_builder_finish(DbeBloomBuilder *b,
									 const void **bytes_out, uint64_t *len_out);
void		dbe_bloom_destroy(DbeBloomBuilder *b);

DbeStatus	dbe_bloom_contains(const void *filter, uint64_t len, uint64_t hash,
							   int32_t *maybe_out);
DbeStatus	dbe_bloom_stats(const void *filter, uint64_t len, uint64_t *nbits_out,
							uint32_t *nhashes_out, uint64_t *nitems_out,
							int32_t *valid_out);

#ifdef __cplusplus
}								/* extern "C" */
#endif

#endif							/* DBENGINE_BRIDGE_H */
