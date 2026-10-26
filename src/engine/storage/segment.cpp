//===----------------------------------------------------------------------===//
//
// segment.cpp
//   Serialización y compresión de segmentos de columna.  Ver segment.hpp.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/segment.hpp"

// FSST es código de terceros sin modificar (ver el Makefile): sus warnings
// no son nuestros, y con los CXXFLAGS de PG 19 (-Wimplicit-fallthrough)
// romperían check-warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../compress/fsst/fsst.h"
#pragma GCC diagnostic pop

#include "../compress/bitpack.hpp"

#include <algorithm>
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

struct FsstDecoder
{
	fsst_decoder_t dec;
};

namespace {

/// Copia un valor de ancho fijo.  Con el ancho como constante el compilador
/// lo reduce a una sola instrucción; un memcpy de ancho variable terminaba
/// en una llamada a libc por valor, y era la cuarta parte de q24.
inline void
copy_value(uint8_t *dst, const uint8_t *src, uint32_t w)
{
	switch (w)
	{
		case 1: *dst = *src; return;
		case 2: std::memcpy(dst, src, 2); return;
		case 4: std::memcpy(dst, src, 4); return;
		case 8: std::memcpy(dst, src, 8); return;
		default: std::memcpy(dst, src, w); return;
	}
}

template <typename T>
void
unpack_for(T *out, const uint8_t *packed, uint32_t n, uint32_t bits, T min_v)
{
	for (uint32_t i = 0; i < n; i++)
		out[i] = static_cast<T>(min_v + bitpack_get(packed, i, bits));
}

template <typename T>
void
fill_run(uint8_t *dst, const uint8_t *value, uint32_t run)
{
	T			v;
	T		   *out = reinterpret_cast<T *>(dst);

	std::memcpy(&v, value, sizeof(T));
	for (uint32_t i = 0; i < run; i++)
		out[i] = v;
}

}	// namespace

/*
 * Búsqueda de una subcadena sobre FSST sin descomprimir.
 *
 * Un autómata KMP sobre la aguja tiene m + 1 estados: el estado es cuánto
 * del principio de la aguja calza con lo último que se leyó, y m es
 * "encontrada".  Como la tabla de símbolos del segmento es fija, se puede
 * precalcular a qué estado lleva cada símbolo desde cada estado —aplicar
 * sus hasta 8 bytes de una—, y una entrada se recorre con una lectura de
 * tabla por código en vez de escribir los bytes y después buscarlos.  El
 * código de escape (255) trae un byte literal y usa la tabla por byte.
 *
 * Ref.: Pop, Riedl y Neumann, "Efficient pattern matching on FSST-compressed
 * strings" (TUM); es lo que hace Vortex para `%aguja%`.
 */
struct FsstSearch
{
	std::vector<uint8_t> needle;
	uint32_t	m = 0;
	std::vector<uint8_t> by_byte;	///< (m + 1) × 256
	std::vector<uint8_t> by_code;	///< (m + 1) × 255
	bool		hot[256];		///< el código saca del estado 0 (255: escape)
};

namespace {

/* Con uint8_t por estado y m como "encontrada", m tiene que entrar en 254. */
constexpr uint32_t kMaxNeedle = 254;

void
build_search(FsstSearch *s, const fsst_decoder_t &dec)
{
	const uint32_t m = s->m;
	const uint8_t *p = s->needle.data();

	s->by_byte.assign((size_t) (m + 1) * 256, 0);
	s->by_byte[p[0]] = 1;
	for (uint32_t j = 1, x = 0; j < m; j++)
	{
		for (uint32_t c = 0; c < 256; c++)
			s->by_byte[j * 256 + c] = s->by_byte[x * 256 + c];
		s->by_byte[j * 256 + p[j]] = static_cast<uint8_t>(j + 1);
		x = s->by_byte[x * 256 + p[j]];
	}
	for (uint32_t c = 0; c < 256; c++)
		s->by_byte[m * 256 + c] = static_cast<uint8_t>(m);

	s->by_code.assign((size_t) (m + 1) * 255, 0);
	for (uint32_t st = 0; st <= m; st++)
		for (uint32_t code = 0; code < 255; code++)
		{
			uint32_t	cur = st;
			const uint64_t sym = dec.symbol[code];

			const uint32_t len = dec.len[code] < 8 ? dec.len[code] : 8;

			for (uint32_t k = 0; k < len && cur != m; k++)
				cur = s->by_byte[cur * 256 + ((sym >> (8 * k)) & 0xff)];
			s->by_code[st * 255 + code] = static_cast<uint8_t>(cur);
		}
	for (uint32_t code = 0; code < 255; code++)
		s->hot[code] = s->by_code[code] != 0;
	s->hot[255] = true;
}

}	// namespace

int
SegmentReader::dict_contains(uint32_t idx, const uint8_t *needle,
							 uint32_t nlen, const uint8_t *hdr_len) const
{
	if (header_.encoding != kEncDictFsst || idx >= header_.aux_count)
		return -1;
	if (nlen == 0 || nlen > kMaxNeedle)
		return -1;

	FsstSearch *s = nullptr;

	for (const auto &cand : searches_)
		if (cand->m == nlen && std::memcmp(cand->needle.data(), needle, nlen) == 0)
			s = cand.get();
	if (s == nullptr)
	{
		auto	   *fresh = new FsstSearch;

		searches_.emplace_back(fresh);
		fresh->needle.assign(needle, needle + nlen);
		fresh->m = nlen;
		build_search(fresh, fsst_->dec);
		s = fresh;
	}

	const fsst_decoder_t &dec = fsst_->dec;
	const uint8_t *in = fsst_comp_ + offsets_[idx];
	const uint8_t *end = fsst_comp_ + offsets_[idx + 1];
	const uint8_t *by_byte = s->by_byte.data();
	const uint8_t *by_code = s->by_code.data();
	const uint32_t m = s->m;
	uint32_t	st = 0;
	uint32_t	pos = 0;		/* bytes decodificados hasta acá */
	uint32_t	hdr = 0;

	/*
	 * La cabecera se saltea byte por byte: un símbolo puede tener una parte
	 * en la cabecera y otra en los datos.  Después, un paso por código.
	 */
	while (in < end && (pos == 0 || pos < hdr))
	{
		uint8_t		bytes[8];
		uint32_t	k;
		const uint8_t code = *in++;

		if (code == 255)
		{
			if (in >= end)
				return -1;
			bytes[0] = *in++;
			k = 1;
		}
		else
		{
			std::memcpy(bytes, &dec.symbol[code], 8);
			k = dec.len[code] < 8 ? dec.len[code] : 8;
		}
		for (uint32_t i = 0; i < k; i++, pos++)
		{
			if (pos == 0)
			{
				hdr = hdr_len[bytes[0]];
				if (hdr == 255 || hdr == 0)
					return -1;
			}
			if (pos >= hdr)
			{
				st = by_byte[st * 256 + bytes[i]];
				if (st == m)
					return 1;
			}
		}
	}

	/*
	 * En el estado 0 —no hay nada de la aguja a medio calzar, que es casi
	 * siempre— un código que deja en 0 no cambia nada.  Saltearlos mira una
	 * tabla por código sin depender del estado, que es lo caro del recorrido:
	 * cada paso del autómata espera al anterior.
	 */
	const bool *hot = s->hot;

	while (in < end)
	{
		if (st == 0)
		{
			while (in < end && !hot[*in])
				in++;
			if (in >= end)
				break;
		}

		const uint8_t code = *in++;

		if (code == 255)
		{
			if (in >= end)
				return -1;
			st = by_byte[st * 256 + *in++];
		}
		else
			st = by_code[st * 255 + code];
		if (st == m)
			return 1;
	}
	return 0;
}

SegmentReader::~SegmentReader() = default;

/*
 * Prepara el diccionario FSST para descomprimirlo por entrada.  Solo se
 * importa la tabla de símbolos y se reserva el arena, sin tocarlo: una
 * query que filtra por otra columna y lee diez filas descomprime diez
 * entradas, no las 20.000 del stripe.
 */
void
SegmentReader::load_fsst(const uint8_t *data, uint32_t len)
{
	const uint32_t n = header_.aux_count;

	if (len < 4)
		throw Error(DBE_ERR_INTERNAL, "segmento FSST sin tabla de símbolos");

	uint32_t	symlen;

	std::memcpy(&symlen, data, 4);
	if (static_cast<uint64_t>(symlen) + 4 > len)
		throw Error(DBE_ERR_INTERNAL, "tabla de símbolos FSST fuera de rango");

	fsst_.reset(new FsstDecoder);
	if (fsst_import(&fsst_->dec, const_cast<unsigned char *>(data + 4)) == 0)
		throw Error(DBE_ERR_INTERNAL, "no se pudo leer la tabla de símbolos FSST");

	fsst_comp_ = data + 4 + symlen;

	const uint32_t comp_bytes = offsets_[n];

	if (static_cast<uint64_t>(4) + symlen + comp_bytes > len)
		throw Error(DBE_ERR_INTERNAL, "diccionario FSST fuera de rango");
	for (uint32_t i = 0; i < n; i++)
		if (offsets_[i + 1] < offsets_[i])
			throw Error(DBE_ERR_INTERNAL, "offsets del diccionario FSST desordenados");

	codes_ = fsst_comp_ + comp_bytes;
	arena_.reset(new uint8_t[(size_t) comp_bytes * 8 + (size_t) n * 32 + 64]);
	fsst_slots_.assign(n, FsstSlot{0, UINT32_MAX});
}

void
SegmentReader::fsst_entry(uint32_t idx, const uint8_t **data,
						  uint32_t *len) const
{
	FsstSlot   &e = fsst_slots_[idx];

	if (e.len == UINT32_MAX)
	{
		const uint32_t clen = offsets_[idx + 1] - offsets_[idx];
		const size_t room = (size_t) clen * 8 + 32;
		const size_t got = fsst_decompress(&fsst_->dec, clen,
										   fsst_comp_ + offsets_[idx],
										   room, arena_.get() + arena_used_);

		if (got > room)
			throw Error(DBE_ERR_INTERNAL, "entrada de diccionario FSST truncada");
		e.off = static_cast<uint32_t>(arena_used_);
		e.len = static_cast<uint32_t>(got);
		arena_used_ += got;
	}

	*data = arena_.get() + e.off;
	*len = e.len;
}

/*
 * RLE: dónde termina cada corrida.  Se arma recién la primera vez que
 * alguien lee una fila, y valida lo que antes se validaba al abrir.
 */
void
SegmentReader::build_runs() const
{
	const uint32_t w = header_.fixed_width;
	const uint32_t len = header_.data_bytes;

	if (len < 4)
		throw Error(DBE_ERR_INTERNAL, "segmento RLE truncado");
	if ((uint64_t) 4 + (uint64_t) header_.aux_count * (4 + w) > len)
		throw Error(DBE_ERR_INTERNAL, "corrida RLE fuera del segmento");

	run_end_.resize(header_.aux_count);

	const uint8_t *p = data_ + 4;
	uint32_t	written = 0;

	for (uint32_t r = 0; r < header_.aux_count; r++, p += 4 + w)
	{
		uint32_t	run;

		std::memcpy(&run, p, 4);
		if (run > header_.row_count - written)
			throw Error(DBE_ERR_INTERNAL, "las corridas RLE exceden las filas");
		written += run;
		run_end_[r] = written;
	}

	if (written != header_.row_count)
		throw Error(DBE_ERR_INTERNAL, "las corridas RLE no cubren todas las filas");
}

uint32_t
SegmentReader::run_of(uint32_t row) const
{
	if (run_end_.empty())
		build_runs();

	uint32_t	r = run_hint_;

	/* Lo normal es la misma corrida o la siguiente. */
	if (r < run_end_.size() && row < run_end_[r] &&
		(r == 0 || row >= run_end_[r - 1]))
		return r;
	if (r + 1 < run_end_.size() && row >= run_end_[r] && row < run_end_[r + 1])
		return run_hint_ = r + 1;

	r = static_cast<uint32_t>(std::upper_bound(run_end_.begin(), run_end_.end(), row) -
							  run_end_.begin());
	return run_hint_ = r;
}

uint8_t *
SegmentReader::slot(uint32_t row) const
{
	if (!values_)
		values_.reset(new uint8_t[(size_t) header_.row_count * header_.fixed_width]);
	return values_.get() + (size_t) row * header_.fixed_width;
}

/*
 * Expande RLE o FOR a un arreglo contiguo.  Pisa también los valores que
 * get() ya había decodificado, con el mismo resultado.
 */
void
SegmentReader::expand() const
{
	const uint32_t w = header_.fixed_width;
	const uint32_t n = header_.row_count;
	uint8_t    *out = slot(0);

	if (header_.encoding == kEncRle)
	{
		if (run_end_.empty())
			build_runs();

		const uint8_t *p = data_ + 4;
		uint32_t	start = 0;

		for (uint32_t r = 0; r < header_.aux_count; r++, p += 4 + w)
		{
			const uint32_t run = run_end_[r] - start;
			uint8_t    *dst = out + (size_t) start * w;

			switch (w)
			{
				case 1: std::memset(dst, p[4], run); break;
				case 2: fill_run<uint16_t>(dst, p + 4, run); break;
				case 4: fill_run<uint32_t>(dst, p + 4, run); break;
				case 8: fill_run<uint64_t>(dst, p + 4, run); break;
				default:
					for (uint32_t i = 0; i < run; i++)
						std::memcpy(dst + (size_t) i * w, p + 4, w);
			}
			start = run_end_[r];
		}
	}
	else
	{
		const uint32_t bits = header_.bit_width;
		const uint64_t min_v = load_uint(data_, w);
		const uint8_t *packed = data_ + w + 1 + 3;

		switch (w)
		{
			case 1: unpack_for<uint8_t>(out, packed, n, bits, (uint8_t) min_v); break;
			case 2:
				unpack_for<uint16_t>(reinterpret_cast<uint16_t *>(out), packed, n,
									 bits, (uint16_t) min_v);
				break;
			case 4:
				unpack_for<uint32_t>(reinterpret_cast<uint32_t *>(out), packed, n,
									 bits, (uint32_t) min_v);
				break;
			default:
				unpack_for<uint64_t>(reinterpret_cast<uint64_t *>(out), packed, n,
									 bits, min_v);
				break;
		}
	}

	expanded_ = true;
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

	if ((uint64_t) off_data + header_.data_bytes > len)
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
			if (header_.fixed_width == 0)
				throw Error(DBE_ERR_INTERNAL, "segmento RLE sin ancho fijo");
			break;

		case kEncFor:
		{
			const uint32_t w = header_.fixed_width;

			if (w != 1 && w != 2 && w != 4 && w != 8)
				throw Error(DBE_ERR_INTERNAL, "ancho inválido en un segmento FOR");
			if ((uint64_t) w + 4 +
				packed_bytes(header_.row_count, header_.bit_width) > header_.data_bytes)
				throw Error(DBE_ERR_INTERNAL, "valores FOR fuera del segmento");
			break;
		}

		case kEncDict:
			if (header_.offsets_bytes != (header_.aux_count + 1) * sizeof(uint32_t))
				throw Error(DBE_ERR_INTERNAL, "offsets del diccionario inconsistentes");
			codes_ = data_ + offsets_[header_.aux_count];
			break;

		case kEncDictFsst:
			if (header_.offsets_bytes != (header_.aux_count + 1) * sizeof(uint32_t))
				throw Error(DBE_ERR_INTERNAL, "offsets del diccionario inconsistentes");
			load_fsst(data_, header_.data_bytes);
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

	// kEncNone apunta al buffer original; RLE y FOR se expanden la primera
	// vez que alguien pide el vector.
	if (header_.encoding == kEncNone)
		*data = data_;
	else
	{
		if (!expanded_)
			expand();
		*data = values_.get();
	}
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
		fsst_entry(idx, data, len);
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
		{
			const uint32_t w = header_.fixed_width;
			uint8_t    *out = slot(row);

			/*
			 * Se decodifica solo esta fila, en su slot: el puntero tiene que
			 * seguir valiendo después de la próxima llamada, porque un tipo
			 * de ancho fijo por referencia (macaddr8) se devuelve así.
			 */
			if (!expanded_)
			{
				if (header_.encoding == kEncRle)
					copy_value(out, data_ + 4 + (size_t) run_of(row) * (4 + w) + 4, w);
				else
				{
					const uint64_t v = load_uint(data_, w) +
						bitpack_get(data_ + w + 1 + 3, row, header_.bit_width);

					copy_value(out, reinterpret_cast<const uint8_t *>(&v), w);
				}
			}
			*data = out;
			*len = w;
			return;
		}

		case kEncDict:
		case kEncDictFsst:
		{
			uint32_t	code = (uint32_t) bitpack_get(codes_, row,
													  header_.bit_width);

			if (code >= header_.aux_count)
				throw Error(DBE_ERR_INTERNAL, "código de diccionario fuera de rango");

			if (header_.encoding == kEncDictFsst)
				fsst_entry(code, data, len);
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
