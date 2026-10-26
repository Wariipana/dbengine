//===----------------------------------------------------------------------===//
//
// bridge.cpp
//   Airlock C -> C++.  ÚNICO punto de cruce en esa dirección (R2).
//
// Invariantes que sostiene este archivo:
//
//   1. Ninguna excepción de C++ sale de acá.  Toda función exportada está
//      envuelta en dbe_guard(), que termina en catch(...) y es noexcept.
//   2. Este archivo NO incluye postgres.h y NO usa PG_TRY.  Un setjmp/
//      longjmp en una unidad de traducción de C++ podría saltar por encima
//      de destructores; por eso todo PG_TRY vive en src/pg/pgshim.c, que es
//      C puro.
//
// Consecuencia: los errores viajan como DbeStatus por valor, y el caller de
// C los convierte en ereport() con dbe_error_if_not_ok().
//
//===----------------------------------------------------------------------===//
#include "dbengine/bridge.h"
#include "dbengine/engine/engine.hpp"
#include "dbengine/engine/bloom.hpp"
#include "dbengine/engine/pgm.hpp"
#include "dbengine/engine/segment.hpp"

#include <cstddef>
#include <exception>
#include <new>

#ifndef __cplusplus
#error "bridge.cpp debe compilarse como C++"
#endif

namespace {

/// Envoltorio universal.  noexcept es intencional: si algo lograra escapar
/// de los catch de acá abajo, queremos un fallo ruidoso e inmediato en vez
/// de comportamiento indefinido al volver a un frame de C.
template <typename Fn>
DbeStatus
dbe_guard(Fn &&fn) noexcept
{
	try
	{
		fn();
		return dbe_status_ok();
	}
	catch (const dbe::Error &e)
	{
		/* Preserva el DbeStatus original, incluido el SQLSTATE si venía de PG. */
		return e.status();
	}
	catch (const std::bad_alloc &)
	{
		return dbe_status_make(DBE_ERR_BAD_ALLOC,
							   "el motor se quedó sin memoria");
	}
	catch (const std::exception &e)
	{
		return dbe_status_make(DBE_ERR_EXCEPTION, e.what());
	}
	catch (...)
	{
		return dbe_status_make(DBE_ERR_UNKNOWN_EXCEPTION,
							   "excepción no estándar escapó del motor");
	}
}

}	// namespace

extern "C" {

DbeStatus
dbe_engine_startup(void)
{
	return dbe_guard([] { dbe::startup(); });
}

DbeStatus
dbe_engine_shutdown(void)
{
	return dbe_guard([] { dbe::shutdown(); });
}

DbeStatus
dbe_engine_version(char *version_out, size_t len)
{
	if (version_out == nullptr || len == 0)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "buffer de versión inválido");

	return dbe_guard([version_out, len] {
		const char *v = dbe::version();
		size_t		i = 0;

		for (; i < len - 1 && v[i] != '\0'; i++)
			version_out[i] = v[i];
		version_out[i] = '\0';
	});
}

DbeStatus
dbe_selftest_throw(int32_t kind, int32_t *dtors_run_out)
{
	return dbe_guard([kind, dtors_run_out] {
		dbe::selftest::throw_kind(kind, dtors_run_out);
	});
}

DbeStatus
dbe_selftest_pg_error(int32_t *dtors_run_out)
{
	return dbe_guard([dtors_run_out] {
		dbe::selftest::pg_error(dtors_run_out);
	});
}

DbeStatus
dbe_selftest_roundtrip(int32_t *dtors_run_out)
{
	return dbe_guard([dtors_run_out] {
		dbe::selftest::roundtrip(dtors_run_out);
	});
}

/* --- Segmentos de columna --------------------------------------------- */

DbeStatus
dbe_segbuilder_create(uint32_t fixed_width, int32_t allow_compression,
					  DbeSegBuilder **out)
{
	if (out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "out nulo");

	*out = nullptr;
	return dbe_guard([fixed_width, allow_compression, out] {
		*out = reinterpret_cast<DbeSegBuilder *>(
			new dbe::SegmentBuilder(fixed_width, allow_compression != 0));
	});
}

DbeStatus
dbe_segbuilder_encoding(DbeSegBuilder *b, int32_t *encoding_out,
						uint32_t *raw_bytes_out, uint32_t *encoded_bytes_out)
{
	if (b == nullptr || encoding_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, encoding_out, raw_bytes_out, encoded_bytes_out] {
		const dbe::EncodingChoice &c =
			reinterpret_cast<dbe::SegmentBuilder *>(b)->choice();

		*encoding_out = c.encoding;
		if (raw_bytes_out != nullptr)
			*raw_bytes_out = c.raw_bytes;
		if (encoded_bytes_out != nullptr)
			*encoded_bytes_out = c.encoded_bytes;
	});
}

DbeStatus
dbe_segbuilder_append(DbeSegBuilder *b, const void *data, uint32_t len,
					  int32_t is_null)
{
	if (b == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "builder nulo");

	return dbe_guard([b, data, len, is_null] {
		reinterpret_cast<dbe::SegmentBuilder *>(b)->append(data, len, is_null != 0);
	});
}

DbeStatus
dbe_segbuilder_finish(DbeSegBuilder *b, const void **bytes_out, uint32_t *len_out)
{
	if (b == nullptr || bytes_out == nullptr || len_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, bytes_out, len_out] {
		const std::vector<uint8_t> &v =
			reinterpret_cast<dbe::SegmentBuilder *>(b)->finish();

		*bytes_out = v.data();
		*len_out = static_cast<uint32_t>(v.size());
	});
}

DbeStatus
dbe_segbuilder_rows(DbeSegBuilder *b, uint32_t *out)
{
	if (b == nullptr || out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, out] {
		*out = reinterpret_cast<dbe::SegmentBuilder *>(b)->row_count();
	});
}

void
dbe_segbuilder_destroy(DbeSegBuilder *b)
{
	/* El destructor no puede tirar; aun así no dejamos que nada escape. */
	(void) dbe_guard([b] { delete reinterpret_cast<dbe::SegmentBuilder *>(b); });
}

DbeStatus
dbe_segreader_open(const void *bytes, uint32_t len, DbeSegReader **out)
{
	if (out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "out nulo");

	*out = nullptr;
	return dbe_guard([bytes, len, out] {
		*out = reinterpret_cast<DbeSegReader *>(
			new dbe::SegmentReader(static_cast<const uint8_t *>(bytes), len));
	});
}

DbeStatus
dbe_segreader_rows(DbeSegReader *r, uint32_t *out)
{
	if (r == nullptr || out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, out] {
		*out = reinterpret_cast<dbe::SegmentReader *>(r)->row_count();
	});
}

DbeStatus
dbe_segreader_get(DbeSegReader *r, uint32_t row, const void **data_out,
				  uint32_t *len_out, int32_t *is_null_out)
{
	if (r == nullptr || data_out == nullptr || len_out == nullptr ||
		is_null_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, row, data_out, len_out, is_null_out] {
		const uint8_t *data = nullptr;
		uint32_t	len = 0;
		bool		is_null = false;

		reinterpret_cast<dbe::SegmentReader *>(r)->get(row, &data, &len, &is_null);

		*data_out = data;
		*len_out = len;
		*is_null_out = is_null ? 1 : 0;
	});
}

DbeStatus
dbe_segreader_vector(DbeSegReader *r, int32_t *is_vector_out,
					 const void **data_out, uint32_t *width_out,
					 const void **nulls_out)
{
	if (r == nullptr || is_vector_out == nullptr || data_out == nullptr ||
		width_out == nullptr || nulls_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, is_vector_out, data_out, width_out, nulls_out] {
		const uint8_t *data = nullptr;
		const uint8_t *nulls = nullptr;
		uint32_t	width = 0;

		*is_vector_out = reinterpret_cast<dbe::SegmentReader *>(r)
			->vector(&data, &width, &nulls) ? 1 : 0;
		*data_out = data;
		*width_out = width;
		*nulls_out = nulls;
	});
}

DbeStatus
dbe_segreader_dict(DbeSegReader *r, int32_t *is_dict_out, uint32_t *size_out)
{
	if (r == nullptr || is_dict_out == nullptr || size_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, is_dict_out, size_out] {
		uint32_t	size = 0;

		*is_dict_out = reinterpret_cast<dbe::SegmentReader *>(r)
			->dict(&size) ? 1 : 0;
		*size_out = size;
	});
}

DbeStatus
dbe_segreader_code(DbeSegReader *r, uint32_t row, uint32_t *code_out,
				   int32_t *isnull_out)
{
	if (r == nullptr || code_out == nullptr || isnull_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, row, code_out, isnull_out] {
		bool		is_null = false;

		reinterpret_cast<dbe::SegmentReader *>(r)->code(row, code_out,
													    &is_null);
		*isnull_out = is_null ? 1 : 0;
	});
}

DbeStatus
dbe_segreader_dict_value(DbeSegReader *r, uint32_t idx, const void **data_out,
						 uint32_t *len_out)
{
	if (r == nullptr || data_out == nullptr || len_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, idx, data_out, len_out] {
		const uint8_t *data = nullptr;
		uint32_t	len = 0;

		reinterpret_cast<dbe::SegmentReader *>(r)->dict_value(idx, &data, &len);
		*data_out = data;
		*len_out = len;
	});
}

DbeStatus
dbe_segreader_dict_contains(DbeSegReader *r, uint32_t idx, const void *needle,
							uint32_t nlen, const uint8_t *hdr_len,
							int32_t *result_out)
{
	if (r == nullptr || needle == nullptr || hdr_len == nullptr ||
		result_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([r, idx, needle, nlen, hdr_len, result_out] {
		*result_out = reinterpret_cast<dbe::SegmentReader *>(r)->dict_contains(
			idx, static_cast<const uint8_t *>(needle), nlen, hdr_len);
	});
}

void
dbe_segreader_close(DbeSegReader *r)
{
	(void) dbe_guard([r] { delete reinterpret_cast<dbe::SegmentReader *>(r); });
}

/* --- PGM-Index (F4) ---------------------------------------------------- */

DbeStatus
dbe_pgm_builder_create(uint32_t epsilon, DbePgmBuilder **out)
{
	if (out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([epsilon, out] {
		*out = reinterpret_cast<DbePgmBuilder *>(new dbe::PgmBuilder(epsilon));
	});
}

DbeStatus
dbe_pgm_builder_add(DbePgmBuilder *b, int64_t key)
{
	if (b == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, key] {
		reinterpret_cast<dbe::PgmBuilder *>(b)->add(key);
	});
}

DbeStatus
dbe_pgm_builder_finish(DbePgmBuilder *b)
{
	if (b == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b] {
		(void) reinterpret_cast<dbe::PgmBuilder *>(b)->finish();
	});
}

DbeStatus
dbe_pgm_bytes(DbePgmBuilder *b, const void **bytes_out, uint64_t *len_out)
{
	if (b == nullptr || bytes_out == nullptr || len_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, bytes_out, len_out] {
		const std::vector<char> &v =
			reinterpret_cast<dbe::PgmBuilder *>(b)->finish();

		*bytes_out = v.data();
		*len_out = v.size();
	});
}

void
dbe_pgm_destroy(DbePgmBuilder *b)
{
	(void) dbe_guard([b] { delete reinterpret_cast<dbe::PgmBuilder *>(b); });
}

DbeStatus
dbe_pgm_locate(const void *model, uint64_t model_len, int64_t key,
			   uint64_t *lo_out, uint64_t *hi_out)
{
	if (lo_out == nullptr || hi_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([model, model_len, key, lo_out, hi_out] {
		dbe::pgm_locate(model, model_len, key, lo_out, hi_out);
	});
}

DbeStatus
dbe_pgm_stats(const void *model, uint64_t model_len,
			  uint64_t *n_out, uint32_t *epsilon_out,
			  uint32_t *nlevels_out, uint64_t *segments_out)
{
	if (n_out == nullptr || epsilon_out == nullptr ||
		nlevels_out == nullptr || segments_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([model, model_len, n_out, epsilon_out, nlevels_out,
					  segments_out] {
		dbe::pgm_stats(model, model_len, n_out, epsilon_out, nlevels_out,
					   segments_out);
	});
}

/* --- Filtro de Bloom por segmento (F5) --------------------------------- */

DbeStatus
dbe_bloom_builder_create(uint64_t expected, uint32_t bits_per_item,
						 DbeBloomBuilder **out)
{
	if (out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([expected, bits_per_item, out] {
		*out = reinterpret_cast<DbeBloomBuilder *>(
			new dbe::BloomBuilder(expected, bits_per_item));
	});
}

DbeStatus
dbe_bloom_builder_add(DbeBloomBuilder *b, uint64_t hash)
{
	if (b == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, hash] {
		reinterpret_cast<dbe::BloomBuilder *>(b)->add(hash);
	});
}

DbeStatus
dbe_bloom_builder_finish(DbeBloomBuilder *b, const void **bytes_out,
						 uint64_t *len_out)
{
	if (b == nullptr || bytes_out == nullptr || len_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([b, bytes_out, len_out] {
		const std::vector<char> &v =
			reinterpret_cast<dbe::BloomBuilder *>(b)->finish();

		*bytes_out = v.data();
		*len_out = v.size();
	});
}

void
dbe_bloom_destroy(DbeBloomBuilder *b)
{
	(void) dbe_guard([b] { delete reinterpret_cast<dbe::BloomBuilder *>(b); });
}

DbeStatus
dbe_bloom_contains(const void *filter, uint64_t len, uint64_t hash,
				   int32_t *maybe_out)
{
	if (maybe_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([filter, len, hash, maybe_out] {
		*maybe_out = dbe::bloom_maybe_contains(filter, len, hash) ? 1 : 0;
	});
}

DbeStatus
dbe_bloom_stats(const void *filter, uint64_t len, uint64_t *nbits_out,
				uint32_t *nhashes_out, uint64_t *nitems_out, int32_t *valid_out)
{
	if (nbits_out == nullptr || nhashes_out == nullptr ||
		nitems_out == nullptr || valid_out == nullptr)
		return dbe_status_make(DBE_ERR_INVALID_ARGUMENT, "argumento nulo");

	return dbe_guard([filter, len, nbits_out, nhashes_out, nitems_out,
					  valid_out] {
		*valid_out = dbe::bloom_stats(filter, len, nbits_out, nhashes_out,
									  nitems_out) ? 1 : 0;
	});
}

}	// extern "C"
