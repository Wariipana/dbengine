# dbengine — build vía PGXS.
#
# Mezcla C y C++ en un solo .so.  Las reglas están separadas a propósito:
# los .c se compilan con el CC y los CFLAGS de Postgres, y los .cpp con el
# CXX y los CXXFLAGS de Postgres — que son juegos de flags distintos porque
# los CFLAGS traen opciones que C++ no acepta.  Ver R2 en el plan.

MODULE_big  = dbengine
EXTENSION   = dbengine
DATA        = sql/dbengine--0.1.0.sql
PGFILEDESC  = "dbengine - columnar storage, vectorized execution, learned indexes, time travel"

# --- Fuentes -----------------------------------------------------------------
# CAPA C: toca APIs de Postgres.  PG_TRY solo puede aparecer acá.
DBE_C_SRCS   = $(wildcard src/pg/*.c)
# CAPA C++: motor interno.  No incluye postgres.h.
DBE_CXX_SRCS = $(wildcard src/pg/*.cpp) \
               $(wildcard src/engine/*.cpp) \
               $(wildcard src/engine/*/*.cpp) \
               $(wildcard src/engine/*/*/*.cpp)

OBJS = $(DBE_C_SRCS:.c=.o) $(DBE_CXX_SRCS:.cpp=.o)

# FSST es una copia sin modificar de la implementación de referencia (MIT).
# Se compila sin warnings a propósito: no se le van a arreglar los -Wshadow a
# código de terceros, y check-warnings tiene que seguir siendo significativo
# para el código propio.
src/engine/compress/fsst/%.o: CXXFLAGS += -w

PG_CPPFLAGS  = -I$(CURDIR)/src/include
# Se linkea con $(CC), así que el runtime de C++ hay que pedirlo explícito.
SHLIB_LINK   = -lstdc++

DBE_CXX_STD ?= c++20

# --- Tests -------------------------------------------------------------------
REGRESS      = extension bridge columnar columnar_mvcc columnar_compress columnar_scan columnar_ddl columnar_compat granule pgm workload bloom clustering grouped merge timetravel
REGRESS_OPTS = --inputdir=test/regress --outputdir=test/regress

# Concurrencia: dos sesiones intercaladas con isolationtester.  Corre en
# `make installcheck` después de REGRESS.
ISOLATION      = unique_concurrent merge_during_scan
ISOLATION_OPTS = --inputdir=test/isolation --outputdir=test/isolation \
                 --load-extension=dbengine

EXTRA_CLEAN  = $(OBJS) $(OBJS:.o=.bc) .pgbuild-* \
               test/regress/results test/regress/regression.diffs \
               test/regress/regression.out test/regress/tmp_check \
               test/isolation/results test/isolation/regression.diffs \
               test/isolation/regression.out test/isolation/output_iso

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# --- Chequeo de versión ------------------------------------------------------
# El #error de dbengine.h también lo atrapa, pero un mensaje de make es más
# claro que 200 líneas de errores del preprocesador.
DBE_PG_MAJOR := $(shell $(PG_CONFIG) --version | sed -E 's/[^0-9]*([0-9]+).*/\1/')
ifeq ($(shell test $(DBE_PG_MAJOR) -lt 18 && echo yes),yes)
$(error dbengine requiere PostgreSQL 18 o superior; pg_config apunta a $(DBE_PG_MAJOR). \
Usá: make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config)
endif

# --- Regla para C++ ----------------------------------------------------------
# PGXS ya expone CXXFLAGS consistentes con cómo se compiló el servidor
# (visibilidad, -fPIC, protecciones de stack, ABI de la plataforma). Se
# reusan tal cual y solo se agrega lo que el motor necesita.
#
# No se pueden usar los CFLAGS de Postgres acá: traen
# -Wdeclaration-after-statement y -Wmissing-prototypes, que no son válidos
# en C++.
DBE_CXXFLAGS = -std=$(DBE_CXX_STD) -fexceptions -Wextra $(CXXFLAGS)

%.o: %.cpp
	$(CXX) $(DBE_CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

# --- Dependencias de headers -------------------------------------------------
# PGXS solo genera dependencias si el servidor se compiló con
# --enable-depend, y la regla propia de C++ no genera ninguna.  Sin esto,
# editar un header no recompila nada y quedan objetos viejos linkeados con
# nuevos: el síntoma son warnings de ODR con LTO, o directamente basura.
#
# La regla es gruesa a propósito — cualquier header recompila todo.  Con
# este tamaño de proyecto cuesta segundos y no se puede equivocar.
DBE_HEADERS = $(wildcard src/include/dbengine/*.h) \
              $(wildcard src/include/dbengine/engine/*.hpp) \
              $(wildcard src/engine/*/*.hpp)

$(OBJS): $(DBE_HEADERS)

# --- Guarda de versión del build ---------------------------------------------
# Alternar PG_CONFIG entre versiones sin limpiar deja objetos mezclados: el
# .so linkea, se instala, y recién falla al arrancar el servidor con
# "undefined symbol". Se detecta con un sello por versión.
#
# El chequeo va en tiempo de PARSEO, no como target.  PGXS declara
# `all: all-lib` antes de que este archivo pueda agregarle nada, así que un
# `all: check-pgversion` se ejecuta DESPUÉS de compilar: el build ya pasó, el
# .so ya se escribió, y lo único que se gana es un código de salida distinto
# de cero al final — que es fácil de perder si la salida está filtrada.
# Costó tres bibliotecas fantasma antes de verlo.
DBE_STAMP = .pgbuild-$(DBE_PG_MAJOR)
DBE_STALE = $(filter-out $(DBE_STAMP),$(wildcard .pgbuild-*))

# La guarda es sobre MEZCLAR OBJETOS, así que solo aplica a los targets que
# compilan o instalan.  Quedan exentos `clean` —si no, un árbol mezclado no
# se podría destrabar—, los tests del motor, que no tocan Postgres, y
# `installcheck`, que corre contra lo que ya está instalado.
ifeq ($(filter clean distclean check-engine check-layering installcheck check-crash check-dump check-warnings,$(MAKECMDGOALS)),)
ifneq ($(DBE_STALE),)
$(error el árbol tiene objetos de PostgreSQL $(patsubst .pgbuild-%,%,$(DBE_STALE)) \
y estás compilando contra $(DBE_PG_MAJOR). Corré 'make clean' antes de cambiar de versión)
endif
DBE_STAMP_TOUCHED := $(shell touch $(DBE_STAMP))
endif

# --- Test del motor ----------------------------------------------------------
# El motor C++ no necesita Postgres para probarse.  Se compila aparte y se
# corre contra una referencia exacta; es mucho más rápido que ir y volver por
# pg_regress, y encuentra otra clase de bugs.
#
# DBE_ENGINE_FLAGS agrega flags al build de los tests; el CI lo corre con
# ASan y UBSan:
#   make check-engine DBE_ENGINE_FLAGS="-fsanitize=address,undefined -g"
DBE_ENGINE_FLAGS ?=

.PHONY: check-engine
check-engine:
	@$(CXX) -std=$(DBE_CXX_STD) -O2 -Wall -Wextra $(DBE_ENGINE_FLAGS) -I$(CURDIR)/src/include \
		-o /tmp/dbe-pgm-test $(CURDIR)/test/engine/pgm_test.cpp \
		$(CURDIR)/src/engine/index/pgm.cpp
	@/tmp/dbe-pgm-test
	@$(CXX) -std=$(DBE_CXX_STD) -O2 -Wall -Wextra $(DBE_ENGINE_FLAGS) -I$(CURDIR)/src/include \
		-o /tmp/dbe-bloom-test $(CURDIR)/test/engine/bloom_test.cpp \
		$(CURDIR)/src/engine/zonemap/bloom.cpp
	@/tmp/dbe-bloom-test

# --- Chequeo de warnings -----------------------------------------------------
# Los warnings del compilador no son ruido: los últimos dos que aparecieron
# eran símbolos que PostgreSQL 19 movió de header y que el .so resolvía
# recién al cargarse, o sea que el servidor no arrancaba.  `make` los
# muestra, pero es fácil perderlos entre las líneas de compilación; este
# target los aísla.
.PHONY: check-warnings
check-warnings:
	@$(MAKE) --no-print-directory PG_CONFIG=$(PG_CONFIG) clean >/dev/null
	@$(MAKE) --no-print-directory PG_CONFIG=$(PG_CONFIG) 2>&1 \
		| grep -iE "error|warning" \
		| grep -vE "^gcc|^g\+\+|^/usr/bin/clang" \
		| (! grep . ) \
		&& echo "check-warnings: OK  (sin warnings)"

# --- Test de crash y recuperación --------------------------------------------
# Para y reinicia el cluster, así que no puede ir en pg_regress.
.PHONY: check-crash
check-crash:
	@PGDATA=$${PGDATA:?falta PGDATA} PGBIN=$${PGBIN:-} \
		bash $(CURDIR)/test/crash/recovery.sh

# pg_dump de un cluster y restore en otro.  Levanta sus propios dos clusters
# con los binarios de PGBIN (o los del PATH), así que no necesita el de
# desarrollo; sí necesita la extensión instalada.
.PHONY: check-dump
check-dump:
	@PGBIN=$${PGBIN:-$$($(PG_CONFIG) --bindir)} \
		bash $(CURDIR)/test/dump/roundtrip.sh

# --- Guardas de arquitectura -------------------------------------------------
# Falla el build si alguien rompe la frontera de R2.  La logica esta en un
# script aparte porque hay que ignorar comentarios y literales.
.PHONY: check-layering
check-layering:
	@python3 $(CURDIR)/tools/check_layering.py

all: check-layering
