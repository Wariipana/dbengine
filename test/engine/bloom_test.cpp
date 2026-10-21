//===----------------------------------------------------------------------===//
//
// bloom_test.cpp
//   El filtro de Bloom contra una referencia exacta.
//
// Lo único que hay que probar de verdad es la garantía que lo hace usable:
// **nunca un falso negativo**.  Si el filtro dijera "no está" para una
// clave que sí está, el resultado de una query sería incorrecto — que es
// justo lo que un descarte de bloques no puede hacer nunca.  La tasa de
// falsos positivos se mide para saber si el tamaño elegido sirve, pero un
// falso positivo solo cuesta leer un stripe de más.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/bloom.hpp"

#include <cstdio>
#include <random>
#include <unordered_set>

using namespace dbe;

static int fallos = 0;

static void
probar(const char *nombre, uint64_t n, uint32_t bits)
{
	std::mt19937_64 rng(1234 + n + bits);
	std::unordered_set<uint64_t> presentes;
	BloomBuilder b(n, bits);

	for (uint64_t i = 0; i < n; i++)
	{
		uint64_t h = rng();
		presentes.insert(h);
		b.add(h);
	}
	const auto &f = b.finish();

	// 1. sin falsos negativos: TODO lo insertado tiene que dar "puede estar"
	uint64_t fn = 0;
	for (uint64_t h : presentes)
		if (!bloom_maybe_contains(f.data(), f.size(), h))
			fn++;

	// 2. tasa de falsos positivos sobre claves que no están
	uint64_t fp = 0, pruebas = 100000;
	for (uint64_t i = 0; i < pruebas; i++)
	{
		uint64_t h = rng();
		if (presentes.count(h))
			continue;
		if (bloom_maybe_contains(f.data(), f.size(), h))
			fp++;
	}

	uint64_t nbits; uint32_t k; uint64_t nitems;
	bloom_stats(f.data(), f.size(), &nbits, &k, &nitems);

	printf("%-22s n=%-7llu bits/item=%-3u k=%-3u tamaño=%-8zu falsos+=%5.2f%%  %s\n",
		   nombre, (unsigned long long) n, bits, k, f.size(),
		   100.0 * fp / pruebas, fn == 0 ? "sin falsos negativos" : "FALSOS NEGATIVOS");
	if (fn != 0)
	{
		fallos++;
		printf("    %llu falsos negativos\n", (unsigned long long) fn);
	}
}

int
main()
{
	probar("segmento tipico", 65536, 8);
	probar("presupuesto chico", 65536, 4);
	probar("presupuesto grande", 65536, 16);
	probar("segmento chico", 1000, 8);
	probar("un solo valor", 1, 8);
	probar("vacio", 0, 8);

	// Un filtro roto o ausente tiene que contestar "puede estar": no saber
	// es leer, nunca saltear.
	{
		char basura[64] = {0};
		bool ok = bloom_maybe_contains(nullptr, 0, 42) &&
			bloom_maybe_contains(basura, sizeof(basura), 42) &&
			bloom_maybe_contains(basura, 3, 42);

		printf("%-22s %s\n", "filtro invalido", ok ? "conservador: ok" : "FALLA");
		if (!ok)
			fallos++;
	}

	printf("\n%s\n", fallos ? "HAY FALLAS" : "todo ok");
	return fallos != 0;
}
