//===----------------------------------------------------------------------===//
//
// bitpack.hpp
//   Empaquetado de enteros de ancho arbitrario en bits.
//
// Sin SIMD ni trucos: la versión obvia y verificable.  Si en F3 aparece
// como cuello de botella se optimiza con números en la mano.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace dbe {

/// Bits necesarios para representar valores en [0, max_value].
inline uint32_t
bits_needed(uint64_t max_value)
{
	uint32_t	bits = 0;

	while (max_value > 0)
	{
		bits++;
		max_value >>= 1;
	}
	return bits;
}

/// Lee un entero sin signo de `width` bytes (1, 2, 4 u 8).
inline uint64_t
load_uint(const uint8_t *p, uint32_t width)
{
	uint64_t	v = 0;

	std::memcpy(&v, p, width);
	return v;
}

inline void
store_uint(uint8_t *p, uint32_t width, uint64_t v)
{
	std::memcpy(p, &v, width);
}

/*
 * Los lectores leen 8 bytes de una y después desplazan, así que el área
 * empaquetada lleva relleno al final para que la última lectura no se pase
 * del buffer.
 */
constexpr uint32_t kBitpackPad = 8;

inline uint32_t
packed_bytes(uint32_t count, uint32_t bits)
{
	if (bits == 0)
		return 0;
	return (uint32_t) (((uint64_t) count * bits + 7) / 8) + kBitpackPad;
}

/// Ancho máximo que puede leerse con el camino rápido de una sola palabra.
constexpr uint32_t kBitpackFastMaxBits = 57;

/// Escribe `value` (de `bits` bits) en la posición `index` de `out`.
/// `out` tiene que estar dimensionado y en cero.
inline void
bitpack_put(uint8_t *out, uint32_t index, uint32_t bits, uint64_t value)
{
	if (bits == 0)
		return;

	uint64_t	bit_pos = (uint64_t) index * bits;

	for (uint32_t i = 0; i < bits; i++)
	{
		if ((value >> i) & 1u)
		{
			uint64_t	b = bit_pos + i;

			out[b >> 3] |= (uint8_t) (1u << (b & 7));
		}
	}
}

/*
 * Lectura de a una palabra: se traen 8 bytes desde el byte que contiene el
 * primer bit y se desplaza.  Vale mientras el valor entre entero en esos 8
 * bytes, o sea bits + 7 <= 64.  El relleno de packed_bytes() garantiza que
 * los 8 bytes siempre existen.
 *
 * La versión bit a bit tardaba `bits` iteraciones por valor y se notaba: en
 * una columna con diccionario de 1000 entradas son 10 vueltas por fila.
 */
inline uint64_t
bitpack_get(const uint8_t *in, uint32_t index, uint32_t bits)
{
	if (bits == 0)
		return 0;

	uint64_t	bit_pos = (uint64_t) index * bits;

	if (bits <= kBitpackFastMaxBits)
	{
		uint64_t	chunk;

		std::memcpy(&chunk, in + (bit_pos >> 3), sizeof(chunk));
		return (chunk >> (bit_pos & 7)) & (((uint64_t) 1 << bits) - 1);
	}

	uint64_t	value = 0;

	for (uint32_t i = 0; i < bits; i++)
	{
		uint64_t	b = bit_pos + i;

		if (in[b >> 3] & (1u << (b & 7)))
			value |= (uint64_t) 1 << i;
	}
	return value;
}

}	// namespace dbe
