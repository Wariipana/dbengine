//===----------------------------------------------------------------------===//
//
// pgm_test.cpp
//   Prueba del motor del PGM contra una referencia exacta.
//
// No necesita Postgres: compila y corre solo.  Es el test que encontró los
// tres bugs de construcción de F4 —la resta en double, la ventana corta por
// uno y el cono que no cerraba con claves repetidas— y el que mostró que el
// modelo tiene que ir sobre las claves distintas.
//
// Se corre con `make check-engine`.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/pgm.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace dbe;

static int fallos = 0;
static long long fallbacks = 0, consultas = 0, ventana_total = 0;

// Exactamente el algoritmo que va a correr en la capa C.
static size_t
lower_bound_con_pista(const std::vector<int64_t> &k, const std::vector<char> &m, int64_t key)
{
	uint64_t lo, hi;
	pgm_locate(m.data(), m.size(), key, &lo, &hi);
	consultas++;
	ventana_total += (hi - lo);

	bool izq = (lo == 0) || (k[lo - 1] < key);
	bool der = (hi == k.size()) || (k[hi] >= key);

	if (!izq || !der)
	{
		fallbacks++;
		lo = 0; hi = k.size();
	}
	return std::lower_bound(k.begin() + lo, k.begin() + hi, key) - k.begin();
}

static void
check(const char *nombre, std::vector<int64_t> keys, uint32_t eps)
{
	std::sort(keys.begin(), keys.end());

	// El modelo va sobre las claves DISTINTAS, con y = rango.  start[r] es
	// la fila donde arranca la corrida de la clave distinta r.
	std::vector<int64_t> D;
	std::vector<uint64_t> start;
	for (size_t i = 0; i < keys.size(); i++)
		if (i == 0 || keys[i] != keys[i - 1]) { D.push_back(keys[i]); start.push_back(i); }
	start.push_back(keys.size());

	auto model = pgm_build(D.data(), D.size(), eps);
	uint64_t n, segs; uint32_t e, lv;
	pgm_stats(model.data(), model.size(), &n, &e, &lv, &segs);

	fallbacks = consultas = ventana_total = 0;
	int malos = 0;

	auto lb = [&](int64_t q) -> size_t {
		size_t r = lower_bound_con_pista(D, model, q);   // rango distinto
		return start[r];                                 // fila
	};

	for (size_t i = 0; i < keys.size(); i += 1 + keys.size() / 200000)
		if (lb(keys[i]) != (size_t)(std::lower_bound(keys.begin(), keys.end(), keys[i]) - keys.begin()))
			malos++;

	std::mt19937_64 rng(999);
	for (int t = 0; t < 20000; t++)
	{
		int64_t q = (int64_t) rng() % 2000000 - 1000000;
		if (lb(q) != (size_t)(std::lower_bound(keys.begin(), keys.end(), q) - keys.begin()))
			malos++;
	}

	size_t bytes = model.size() + D.size() * 8 + (D.size() == keys.size() ? 0 : (D.size() + 1) * 8);
	printf("%-24s filas=%-8zu distintas=%-8zu tramos=%-6llu modelo=%-8zu indice=%-9zu (%.1f B/fila) respaldo=%5.2f%%  %s\n",
		   nombre, keys.size(), D.size(), (unsigned long long) segs, model.size(),
		   bytes, keys.empty() ? 0.0 : (double) (bytes + keys.size() * 8) / keys.size(),
		   consultas ? 100.0 * fallbacks / consultas : 0.0,
		   malos == 0 ? "ok" : "FALLA");
	if (malos) { fallos++; printf("    %d resultados incorrectos\n", malos); }
	(void) n; (void) e; (void) lv;
}

int
main()
{
	std::mt19937_64 rng(42);
	{ std::vector<int64_t> k(1000000); for (size_t i=0;i<k.size();i++) k[i]=(int64_t)i;
	  check("secuencial", k, 64); }
	{ std::vector<int64_t> k(1000000); for (size_t i=0;i<k.size();i++) k[i]=(int64_t)i*7;
	  check("secuencial con huecos", k, 64); }
	{ std::vector<int64_t> k(1000000); for (auto &x:k) x=(int64_t)(rng()%100000000);
	  check("uniforme", k, 64); }
	{ std::lognormal_distribution<double> d(10,3); std::vector<int64_t> k(500000);
	  for (auto &x:k) x=(int64_t) std::min(d(rng), 9e17);
	  check("lognormal", k, 64); }
	{ std::vector<int64_t> k(500000); for (size_t i=0;i<k.size();i++) k[i]=(int64_t)(i/1000);
	  check("duplicados 1000x", k, 64); }
	{ std::vector<int64_t> k(100000, 7); check("todas iguales", k, 64); }
	{ std::vector<int64_t> k; check("vacio", k, 64); }
	{ std::vector<int64_t> k{5}; check("una sola", k, 64); }
	{ std::vector<int64_t> k{INT64_MIN, 0, INT64_MAX}; check("extremos int64", k, 64); }
	{ std::vector<int64_t> k(200000); for (auto &x:k) x=(int64_t)(rng()%10000000);
	  check("uniforme eps=4", k, 4); }
	{ std::vector<int64_t> k(200000); for (auto &x:k) x=(int64_t)(rng()%10000000);
	  check("uniforme eps=1024", k, 1024); }
	// clusterizado: bloques densos separados por huecos enormes
	{ std::vector<int64_t> k; for (int b=0;b<200;b++) for (int i=0;i<5000;i++)
	    k.push_back((int64_t)b*1000000000LL + i);
	  check("clusterizado", k, 64); }

	// El constructor incremental —el que usa el build que no entra en RAM—
	// tiene que dar EXACTAMENTE el mismo modelo que el de una sola pasada.
	{
		std::mt19937_64 r2(7);
		int distintos = 0;
		for (int caso = 0; caso < 6; caso++)
		{
			std::vector<int64_t> k(50000);
			for (size_t i = 0; i < k.size(); i++)
				k[i] = (caso % 2) ? (int64_t) (r2() % 1000000) : (int64_t) (i / (caso + 1));
			std::sort(k.begin(), k.end());
			k.erase(std::unique(k.begin(), k.end()), k.end());

			auto batch = pgm_build(k.data(), k.size(), 64);
			PgmBuilder b(64);
			for (int64_t x : k)
				b.add(x);
			const auto &inc = b.finish();
			if (batch != inc)
				distintos++;
		}
		printf("%-24s %s\n", "incremental == batch", distintos ? "FALLA" : "ok");
		if (distintos) fallos++;
	}

	printf("\n%s\n", fallos ? "HAY FALLAS" : "todo ok");
	return fallos != 0;
}
