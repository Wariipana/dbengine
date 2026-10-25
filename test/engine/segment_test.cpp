//===----------------------------------------------------------------------===//
//
// segment_test.cpp
//   SegmentBuilder + SegmentReader contra los valores originales.
//
// El reader decodifica a demanda: get() de una fila de FOR o RLE decodifica
// solo esa, vector() expande todo, y el diccionario FSST se descomprime por
// entrada.  Lo que hay que probar es que el orden de acceso no cambie nada:
// filas en orden, salteadas, al revés, antes y después de pedir el vector,
// y que un puntero devuelto por get() siga valiendo después de las
// llamadas siguientes.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/segment.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace dbe;

static int fallos = 0;

using Valor = std::optional<std::string>;	// nullopt = NULL

static const char *
nombre_enc(uint8_t e)
{
	static const char *n[] = {"none", "rle", "for", "dict", "dict+fsst"};
	return e < 5 ? n[e] : "?";
}

static void
fallar(const char *caso, const char *que, uint32_t fila)
{
	if (fallos++ < 20)
		printf("  FALLA %s: %s en la fila %u\n", caso, que, fila);
}

static bool
igual(const Valor &esperado, const uint8_t *data, uint32_t len, bool is_null)
{
	if (!esperado)
		return is_null;
	return !is_null && len == esperado->size() &&
		std::memcmp(data, esperado->data(), len) == 0;
}

static void
verificar_get(const char *caso, const SegmentReader &r,
			  const std::vector<Valor> &ref, const std::vector<uint32_t> &orden)
{
	// Los punteros de get() valen mientras viva el reader: se guardan todos
	// y se comparan al final, cuando ya pasaron las demás llamadas.
	std::vector<const uint8_t *> ptrs(ref.size());
	std::vector<uint32_t> lens(ref.size());
	std::vector<bool> nulos(ref.size());

	for (uint32_t i : orden)
	{
		const uint8_t *d;
		uint32_t	len;
		bool		is_null;

		r.get(i, &d, &len, &is_null);
		if (!igual(ref[i], d, len, is_null))
			fallar(caso, "get() distinto", i);
		ptrs[i] = d;
		lens[i] = len;
		nulos[i] = is_null;
	}
	for (uint32_t i : orden)
		if (!igual(ref[i], ptrs[i], lens[i], nulos[i]))
			fallar(caso, "un puntero de get() dejó de valer", i);
}

static void
verificar_vector(const char *caso, const SegmentReader &r,
				 const std::vector<Valor> &ref, uint32_t ancho)
{
	const uint8_t *data;
	const uint8_t *nulls;
	uint32_t	w;

	if (!r.vector(&data, &w, &nulls))
	{
		if (ancho > 0 && r.encoding() != kEncDict)
			fallar(caso, "vector() devolvió false con ancho fijo", 0);
		return;
	}
	if (w != ancho)
		fallar(caso, "vector() con otro ancho", 0);
	for (uint32_t i = 0; i < ref.size(); i++)
	{
		const bool	es_nulo = nulls != nullptr && !(nulls[i / 8] & (1u << (i % 8)));

		if (es_nulo != !ref[i].has_value())
			fallar(caso, "bitmap de nulos distinto", i);
		else if (ref[i] && std::memcmp(data + (size_t) i * w, ref[i]->data(), w) != 0)
			fallar(caso, "vector() distinto", i);
	}
}

static void
verificar_dict(const char *caso, const SegmentReader &r,
			   const std::vector<Valor> &ref)
{
	uint32_t	n;

	if (!r.dict(&n))
		return;
	// Primero por código (lo que hace el GROUP BY por diccionario), en
	// orden inverso para que las entradas no se descompriman en orden.
	for (uint32_t i = (uint32_t) ref.size(); i-- > 0;)
	{
		uint32_t	code;
		bool		is_null;
		const uint8_t *d;
		uint32_t	len;

		r.code(i, &code, &is_null);
		if (is_null != !ref[i].has_value())
		{
			fallar(caso, "code() con otro nulo", i);
			continue;
		}
		if (is_null)
			continue;
		if (code >= n)
		{
			fallar(caso, "código fuera de rango", i);
			continue;
		}
		r.dict_value(code, &d, &len);
		if (!igual(ref[i], d, len, false))
			fallar(caso, "dict_value() distinto", i);
	}
}

static void
probar(const char *caso, uint32_t ancho, const std::vector<Valor> &ref,
	   int enc_esperada)
{
	SegmentBuilder b(ancho, true);

	for (const Valor &v : ref)
		b.append(v ? v->data() : nullptr, v ? (uint32_t) v->size() : 0, !v);

	// Copia a un buffer alineado a 8, como el palloc del lado C.
	const std::vector<uint8_t> &bytes = b.finish();
	std::vector<uint64_t> alineado((bytes.size() + 7) / 8);

	std::memcpy(alineado.data(), bytes.data(), bytes.size());

	const uint8_t *buf = reinterpret_cast<const uint8_t *>(alineado.data());
	const uint32_t n = (uint32_t) ref.size();
	const int	fallos_antes = fallos;
	uint8_t		enc;

	std::vector<uint32_t> adelante(n), atras(n), salteado, azar(n);

	for (uint32_t i = 0; i < n; i++)
		adelante[i] = atras[n - 1 - i] = azar[i] = i;
	for (uint32_t i = 0; i < n; i += 997)
		salteado.push_back(i);
	std::shuffle(azar.begin(), azar.end(), std::mt19937(n + ancho));

	{
		SegmentReader r(buf, (uint32_t) bytes.size());

		enc = r.encoding();
		if (r.row_count() != n)
			fallar(caso, "row_count distinto", 0);
		verificar_get(caso, r, ref, salteado);
		verificar_get(caso, r, ref, azar);
		verificar_dict(caso, r, ref);
		verificar_vector(caso, r, ref, ancho);
		verificar_get(caso, r, ref, adelante);
	}
	{
		// Vector primero, y después get() sobre lo ya expandido.
		SegmentReader r(buf, (uint32_t) bytes.size());

		verificar_vector(caso, r, ref, ancho);
		verificar_get(caso, r, ref, atras);
	}
	{
		// Solo filas en orden: el camino del scan, con la pista de corrida.
		SegmentReader r(buf, (uint32_t) bytes.size());

		verificar_get(caso, r, ref, adelante);
		verificar_dict(caso, r, ref);
	}

	if (enc_esperada >= 0 && enc != enc_esperada)
	{
		printf("  FALLA %s: salió %s y se esperaba %s\n", caso, nombre_enc(enc),
			   nombre_enc((uint8_t) enc_esperada));
		fallos++;
	}
	printf("%-28s %-9s %6zu bytes  %s\n", caso, nombre_enc(enc), bytes.size(),
		   fallos == fallos_antes ? "ok" : "FALLA");
}

template <typename T>
static Valor
fijo(T v)
{
	return std::string(reinterpret_cast<const char *>(&v), sizeof(T));
}

int
main()
{
	const uint32_t N = 65536;
	std::mt19937_64 rng(42);

	// Ancho fijo: una columna por codificación y por ancho.
	for (uint32_t w : {1u, 2u, 4u, 8u})
	{
		std::vector<Valor> rle, forv, crudo, rle_nulos;

		for (uint32_t i = 0; i < N; i++)
		{
			uint64_t	v_rle = (i / 300) * 0x0101010101010101ull;
			uint64_t	v_for = 1000 + (rng() % 50);
			uint64_t	v_raw = rng();
			std::string s;

			s.assign(reinterpret_cast<const char *>(&v_rle), w);
			rle.push_back(s);
			s.assign(reinterpret_cast<const char *>(&v_for), w);
			forv.push_back(s);
			s.assign(reinterpret_cast<const char *>(&v_raw), w);
			crudo.push_back(s);
			if (i % 7 == 3)
				rle_nulos.push_back(std::nullopt);
			else
				rle_nulos.push_back(rle.back());
		}

		char		caso[64];

		snprintf(caso, sizeof(caso), "rle ancho %u", w);
		probar(caso, w, rle, kEncRle);
		snprintf(caso, sizeof(caso), "rle con nulos ancho %u", w);
		probar(caso, w, rle_nulos, -1);
		if (w > 1)
		{
			snprintf(caso, sizeof(caso), "for ancho %u", w);
			probar(caso, w, forv, kEncFor);
		}
		snprintf(caso, sizeof(caso), "crudo ancho %u", w);
		probar(caso, w, crudo, kEncNone);
	}

	// Negativos y positivos mezclados en int8.  FOR mira los bytes como
	// enteros sin signo, así que -1 y 1 quedan en los extremos del rango y
	// no comprime: sale crudo.  Es una limitación del builder (ver C2 en
	// doc/plan-cierre.md); acá importa que se lea bien.
	{
		std::vector<Valor> v;

		for (uint32_t i = 0; i < N; i++)
			v.push_back(fijo<int64_t>((int64_t) (rng() % 2000) - 1000));
		probar("int8 con signo mezclado", 8, v, -1);
	}

	// Solo negativos: ahí FOR sí sirve.
	{
		std::vector<Valor> v;

		for (uint32_t i = 0; i < N; i++)
			v.push_back(fijo<int64_t>(-5000 - (int64_t) (rng() % 2000)));
		probar("for int8 negativos", 8, v, kEncFor);
	}

	// FOR con todos iguales salvo nulos: cero bits por valor.
	{
		std::vector<Valor> v;

		for (uint32_t i = 0; i < N; i++)
			v.push_back(i % 2 ? Valor(fijo<int32_t>(7)) : std::nullopt);
		probar("constante con nulos", 4, v, -1);
	}

	// Ancho que no es potencia de dos (uuid, 16 bytes): solo RLE o crudo.
	{
		std::vector<Valor> v;

		for (uint32_t i = 0; i < N; i++)
			v.push_back(std::string(16, (char) ('a' + (i / 4000))));
		probar("rle ancho 16", 16, v, kEncRle);
	}

	// Pocas filas.
	{
		std::vector<Valor> v = {fijo<int64_t>(5), std::nullopt, fijo<int64_t>(5)};

		probar("tres filas", 8, v, -1);
	}

	// Largo variable.
	{
		const char *paises[] = {"Argentina", "Chile", "Uruguay", "Perú", ""};
		std::vector<Valor> dict, url, unicos;

		for (uint32_t i = 0; i < N; i++)
		{
			dict.push_back(i % 11 == 0 ? Valor() : Valor(paises[rng() % 5]));
			url.push_back("https://www.ejemplo.com.ar/catalogo/producto/" +
						  std::to_string(rng() % 20000) + "?ref=portada&utm=" +
						  std::to_string(rng() % 3));
			unicos.push_back(std::to_string(rng()) + std::to_string(rng()));
		}
		url[17] = std::nullopt;
		url[18] = std::string();
		probar("dict texto", 0, dict, kEncDict);
		probar("dict+fsst urls", 0, url, kEncDictFsst);
		probar("texto sin repetir", 0, unicos, -1);
	}

	if (fallos)
	{
		printf("segment_test: %d fallos\n", fallos);
		return 1;
	}
	printf("segment_test: OK\n");
	return 0;
}
