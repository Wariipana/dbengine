//===----------------------------------------------------------------------===//
//
// bloom.cpp
//   Filtro de Bloom por segmento.  Ver bloom.hpp.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/bloom.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dbe {

namespace {

/*
 * Dos hashes independientes a partir de uno solo (Kirsch-Mitzenmacher): el
 * i-ésimo bit se calcula como h1 + i*h2.  Da la misma tasa de falsos
 * positivos que k hashes de verdad y cuesta k sumas.
 */
inline void
split(uint64_t hash, uint64_t *h1, uint64_t *h2)
{
	*h1 = hash;
	/* Mezcla estilo splitmix64 para que h2 no correlacione con h1. */
	uint64_t	z = hash + 0x9e3779b97f4a7c15ULL;

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	*h2 = (z ^ (z >> 31)) | 1ULL;	/* impar: recorre todo el espacio */
}

}								// namespace

BloomBuilder::BloomBuilder(uint64_t expected, uint32_t bits_per_item)
{
	if (bits_per_item < 1)
		bits_per_item = 1;
	if (bits_per_item > 32)
		bits_per_item = 32;

	uint64_t	n = std::max<uint64_t>(expected, 1);
	uint64_t	m = n * bits_per_item;

	/* Redondeo a byte, y un piso para que un segmento chico siga sirviendo. */
	m = ((m + 63) / 64) * 64;
	if (m < 512)
		m = 512;

	/*
	 * k óptimo = (m/n)·ln2.  Acotado: más de 16 hashes cuesta más de lo que
	 * baja la tasa, y menos de 1 no es un filtro.
	 */
	double		k = (static_cast<double>(m) / static_cast<double>(n)) * 0.6931471805599453;
	int			ki = static_cast<int>(k + 0.5);

	if (ki < 1)
		ki = 1;
	if (ki > 16)
		ki = 16;

	nbits_ = m;
	nhashes_ = static_cast<uint16_t>(ki);
	buf_.assign(sizeof(BloomHeader) + m / 8, 0);
}

void
BloomBuilder::add(uint64_t hash)
{
	if (done_)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "el filtro de Bloom ya está cerrado");

	uint64_t	h1;
	uint64_t	h2;
	uint8_t    *bits = reinterpret_cast<uint8_t *>(buf_.data()) + sizeof(BloomHeader);

	split(hash, &h1, &h2);
	for (uint16_t i = 0; i < nhashes_; i++)
	{
		uint64_t	b = (h1 + static_cast<uint64_t>(i) * h2) % nbits_;

		bits[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
	}
	nitems_++;
}

const std::vector<char> &
BloomBuilder::finish()
{
	if (!done_)
	{
		BloomHeader hdr{};

		hdr.magic = kBloomMagic;
		hdr.version = kBloomVersion;
		hdr.nhashes = nhashes_;
		hdr.nbits = nbits_;
		hdr.nitems = nitems_;
		std::memcpy(buf_.data(), &hdr, sizeof(hdr));
		done_ = true;
	}
	return buf_;
}

bool
bloom_maybe_contains(const void *filter, uint64_t len, uint64_t hash)
{
	BloomHeader hdr;

	/*
	 * Cualquier cosa rara —nulo, truncado, versión desconocida— se contesta
	 * "puede estar".  El filtro es una optimización: no saber es leer.
	 */
	if (filter == nullptr || len < sizeof(BloomHeader))
		return true;

	std::memcpy(&hdr, filter, sizeof(hdr));
	if (hdr.magic != kBloomMagic || hdr.version != kBloomVersion)
		return true;
	if (hdr.nbits == 0 || hdr.nhashes == 0)
		return true;
	if (len < sizeof(BloomHeader) + hdr.nbits / 8)
		return true;

	const uint8_t *bits =
		static_cast<const uint8_t *>(filter) + sizeof(BloomHeader);
	uint64_t	h1;
	uint64_t	h2;

	split(hash, &h1, &h2);
	for (uint16_t i = 0; i < hdr.nhashes; i++)
	{
		uint64_t	b = (h1 + static_cast<uint64_t>(i) * h2) % hdr.nbits;

		if ((bits[b / 8] & static_cast<uint8_t>(1u << (b % 8))) == 0)
			return false;		/* seguro que no está */
	}
	return true;
}

bool
bloom_stats(const void *filter, uint64_t len, uint64_t *nbits_out,
			uint32_t *nhashes_out, uint64_t *nitems_out)
{
	BloomHeader hdr;

	if (filter == nullptr || len < sizeof(BloomHeader))
		return false;
	std::memcpy(&hdr, filter, sizeof(hdr));
	if (hdr.magic != kBloomMagic || hdr.version != kBloomVersion)
		return false;

	*nbits_out = hdr.nbits;
	*nhashes_out = hdr.nhashes;
	*nitems_out = hdr.nitems;
	return true;
}

}								// namespace dbe
