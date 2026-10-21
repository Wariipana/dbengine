//===----------------------------------------------------------------------===//
//
// pgm.hpp
//   PGM-Index: aproximación lineal por tramos de la posición de una clave
//   dentro de un arreglo ORDENADO.
//
// La idea: si las claves están ordenadas, el gráfico (clave, posición) es
// monótono creciente y casi siempre casi lineal por tramos.  En vez de
// guardar un árbol con punteros, se guardan rectas: `pos ≈ a*(k - k0) + b`.
// La recta puede errar, pero por construcción **nunca más de ε posiciones**,
// así que después alcanza con mirar una ventana de 2ε+1 elementos.
//
// Contra un btree: el índice ocupa lo que ocupan las claves más un puñado
// de rectas, en vez de un árbol de páginas con punteros; y la búsqueda son
// dos o tres multiplicaciones más una binaria acotada, en vez de log(n)
// saltos de página.  El precio es que solo sirve para claves mapeables a un
// entero de forma monótona, y que insertar de a una no se puede: el modelo
// se construye sobre el arreglo entero.
//
// El error ε es una GARANTÍA, no una estadística: la construcción es la
// aproximación lineal óptima con cono decreciente (Q. Xie et al.), que va
// achicando el cono de rectas factibles y cierra el tramo justo antes de
// que se vuelva vacío.  Se verifica además a la salida (`kVerifyEpsilon`).
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <vector>

#include "dbengine/engine/engine.hpp"

namespace dbe {

/// Un tramo: para claves en [key, siguiente_key), pos ≈ slope*(k-key)+intercept.
struct PgmSegment
{
	int64_t		key;
	double		slope;
	double		intercept;
};

constexpr uint32_t kPgmMagic = 0x4d47504b;	// "KPGM"
constexpr uint16_t kPgmVersion = 1;
constexpr uint32_t kPgmMaxLevels = 8;

/// Cabecera serializada.  Detrás vienen los niveles, de abajo hacia arriba:
/// nivel 0 sobre las claves, nivel i+1 sobre las primeras claves del nivel i.
struct PgmHeader
{
	uint32_t	magic;
	uint16_t	version;
	uint16_t	epsilon;
	uint64_t	n;					///< claves indexadas
	uint32_t	nlevels;
	uint32_t	reserved;
	uint32_t	level_segments[kPgmMaxLevels];
};

static_assert(sizeof(PgmHeader) == 56, "el header del PGM cambió de tamaño");

/// Construye el modelo sobre `keys`, que tiene que venir ORDENADO de forma
/// no decreciente.  Devuelve el modelo serializado.
std::vector<char> pgm_build(const int64_t *keys, uint64_t n, uint32_t epsilon);

/// Constructor incremental.
///
/// El nivel 0 se arma de a un punto —el cono decreciente ya es un algoritmo
/// en línea— así que no hace falta tener las claves todas juntas en memoria.
/// Lo único que se guarda son los tramos, que son órdenes de magnitud menos.
/// Es lo que permite indexar una tabla que no entra en RAM.
class PgmBuilder
{
public:
	explicit PgmBuilder(uint32_t epsilon);
	~PgmBuilder();

	/// Las claves tienen que llegar en orden no decreciente, y `pos` es el
	/// número de orden del punto (0, 1, 2, ...).
	void add(int64_t key);

	/// Cierra el nivel 0, arma los de arriba y serializa.
	const std::vector<char> &finish();

private:
	struct Impl;
	Impl	   *impl_;
};

/// Ventana donde puede estar la primera aparición de `key`.
///
/// Post: si `key` está en el arreglo, su primera aparición está en [lo, hi).
/// Si no está, [lo, hi) es donde habría que insertarla.  Siempre
/// 0 <= lo <= hi <= n.
void pgm_locate(const void *model, uint64_t model_len, int64_t key,
				uint64_t *lo_out, uint64_t *hi_out);

/// Cuántos tramos tiene cada nivel; para poder explicar el tamaño.
void pgm_stats(const void *model, uint64_t model_len,
			   uint64_t *n_out, uint32_t *epsilon_out,
			   uint32_t *nlevels_out, uint64_t *segments_out);

}								// namespace dbe
