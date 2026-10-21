//===----------------------------------------------------------------------===//
//
// segment.cpp
//   Serialización y compresión de segmentos de columna.  Ver segment.hpp.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/segment.hpp"

#include "../compress/fsst/fsst.h"

#include "../compress/bitpack.hpp"

#include <cstring>
#include <map>
#include <string>

namespace dbe {

namespace {

/// Los tres bloques del cuerpo se alinean a 8 bytes para que un segmento de
/// ancho fijo se pueda leer con acceso alineado directamente del buffer.
constexpr uint32_t kAlign = 8;

/// Solo vale la pena codificar si se ahorra algo real; si no, el costo de
/// decodificar no se paga con nada.
constexpr uint32_t kMinGainPercent = 10;

inline uint32_t
align_up(uint32_t n)
{
	return (n + (kAlign - 1)) & ~(kAlign - 1);
}

inline uint32_t
null_bitmap_size(uint32_t rows)
{
	return (rows + 7) / 8;
}

inline bool
worth_it(uint32_t raw, uint32_t encoded)
{
	if (encoded >= raw)
		return false;
	return (raw - encoded) * 100 >= raw * kMinGainPercent;
}

}	// namespace

SegmentBuilder::SegmentBuilder(uint32_t fixed_width, bool allow_compression)
	: fixed_width_(fixed_width), allow_compression_(allow_compression)
{
	if (fixed_width_ == 0)
		offsets_.push_back(0);
}

void
SegmentBuilder::append(const void *data, uint32_t len, bool is_null)
{
	if (is_null)
	{
		has_nulls_ = true;
		if (fixed_width_ > 0)
			data_.insert(data_.end(), fixed_width_, 0);	// slot en cero
		// Para largo variable no se agrega nada: el rango queda vacío.
	}
	else
	{
		if (data == nullptr)
			throw Error(DBE_ERR_INVALID_ARGUMENT,
						"valor no nulo con puntero nulo en SegmentBuilder::append");

		const uint32_t n = (fixed_width_ > 0) ? fixed_width_ : len;
		const uint8_t *src = static_cast<const uint8_t *>(data);

		data_.insert(data_.end(), src, src + n);
	}

	if (fixed_width_ == 0)
		offsets_.push_back(static_cast<uint32_t>(data_.size()));

	// Bit prendido = NO nulo, igual que el t_bits del heap de Postgres.
	const uint32_t byte = row_count_ / 8;

	if (byte >= nulls_.size())
		nulls_.push_back(0);
	if (!is_null)
		nulls_[byte] |= static_cast<uint8_t>(1u << (row_count_ % 8));

	row_count_++;
}

/*
 * Run-length encoding.  Gana en columnas ordenadas o de baja cardinalidad,
 * que es justo lo que deja el clustering de F6.
 */
bool
SegmentBuilder::try_rle(std::vector<uint8_t> *out, uint32_t *runs) const
{
	if (fixed_width_ == 0 || row_count_ == 0)
		return false;

	const uint32_t w = fixed_width_;
	uint32_t	run_count = 1;

	for (uint32_t i = 1; i < row_count_; i++)
	{
		if (std::memcmp(&data_[(size_t) i * w], &data_[(size_t) (i - 1) * w], w) != 0)
			run_count++;
	}

	const uint32_t raw = row_count_ * w;
	const uint32_t encoded = 4 + run_count * (4 + w);

	if (!worth_it(raw, encoded))
		return false;

	out->assign(encoded, 0);
	std::memcpy(out->data(), &run_count, 4);

	uint8_t    *p = out->data() + 4;
	uint32_t	start = 0;

	for (uint32_t i = 1; i <= row_count_; i++)
	{
		if (i == row_count_ ||
			std::memcmp(&data_[(size_t) i * w], &data_[(size_t) start * w], w) != 0)
		{
			const uint32_t len = i - start;

			std::memcpy(p, &len, 4);
			std::memcpy(p + 4, &data_[(size_t) start * w], w);
			p += 4 + w;
			start = i;
		}
	}

	*runs = run_count;
	return true;
}

/*
 * Frame of reference: se guarda el mínimo y de cada valor solo los bits que
 * hacen falta para la diferencia.  Gana en enteros de rango acotado —
 * fechas, ids correlativos, precios.
 *
 * Los valores se tratan como enteros sin signo.  No importa si el tipo es
 * con signo: (v - min) en aritmética sin signo cae siempre dentro del rango
 * (max - min), que es lo único que hay que representar.
 */
bool
SegmentBuilder::try_for(std::vector<uint8_t> *out, uint32_t *bits) const
{
	if (fixed_width_ == 0 || row_count_ == 0)
		return false;
	if (fixed_width_ != 1 && fixed_width_ != 2 && fixed_width_ != 4 && fixed_width_ != 8)
		return false;

	const uint32_t w = fixed_width_;
	uint64_t	min_v = load_uint(&data_[0], w);
	uint64_t	max_v = min_v;

	for (uint32_t i = 1; i < row_count_; i++)
	{
		const uint64_t v = load_uint(&data_[(size_t) i * w], w);

		if (v < min_v)
			min_v = v;
		if (v > max_v)
			max_v = v;
	}

	const uint32_t nbits = bits_needed(max_v - min_v);
	const uint32_t raw = row_count_ * w;
	const uint32_t encoded = w + 1 + 3 + packed_bytes(row_count_, nbits);

	if (nbits >= w * 8 || !worth_it(raw, encoded))
		return false;

	out->assign(encoded, 0);
	store_uint(out->data(), w, min_v);
	(*out)[w] = static_cast<uint8_t>(nbits);

	uint8_t    *packed = out->data() + w + 1 + 3;

	for (uint32_t i = 0; i < row_count_; i++)
		bitpack_put(packed, i, nbits, load_uint(&data_[(size_t) i * w], w) - min_v);

	*bits = nbits;
	return true;
}

/*
 * Diccionario para largo variable.  Es lo que pide F2 para VARCHAR: en
 * columnas de baja cardinalidad (estados, países, categorías) cada valor
 * pasa a costar unos pocos bits.
 */
bool
SegmentBuilder::fsst_pack(const std::vector<const std::string *> &order,
						  std::vector<uint8_t> *offsets,
						  std::vector<uint8_t> *dict_bytes)
{
	const size_t n = order.size();

	if (n == 0)
		return false;

	std::vector<size_t> lens;
	std::vector<const unsigned char *> ptrs;
	size_t		raw = 0;

	lens.reserve(n);
	ptrs.reserve(n);
	for (const std::string *sp : order)
	{
		lens.push_back(sp->size());
		ptrs.push_back(reinterpret_cast<const unsigned char *>(sp->data()));
		raw += sp->size();
	}

	fsst_encoder_t *enc = fsst_create(n, lens.data(), ptrs.data(), 0);

	if (enc == nullptr)
		return false;

	unsigned char symtab[FSST_MAXHEADER];
	const uint32_t symlen = fsst_export(enc, symtab);

	/* fsst_compress pide holgura: 7 + 2*entrada por cadena. */
	std::vector<unsigned char> out(7 + 2 * raw + 16 * n);
	std::vector<size_t> olens(n);
	std::vector<unsigned char *> optrs(n);
	const size_t done = fsst_compress(enc, n, lens.data(), ptrs.data(),
									  out.size(), out.data(), olens.data(),
									  optrs.data());

	fsst_destroy(enc);

	if (done != n)
		return false;			/* no entró: se queda con el diccionario crudo */

	size_t		comp = 0;

	for (size_t i = 0; i < n; i++)
		comp += olens[i];

	/*
	 * La tabla de símbolos se paga una vez por segmento.  Si con ella el
	 * diccionario no baja al menos un 10%, no vale el trabajo de
	 * descomprimirlo al abrir.
	 */
	if (comp + symlen + 4 >= raw - raw / 10)
		return false;

	{
		std::vector<uint32_t> offs;

		offs.reserve(n + 1);
		offs.push_back(0);
		for (size_t i = 0; i < n; i++)
			offs.push_back(offs.back() + static_cast<uint32_t>(olens[i]));

		offsets->assign(reinterpret_cast<const uint8_t *>(offs.data()),
						reinterpret_cast<const uint8_t *>(offs.data()) +
						offs.size() * sizeof(uint32_t));
	}

	dict_bytes->clear();
	dict_bytes->reserve(4 + symlen + comp);
	{
		const uint32_t len32 = symlen;

		dict_bytes->insert(dict_bytes->end(),
						   reinterpret_cast<const uint8_t *>(&len32),
						   reinterpret_cast<const uint8_t *>(&len32) + 4);
		dict_bytes->insert(dict_bytes->end(), symtab, symtab + symlen);
		for (size_t i = 0; i < n; i++)
			dict_bytes->insert(dict_bytes->end(), optrs[i],
							   optrs[i] + olens[i]);
	}

	return true;
}

bool
SegmentBuilder::try_dict(std::vector<uint8_t> *dict_offsets,
						 std::vector<uint8_t> *data,
						 uint32_t *entries, uint32_t *bits,
						 uint8_t *encoding_out) const
{
	*encoding_out = kEncDict;
	if (fixed_width_ != 0 || row_count_ == 0)
		return false;

	std::map<std::string, uint32_t> codes;
	std::vector<uint32_t> row_codes;
	std::vector<const std::string *> order;

	row_codes.reserve(row_count_);

	for (uint32_t i = 0; i < row_count_; i++)
	{
		const uint32_t start = offsets_[i];
		const uint32_t len = offsets_[i + 1] - start;
		std::string key(reinterpret_cast<const char *>(data_.data() + start), len);
		auto		it = codes.find(key);

		if (it == codes.end())
		{
			const uint32_t code = static_cast<uint32_t>(codes.size());

			it = codes.emplace(std::move(key), code).first;
			order.push_back(&it->first);
		}
		row_codes.push_back(it->second);
	}

	const uint32_t nentries = static_cast<uint32_t>(order.size());
	const uint32_t nbits = bits_needed(nentries > 0 ? nentries - 1 : 0);

	uint32_t	dict_bytes = 0;

	for (const std::string *s : order)
		dict_bytes += static_cast<uint32_t>(s->size());

	const uint32_t raw = static_cast<uint32_t>(data_.size()) +
		(row_count_ + 1) * 4;
	const uint32_t encoded = (nentries + 1) * 4 + dict_bytes +
		packed_bytes(row_count_, nbits);

	if (!worth_it(raw, encoded))
		return false;

	/*
	 * Con el diccionario ya armado, probar si comprimirlo con FSST paga.
	 * Si paga, el segmento sale como kEncDictFsst y los offsets apuntan al
	 * diccionario comprimido; el resto del layout no cambia.
	 */
	{
		std::vector<uint8_t> fsst_offsets;
		std::vector<uint8_t> fsst_dict;

		if (fsst_pack(order, &fsst_offsets, &fsst_dict))
		{
			*dict_offsets = std::move(fsst_offsets);
			data->assign(fsst_dict.begin(), fsst_dict.end());
			data->resize(data->size() + packed_bytes(row_count_, nbits), 0);
			{
				uint8_t    *packed = data->data() + fsst_dict.size();

				for (uint32_t i = 0; i < row_count_; i++)
					bitpack_put(packed, i, nbits, row_codes[i]);
			}
			*entries = nentries;
			*bits = nbits;
			*encoding_out = kEncDictFsst;
			return true;
		}
	}

	// Offsets del diccionario.
	{
		std::vector<uint32_t> offs;

		offs.reserve(nentries + 1);
		offs.push_back(0);
		for (const std::string *s : order)
			offs.push_back(offs.back() + static_cast<uint32_t>(s->size()));

		dict_offsets->assign(reinterpret_cast<const uint8_t *>(offs.data()),
							 reinterpret_cast<const uint8_t *>(offs.data()) +
							 offs.size() * sizeof(uint32_t));
	}

	// Bytes del diccionario seguidos de los códigos empacados.
	data->assign(dict_bytes + packed_bytes(row_count_, nbits), 0);
	{
		uint8_t    *p = data->data();

		for (const std::string *s : order)
		{
			std::memcpy(p, s->data(), s->size());
			p += s->size();
		}

		uint8_t    *packed = data->data() + dict_bytes;

		for (uint32_t i = 0; i < row_count_; i++)
			bitpack_put(packed, i, nbits, row_codes[i]);
	}

	*entries = nentries;
	*bits = nbits;
	return true;
}

/*
 * Elige la codificación más chica.  La heurística es directa: se arman las
 * candidatas, se miran los tamaños y gana la menor, con kEncNone siempre
 * disponible como piso.  No hay muestreo ni adivinanza — a esta altura los
 * datos ya están todos en memoria, así que medir sale casi gratis y no se
 * equivoca.
 */
void
SegmentBuilder::encode_body(SegmentHeader *hdr, std::vector<uint8_t> *offsets,
							std::vector<uint8_t> *data)
{
	std::vector<uint8_t> candidate;
	uint32_t	aux = 0;
	uint32_t	bits = 0;

	choice_.raw_bytes = static_cast<uint32_t>(data_.size()) +
		(fixed_width_ == 0
		 ? static_cast<uint32_t>(offsets_.size() * sizeof(uint32_t))
		 : 0);

	if (!allow_compression_)
	{
		if (fixed_width_ == 0)
			offsets->assign(reinterpret_cast<const uint8_t *>(offsets_.data()),
							reinterpret_cast<const uint8_t *>(offsets_.data()) +
							offsets_.size() * sizeof(uint32_t));
		*data = data_;
	}
	else if (fixed_width_ > 0)
	{
		std::vector<uint8_t> rle;
		std::vector<uint8_t> forc;
		uint32_t	rle_runs = 0;
		uint32_t	for_bits = 0;
		const bool	have_rle = try_rle(&rle, &rle_runs);
		const bool	have_for = try_for(&forc, &for_bits);

		if (have_rle && (!have_for || rle.size() <= forc.size()))
		{
			hdr->encoding = kEncRle;
			aux = rle_runs;
			candidate = std::move(rle);
		}
		else if (have_for)
		{
			hdr->encoding = kEncFor;
			bits = for_bits;
			candidate = std::move(forc);
		}

		if (hdr->encoding != kEncNone)
		{
			offsets->clear();
			*data = std::move(candidate);
		}
		else
			*data = data_;
	}
	else
	{
		std::vector<uint8_t> dict_offs;
		std::vector<uint8_t> dict_data;
		uint32_t	entries = 0;
		uint32_t	dict_bits = 0;

		uint8_t		dict_enc = kEncDict;

		if (try_dict(&dict_offs, &dict_data, &entries, &dict_bits, &dict_enc))
		{
			hdr->encoding = dict_enc;
			aux = entries;
			bits = dict_bits;
			*offsets = std::move(dict_offs);
			*data = std::move(dict_data);
		}
		else
		{
			offsets->assign(reinterpret_cast<const uint8_t *>(offsets_.data()),
							reinterpret_cast<const uint8_t *>(offsets_.data()) +
							offsets_.size() * sizeof(uint32_t));
			*data = data_;
		}
	}

	hdr->bit_width = static_cast<uint8_t>(bits);
	hdr->aux_count = aux;

	choice_.encoding = hdr->encoding;
	choice_.encoded_bytes = static_cast<uint32_t>(offsets->size() + data->size());
}

const std::vector<uint8_t> &
SegmentBuilder::finish()
{
	SegmentHeader hdr;
	std::vector<uint8_t> offsets;
	std::vector<uint8_t> data;

	std::memset(&hdr, 0, sizeof(hdr));
	hdr.magic = kSegmentMagic;
	hdr.version = kSegmentVersion;
	hdr.flags = has_nulls_ ? kSegFlagHasNulls : 0;
	hdr.row_count = row_count_;
	hdr.null_bitmap_bytes = has_nulls_ ? null_bitmap_size(row_count_) : 0;
	hdr.fixed_width = fixed_width_;
	hdr.encoding = kEncNone;

	encode_body(&hdr, &offsets, &data);

	hdr.offsets_bytes = static_cast<uint32_t>(offsets.size());
	hdr.data_bytes = static_cast<uint32_t>(data.size());

	const uint32_t off_nulls = align_up(sizeof(SegmentHeader));
	const uint32_t off_offsets = align_up(off_nulls + hdr.null_bitmap_bytes);
	const uint32_t off_data = align_up(off_offsets + hdr.offsets_bytes);
	const uint32_t total = off_data + hdr.data_bytes;

	out_.assign(total, 0);
	std::memcpy(out_.data(), &hdr, sizeof(hdr));

	if (hdr.null_bitmap_bytes > 0)
	{
		// nulls_ puede ser más corto que null_bitmap_bytes si la última
		// fila cayó justo en un byte nuevo que quedó en cero.
		std::memcpy(out_.data() + off_nulls, nulls_.data(),
					nulls_.size() < hdr.null_bitmap_bytes
					? nulls_.size() : hdr.null_bitmap_bytes);
	}
	if (hdr.offsets_bytes > 0)
		std::memcpy(out_.data() + off_offsets, offsets.data(), hdr.offsets_bytes);
	if (hdr.data_bytes > 0)
		std::memcpy(out_.data() + off_data, data.data(), hdr.data_bytes);

	return out_;
}

/*
 * Descomprime el diccionario entero al abrir el segmento.
 *
 * Es la única vez que se paga: después el acceso por fila es idéntico a
 * kEncDict, un bitpack_get para el código y un índice en el arreglo ya
 * descomprimido.  Medido sobre ClickBench, el diccionario de un stripe son
 * ~5 MB entre las tres columnas de texto y se descomprime a 1.200-1.600
 * MB/s.
 */
void
SegmentReader::materialize_fsst_dict(const uint8_t *data, uint32_t len)
{
	const uint32_t n = header_.aux_count;

	if (len < 4)
		throw Error(DBE_ERR_INTERNAL, "segmento FSST sin tabla de símbolos");

	uint32_t	symlen;

	std::memcpy(&symlen, data, 4);
	if (static_cast<uint64_t>(symlen) + 4 > len)
		throw Error(DBE_ERR_INTERNAL, "tabla de símbolos FSST fuera de rango");

	fsst_decoder_t dec;

	if (fsst_import(&dec, const_cast<unsigned char *>(data + 4)) == 0)
		throw Error(DBE_ERR_INTERNAL, "no se pudo leer la tabla de símbolos FSST");

	const uint8_t *comp = data + 4 + symlen;
	const uint32_t comp_bytes = offsets_[n];

	if (static_cast<uint64_t>(4) + symlen + comp_bytes > len)
		throw Error(DBE_ERR_INTERNAL, "diccionario FSST fuera de rango");

	/*
	 * Cota superior de lo descomprimido: FSST nunca expande más de 8 bytes
	 * por código, y el peor caso de un escape son 2 bytes por byte original.
	 */
	dict_offsets_.clear();
	dict_offsets_.reserve(n + 1);
	dict_offsets_.push_back(0);
	expanded_.assign(static_cast<size_t>(comp_bytes) * 8 + 64, 0);

	size_t		out = 0;

	for (uint32_t i = 0; i < n; i++)
	{
		const uint32_t clen = offsets_[i + 1] - offsets_[i];
		const size_t room = expanded_.size() - out;
		const size_t got = fsst_decompress(&dec, clen, comp + offsets_[i],
										   room, expanded_.data() + out);

		if (got > room)
			throw Error(DBE_ERR_INTERNAL, "entrada de diccionario FSST truncada");
		out += got;
		dict_offsets_.push_back(static_cast<uint32_t>(out));
	}

	expanded_.resize(out);
	codes_ = comp + comp_bytes;
}

void
SegmentReader::materialize_rle(const uint8_t *data, uint32_t len)
{
	const uint32_t w = header_.fixed_width;

	if (len < 4)
		throw Error(DBE_ERR_INTERNAL, "segmento RLE truncado");

	expanded_.assign((size_t) header_.row_count * w, 0);

	const uint8_t *p = data + 4;
	uint32_t	written = 0;

	for (uint32_t r = 0; r < header_.aux_count; r++)
	{
		uint32_t	run;

		if ((uint32_t) (p - data) + 4 + w > len)
			throw Error(DBE_ERR_INTERNAL, "corrida RLE fuera del segmento");

		std::memcpy(&run, p, 4);
		p += 4;

		if (written + run > header_.row_count)
			throw Error(DBE_ERR_INTERNAL, "las corridas RLE exceden las filas");

		for (uint32_t i = 0; i < run; i++)
			std::memcpy(&expanded_[(size_t) (written + i) * w], p, w);

		written += run;
		p += w;
	}

	if (written != header_.row_count)
		throw Error(DBE_ERR_INTERNAL, "las corridas RLE no cubren todas las filas");
}

void
SegmentReader::materialize_for(const uint8_t *data, uint32_t len)
{
	const uint32_t w = header_.fixed_width;
	const uint32_t bits = header_.bit_width;

	if (len < w + 4)
		throw Error(DBE_ERR_INTERNAL, "segmento FOR truncado");

	const uint64_t min_v = load_uint(data, w);
	const uint8_t *packed = data + w + 1 + 3;

	if ((uint32_t) (packed - data) + packed_bytes(header_.row_count, bits) > len)
		throw Error(DBE_ERR_INTERNAL, "valores FOR fuera del segmento");

	expanded_.assign((size_t) header_.row_count * w, 0);

	for (uint32_t i = 0; i < header_.row_count; i++)
		store_uint(&expanded_[(size_t) i * w], w,
				   min_v + bitpack_get(packed, i, bits));
}

SegmentReader::SegmentReader(const uint8_t *bytes, uint32_t len)
{
	if (bytes == nullptr || len < sizeof(SegmentHeader))
		throw Error(DBE_ERR_INVALID_ARGUMENT, "segmento truncado");

	std::memcpy(&header_, bytes, sizeof(header_));

	if (header_.magic != kSegmentMagic)
		throw Error(DBE_ERR_INTERNAL, "magic de segmento inválido");
	if (header_.version != kSegmentVersion)
		throw Error(DBE_ERR_INTERNAL, "versión de segmento no soportada");

	const uint32_t off_nulls = align_up(sizeof(SegmentHeader));
	const uint32_t off_offsets = align_up(off_nulls + header_.null_bitmap_bytes);
	const uint32_t off_data = align_up(off_offsets + header_.offsets_bytes);

	if (off_data + header_.data_bytes > len)
		throw Error(DBE_ERR_INTERNAL, "segmento más corto que su propio header");

	if (header_.null_bitmap_bytes > 0)
		nulls_ = bytes + off_nulls;
	if (header_.offsets_bytes > 0)
		offsets_ = reinterpret_cast<const uint32_t *>(bytes + off_offsets);
	data_ = bytes + off_data;

	switch (header_.encoding)
	{
		case kEncNone:
			// Coherencia interna: si es de largo variable tiene que haber
			// row_count+1 offsets.
			if (header_.fixed_width == 0 &&
				header_.offsets_bytes != (header_.row_count + 1) * sizeof(uint32_t))
				throw Error(DBE_ERR_INTERNAL, "cantidad de offsets inconsistente");
			break;

		case kEncRle:
			materialize_rle(data_, header_.data_bytes);
			break;

		case kEncFor:
			materialize_for(data_, header_.data_bytes);
			break;

		case kEncDict:
			if (header_.offsets_bytes != (header_.aux_count + 1) * sizeof(uint32_t))
				throw Error(DBE_ERR_INTERNAL, "offsets del diccionario inconsistentes");
			codes_ = data_ + offsets_[header_.aux_count];
			break;

		case kEncDictFsst:
			if (header_.offsets_bytes != (header_.aux_count + 1) * sizeof(uint32_t))
				throw Error(DBE_ERR_INTERNAL, "offsets del diccionario inconsistentes");
			materialize_fsst_dict(data_, header_.data_bytes);
			break;

		default:
			throw Error(DBE_ERR_INTERNAL, "codificación de segmento desconocida");
	}
}

bool
SegmentReader::vector(const uint8_t **data, uint32_t *width,
					  const uint8_t **nulls) const
{
	if (header_.fixed_width == 0)
		return false;			// largo variable
	if (header_.encoding == kEncDict)
		return false;			// indirecto por códigos

	// kEncNone apunta al buffer original; RLE y FOR ya se expandieron al
	// abrir el segmento.  En los dos casos hay un arreglo contiguo.
	*data = (header_.encoding == kEncNone) ? data_ : expanded_.data();
	*width = header_.fixed_width;
	*nulls = nulls_;
	return true;
}

bool
SegmentReader::dict(uint32_t *size) const noexcept
{
	if (header_.encoding != kEncDict && header_.encoding != kEncDictFsst)
		return false;
	*size = header_.aux_count;
	return true;
}

void
SegmentReader::code(uint32_t row, uint32_t *code_out, bool *is_null) const
{
	if (row >= header_.row_count)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "fila fuera de rango en el segmento");
	if (header_.encoding != kEncDict && header_.encoding != kEncDictFsst)
		throw Error(DBE_ERR_INTERNAL, "code() sobre un segmento sin diccionario");

	if (nulls_ != nullptr &&
		(nulls_[row / 8] & (1u << (row % 8))) == 0)
	{
		*is_null = true;
		*code_out = 0;
		return;
	}

	{
		*is_null = false;
		*code_out = (uint32_t) bitpack_get(codes_, row, header_.bit_width);
		if (*code_out >= header_.aux_count)
			throw Error(DBE_ERR_INTERNAL, "código de diccionario fuera de rango");
	}
}

void
SegmentReader::dict_value(uint32_t idx, const uint8_t **data,
						  uint32_t *len) const
{
	if ((header_.encoding != kEncDict && header_.encoding != kEncDictFsst) ||
		idx >= header_.aux_count)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "entrada de diccionario inválida");

	if (header_.encoding == kEncDictFsst)
	{
		*data = expanded_.data() + dict_offsets_[idx];
		*len = dict_offsets_[idx + 1] - dict_offsets_[idx];
		return;
	}

	*data = data_ + offsets_[idx];
	*len = offsets_[idx + 1] - offsets_[idx];
}

void
SegmentReader::get(uint32_t row, const uint8_t **data, uint32_t *len,
				   bool *is_null) const
{
	if (row >= header_.row_count)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "fila fuera de rango en el segmento");

	if (nulls_ != nullptr &&
		(nulls_[row / 8] & (1u << (row % 8))) == 0)
	{
		*is_null = true;
		*data = nullptr;
		*len = 0;
		return;
	}

	*is_null = false;

	switch (header_.encoding)
	{
		case kEncRle:
		case kEncFor:
			*data = expanded_.data() + (size_t) row * header_.fixed_width;
			*len = header_.fixed_width;
			return;

		case kEncDict:
		case kEncDictFsst:
		{
			uint32_t	code = (uint32_t) bitpack_get(codes_, row,
													  header_.bit_width);

			if (code >= header_.aux_count)
				throw Error(DBE_ERR_INTERNAL, "código de diccionario fuera de rango");

			if (header_.encoding == kEncDictFsst)
			{
				*data = expanded_.data() + dict_offsets_[code];
				*len = dict_offsets_[code + 1] - dict_offsets_[code];
			}
			else
			{
				*data = data_ + offsets_[code];
				*len = offsets_[code + 1] - offsets_[code];
			}
			return;
		}

		default:
			break;
	}

	if (header_.fixed_width > 0)
	{
		*data = data_ + (size_t) row * header_.fixed_width;
		*len = header_.fixed_width;
	}
	else
	{
		const uint32_t start = offsets_[row];

		*data = data_ + start;
		*len = offsets_[row + 1] - start;
	}
}

}	// namespace dbe
