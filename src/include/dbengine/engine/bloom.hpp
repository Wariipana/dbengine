//===----------------------------------------------------------------------===//
//
// bloom.hpp
//   Filtro de Bloom por segmento, para descartar stripes con predicados de
//   igualdad.
//
// Por qué acá y no un modelo aprendido: descartar un stripe con `col = k`
// es responder "¿está k entre estas 65.536 claves?".  Eso es pertenencia a
// un conjunto, no aproximación de una función suave — ninguna regresión ni
// red chica lo hace, y una que pudiera sería una tabla de hash cara.  Un
// Bloom lo hace en espacio óptimo y, lo que decide, **con error de un solo
// lado**: puede decir "puede estar" cuando no está, nunca al revés.  Así el
// filtro no puede cambiar un resultado, solo cuánto se lee.
//
// Complementa a los zone maps min/max de F2, no los reemplaza: el min/max
// cubre los rangos y las columnas correlacionadas con el orden de
// inserción; el Bloom cubre la igualdad sobre columnas que no lo están, que
// es exactamente el único hueco que la medición de F5 encontró.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <vector>

#include "dbengine/engine/engine.hpp"

namespace dbe {

constexpr uint32_t kBloomMagic = 0x4d4c4244;	// "DBLM"
constexpr uint16_t kBloomVersion = 1;

struct BloomHeader
{
	uint32_t	magic;
	uint16_t	version;
	uint16_t	nhashes;		///< k
	uint64_t	nbits;			///< m, múltiplo de 8
	uint64_t	nitems;			///< n insertados (informativo)
};

static_assert(sizeof(BloomHeader) == 24, "el header del bloom cambió de tamaño");

/// Constructor incremental: los hashes se van pasando de a uno.
class BloomBuilder
{
public:
	/// `expected` es cuántos valores se van a insertar y `bits_per_item` el
	/// presupuesto.  k se deriva del óptimo teórico, acotado a [1, 16].
	BloomBuilder(uint64_t expected, uint32_t bits_per_item);

	void add(uint64_t hash);

	/// Serializa.  Devuelve una referencia que vive mientras el builder.
	const std::vector<char> &finish();

private:
	std::vector<char> buf_;
	uint64_t	nbits_ = 0;
	uint16_t	nhashes_ = 0;
	uint64_t	nitems_ = 0;
	bool		done_ = false;
};

/// ¿Puede estar?  false significa "seguro que no"; true, "puede que sí".
/// Un filtro nulo o inválido devuelve true: no aporta información, y la
/// respuesta segura es leer el stripe.
bool bloom_maybe_contains(const void *filter, uint64_t len, uint64_t hash);

/// Bits por item que se usaron, para poder explicar el tamaño.
bool bloom_stats(const void *filter, uint64_t len, uint64_t *nbits_out,
				 uint32_t *nhashes_out, uint64_t *nitems_out);

}								// namespace dbe
