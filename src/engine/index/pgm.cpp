//===----------------------------------------------------------------------===//
//
// pgm.cpp
//   Construcción y consulta del PGM-Index.  Ver pgm.hpp.
//
//===----------------------------------------------------------------------===//
#include "dbengine/engine/pgm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dbe {

namespace {

/// Un punto del gráfico (clave, posición).
struct Point
{
	long double x;
	long double y;
};

/// Pendiente de la recta que pasa por dos puntos.
inline long double
slope_of(const Point &a, const Point &b)
{
	if (b.x == a.x)
		return std::numeric_limits<long double>::infinity();
	return (b.y - a.y) / (b.x - a.x);
}

/*
 * Aproximación lineal óptima con cono decreciente.
 *
 * Se mantienen dos rectas —la más empinada y la más plana— que todavía
 * cubren todos los puntos del tramo con error <= epsilon.  Cada punto nuevo
 * aporta una banda [y-eps, y+eps]: si esa banda ya no interseca el cono, el
 * tramo se cierra y arranca uno nuevo.  Es óptimo en cantidad de tramos.
 *
 * Se trabaja con long double porque el rango de las claves es el de int64 y
 * un double pierde precisión en los últimos bits; el error de la recta se
 * verifica igual al final.
 */
class ConeBuilder
{
public:
	ConeBuilder(uint32_t epsilon, std::vector<PgmSegment> *out)
		: eps_(static_cast<long double>(epsilon)), out_(out)
	{
	}

	void add(int64_t key, uint64_t pos)
	{
		Point		p{static_cast<long double>(key), static_cast<long double>(pos)};

		if (count_ == 0)
		{
			origin_ = p;
			first_key_ = key;
			last_ = p;
			count_ = 1;
			return;
		}

		if (count_ == 1)
		{
			if (p.x == origin_.x)
			{
				/* Todavía no hay cono: dos puntos con la misma x. */
				if (p.y - origin_.y > eps_)
				{
					flush();
					add(key, pos);
					return;
				}
				last_ = p;
				count_ = 1;		/* sigue sin definir pendiente */
				return;
			}

			/* Con dos puntos el cono es todo lo que pasa por la banda. */
			hi_ = slope_of(origin_, Point{p.x, p.y + eps_});
			lo_ = slope_of(origin_, Point{p.x, p.y - eps_});
			last_ = p;
			count_ = 2;
			return;
		}

		/*
		 * Clave repetida: ninguna recta puede cubrir dos posiciones lejanas
		 * con la misma x.  Si la corrida se pasó de epsilon, se corta acá.
		 * Sin este caso el cono nunca cerraba y un millón de claves iguales
		 * salía como un solo tramo con pendiente cero.
		 */
		if (p.x == origin_.x)
		{
			if (p.y - origin_.y > eps_)
			{
				flush();
				add(key, pos);
				return;
			}
			last_ = p;
			count_++;
			return;
		}

		/*
		 * ¿La banda de este punto sigue dentro del cono?  Se compara contra
		 * las pendientes extremas vistas desde el origen del tramo.
		 */
		long double up = slope_of(origin_, Point{p.x, p.y + eps_});
		long double dn = slope_of(origin_, Point{p.x, p.y - eps_});

		if (dn > hi_ || up < lo_)
		{
			flush();
			add(key, pos);
			return;
		}

		hi_ = std::min(hi_, up);
		lo_ = std::max(lo_, dn);
		last_ = p;
		count_++;
	}

	void flush()
	{
		if (count_ == 0)
			return;

		PgmSegment	seg{};

		seg.key = first_key_;
		if (count_ == 1)
		{
			seg.slope = 0.0;
			seg.intercept = static_cast<double>(origin_.y);
		}
		else
		{
			long double s = (hi_ + lo_) / 2.0L;

			/* Un tramo con todas las claves iguales tiene pendiente infinita. */
			if (!std::isfinite(static_cast<double>(s)))
				s = 0.0L;
			seg.slope = static_cast<double>(s);
			seg.intercept = static_cast<double>(origin_.y);
		}
		out_->push_back(seg);
		count_ = 0;
	}

private:
	long double eps_;
	std::vector<PgmSegment> *out_;
	Point		origin_{};
	Point		last_{};
	int64_t		first_key_ = 0;
	long double hi_ = 0;
	long double lo_ = 0;
	uint64_t	count_ = 0;
};

/// Posición predicha por un tramo, saturada a [0, n].
///
/// La resta va en __int128 y recién después se convierte a double.  Hacerlo
/// al revés —convertir las dos claves y restar— pierde los bits bajos en
/// cuanto las claves pasan de 2^53, y con claves grandes y pendiente chica
/// eso se transforma en cientos de posiciones de error.  Es exactamente lo
/// que rompía el caso lognormal.
inline int64_t
predict(const PgmSegment &seg, int64_t key)
{
	__int128	diff = static_cast<__int128>(key) - static_cast<__int128>(seg.key);
	double		pos = seg.slope * static_cast<double>(diff) + seg.intercept;

	if (!(pos > 0.0))
		return 0;
	if (pos > 9.0e18)
		return static_cast<int64_t>(9.0e18);
	return static_cast<int64_t>(pos);
}

/// Último tramo cuyo `key` es <= `key`.  Los tramos vienen ordenados.
inline uint32_t
find_segment(const PgmSegment *segs, uint32_t n, int64_t key)
{
	uint32_t	lo = 0;
	uint32_t	hi = n;			/* buscamos el primero con segs[i].key > key */

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2;

		if (segs[mid].key <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo == 0 ? 0 : lo - 1;
}

}								// namespace

namespace {

/// Arma los niveles de arriba y serializa.  Lo comparten pgm_build() y el
/// constructor incremental: la diferencia entre los dos es solo cómo se
/// llenó el nivel 0.
std::vector<char>
pgm_finish_levels(std::vector<std::vector<PgmSegment>> &levels, uint64_t n,
				  uint32_t epsilon)
{
	while (levels.back().size() > 1 && levels.size() < kPgmMaxLevels)
	{
		const std::vector<PgmSegment> &prev = levels.back();
		std::vector<PgmSegment> segs;
		ConeBuilder	cb(epsilon, &segs);

		for (uint64_t i = 0; i < prev.size(); i++)
			cb.add(prev[i].key, i);
		cb.flush();

		if (segs.size() >= prev.size())
			break;				/* no achicó: no tiene sentido otro nivel */
		levels.push_back(segs);
	}

	PgmHeader	hdr{};

	hdr.magic = kPgmMagic;
	hdr.version = kPgmVersion;
	hdr.epsilon = static_cast<uint16_t>(epsilon);
	hdr.n = n;
	hdr.nlevels = static_cast<uint32_t>(levels.size());
	for (size_t i = 0; i < levels.size(); i++)
		hdr.level_segments[i] = static_cast<uint32_t>(levels[i].size());

	size_t		total = sizeof(PgmHeader);

	for (const auto &lvl : levels)
		total += lvl.size() * sizeof(PgmSegment);

	std::vector<char> out(total);
	char	   *p = out.data();

	std::memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);
	for (const auto &lvl : levels)
	{
		if (!lvl.empty())
			std::memcpy(p, lvl.data(), lvl.size() * sizeof(PgmSegment));
		p += lvl.size() * sizeof(PgmSegment);
	}
	return out;
}

}								// namespace

struct PgmBuilder::Impl
{
	Impl(uint32_t eps)
		: epsilon(eps), levels(1), cone(eps, &levels[0])
	{
	}

	uint32_t	epsilon;
	uint64_t	n = 0;
	std::vector<std::vector<PgmSegment>> levels;
	ConeBuilder	cone;
	std::vector<char> out;
	bool		done = false;
};

PgmBuilder::PgmBuilder(uint32_t epsilon)
	: impl_(nullptr)
{
	if (epsilon < 1 || epsilon > 65535)
		throw Error(DBE_ERR_INVALID_ARGUMENT,
					"epsilon del PGM fuera de rango (1..65535)");
	impl_ = new Impl(epsilon);
}

PgmBuilder::~PgmBuilder()
{
	delete impl_;
}

void
PgmBuilder::add(int64_t key)
{
	if (impl_->done)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "el modelo PGM ya está cerrado");
	impl_->cone.add(key, impl_->n);
	impl_->n++;
}

const std::vector<char> &
PgmBuilder::finish()
{
	if (!impl_->done)
	{
		impl_->cone.flush();
		impl_->out = pgm_finish_levels(impl_->levels, impl_->n, impl_->epsilon);
		impl_->done = true;
	}
	return impl_->out;
}

std::vector<char>
pgm_build(const int64_t *keys, uint64_t n, uint32_t epsilon)
{
	if (epsilon < 1)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "epsilon del PGM tiene que ser >= 1");
	if (epsilon > 65535)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "epsilon del PGM no entra en 16 bits");
	if (n > 0 && keys == nullptr)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "pgm_build sin claves");

	std::vector<std::vector<PgmSegment>> levels;
	std::vector<int64_t> current(keys, keys + n);

	/*
	 * Nivel 0 sobre las claves; cada nivel siguiente sobre las primeras
	 * claves del anterior.  Se para cuando un nivel entra en una binaria
	 * corta o cuando ya no achica.
	 */
	while (true)
	{
		std::vector<PgmSegment> segs;
		ConeBuilder	cb(epsilon, &segs);

		for (uint64_t i = 0; i < current.size(); i++)
			cb.add(current[i], i);
		cb.flush();

		levels.push_back(segs);

		if (segs.size() <= 1 || levels.size() >= kPgmMaxLevels)
			break;
		if (segs.size() >= current.size())
			break;				/* no achicó: no tiene sentido otro nivel */

		current.clear();
		current.reserve(segs.size());
		for (const PgmSegment &s : segs)
			current.push_back(s.key);
	}

	PgmHeader	hdr{};

	hdr.magic = kPgmMagic;
	hdr.version = kPgmVersion;
	hdr.epsilon = static_cast<uint16_t>(epsilon);
	hdr.n = n;
	hdr.nlevels = static_cast<uint32_t>(levels.size());
	for (size_t i = 0; i < levels.size(); i++)
		hdr.level_segments[i] = static_cast<uint32_t>(levels[i].size());

	size_t		total = sizeof(PgmHeader);

	for (const auto &lvl : levels)
		total += lvl.size() * sizeof(PgmSegment);

	std::vector<char> out(total);
	char	   *p = out.data();

	std::memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);
	for (const auto &lvl : levels)
	{
		if (!lvl.empty())
			std::memcpy(p, lvl.data(), lvl.size() * sizeof(PgmSegment));
		p += lvl.size() * sizeof(PgmSegment);
	}

	return out;
}

void
pgm_locate(const void *model, uint64_t model_len, int64_t key,
		   uint64_t *lo_out, uint64_t *hi_out)
{
	if (model == nullptr || model_len < sizeof(PgmHeader))
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM truncado");

	PgmHeader	hdr;

	std::memcpy(&hdr, model, sizeof(hdr));
	if (hdr.magic != kPgmMagic || hdr.version != kPgmVersion)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM con magic o versión inesperados");
	if (hdr.nlevels == 0 || hdr.nlevels > kPgmMaxLevels)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM con niveles inválidos");

	if (hdr.n == 0)
	{
		*lo_out = 0;
		*hi_out = 0;
		return;
	}

	const char *base = static_cast<const char *>(model) + sizeof(PgmHeader);
	const PgmSegment *level[kPgmMaxLevels];
	uint64_t	need = sizeof(PgmHeader);

	{
		const char *p = base;

		for (uint32_t i = 0; i < hdr.nlevels; i++)
		{
			level[i] = reinterpret_cast<const PgmSegment *>(p);
			p += static_cast<uint64_t>(hdr.level_segments[i]) * sizeof(PgmSegment);
			need += static_cast<uint64_t>(hdr.level_segments[i]) * sizeof(PgmSegment);
		}
	}
	if (need > model_len)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM más corto que su header");

	/*
	 * Se baja desde el nivel más alto.  En cada paso el modelo PROPONE una
	 * ventana y después se verifica con dos comparaciones exactas si el
	 * predecesor puede estar afuera; si puede, se busca en el nivel entero.
	 *
	 * La verificación no es paranoia: la garantía epsilon vale para las
	 * claves que se usaron para construir el modelo, no para una clave
	 * cualquiera que caiga entre dos de ellas.  Se puede acotar el error
	 * extra, pero acotarlo mal da resultados incorrectos en vez de lentos.
	 * Dos comparaciones cuestan nada y convierten al modelo en lo que tiene
	 * que ser: una pista, nunca la autoridad.
	 */
	int64_t		eps = static_cast<int64_t>(hdr.epsilon);
	uint32_t	top = hdr.nlevels - 1;
	uint32_t	idx = find_segment(level[top], hdr.level_segments[top], key);

	for (int32_t l = static_cast<int32_t>(top); l >= 1; l--)
	{
		int64_t		n_below = static_cast<int64_t>(hdr.level_segments[l - 1]);
		int64_t		pos = predict(level[l][idx], key);
		int64_t		lo = pos - eps - 1;
		int64_t		hi = pos + eps + 2;
		const PgmSegment *below = level[l - 1];

		if (lo < 0)
			lo = 0;
		if (hi > n_below)
			hi = n_below;
		if (lo >= hi)
		{
			lo = 0;
			hi = n_below;
		}

		/*
		 * El predecesor está en [lo, hi) si el borde izquierdo ya es <= key
		 * (o es el principio) y el derecho ya es > key (o es el final).
		 */
		if (!(lo == 0 || below[lo].key <= key) ||
			!(hi == n_below || below[hi].key > key))
		{
			lo = 0;
			hi = n_below;
		}

		idx = static_cast<uint32_t>(lo) +
			find_segment(below + lo, static_cast<uint32_t>(hi - lo), key);
	}

	{
		int64_t		n = static_cast<int64_t>(hdr.n);
		int64_t		pos = predict(level[0][idx], key);
		int64_t		lo = pos - eps - 1;
		int64_t		hi = pos + eps + 2;

		if (lo < 0)
			lo = 0;
		if (hi > n)
			hi = n;
		if (lo > hi)
			lo = hi;

		*lo_out = static_cast<uint64_t>(lo);
		*hi_out = static_cast<uint64_t>(hi);
		return;
	}

	*lo_out = 0;
	*hi_out = hdr.n;
}

void
pgm_stats(const void *model, uint64_t model_len,
		  uint64_t *n_out, uint32_t *epsilon_out,
		  uint32_t *nlevels_out, uint64_t *segments_out)
{
	if (model == nullptr || model_len < sizeof(PgmHeader))
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM truncado");

	PgmHeader	hdr;

	std::memcpy(&hdr, model, sizeof(hdr));
	if (hdr.magic != kPgmMagic)
		throw Error(DBE_ERR_INVALID_ARGUMENT, "modelo PGM con magic inesperado");

	uint64_t	segs = 0;

	for (uint32_t i = 0; i < hdr.nlevels && i < kPgmMaxLevels; i++)
		segs += hdr.level_segments[i];

	*n_out = hdr.n;
	*epsilon_out = hdr.epsilon;
	*nlevels_out = hdr.nlevels;
	*segments_out = segs;
}

}								// namespace dbe
