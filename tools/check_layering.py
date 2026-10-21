#!/usr/bin/env python3
"""Guarda de arquitectura de la frontera C/C++ (R2 del plan).

Falla el build si alguien rompe alguna de las invariantes.  Analiza el
codigo con los comentarios y los literales de string ya removidos, para no
disparar con menciones en la documentacion del propio codigo.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Comentarios de bloque y de linea.
COMMENTS = r'/\*.*?\*/|//[^\n]*'
# Literales de string y de char.
LITERALS = r'"(?:\\.|[^"\\])*"' + r"|'(?:\\.|[^'\\])*'"

STRIP_COMMENTS = re.compile(COMMENTS, re.DOTALL)
STRIP_ALL = re.compile(COMMENTS + '|' + LITERALS, re.DOTALL)


def _blank(m):
    """Reemplaza por espacios conservando los saltos de linea."""
    return re.sub(r'[^\n]', ' ', m.group(0))


def strip_noise(text, strip_strings=True):
    """Neutraliza comentarios (y opcionalmente literales) sin mover lineas.

    La regla de #include NO puede borrar strings: el header viene entre
    comillas y quedaria invisible.
    """
    pattern = STRIP_ALL if strip_strings else STRIP_COMMENTS
    return pattern.sub(_blank, text)


def sources(patterns):
    out = []
    for pat in patterns:
        out.extend(sorted(ROOT.glob(pat)))
    return [p for p in out if p.is_file()]


CXX_SOURCES = [
    'src/pg/*.cpp',
    'src/engine/*.cpp',
    'src/engine/*/*.cpp',
    'src/include/dbengine/engine/*.hpp',
]

ENGINE_SOURCES = [
    'src/engine/*.cpp',
    'src/engine/*/*.cpp',
    'src/engine/*.hpp',
    'src/engine/*/*.hpp',
]

PG_HEADER = re.compile(
    r'^\s*#\s*include\s*[<"]'
    r'(postgres\.h|postgres_fe\.h|fmgr\.h|miscadmin\.h|c\.h'
    r'|utils/|access/|catalog/|executor/|nodes/|storage/|optimizer/'
    r'|commands/|postmaster/|libpq/|tcop/)'
)

RULES = [
    # (descripcion, archivos, patron prohibido, sugerencia, borrar_literales)
    (
        'src/engine/ no puede incluir headers de Postgres',
        ENGINE_SOURCES,
        PG_HEADER,
        'el motor solo habla con Postgres a traves de dbengine/pgshim.h',
        False,
    ),
    (
        'ereport/elog no puede aparecer en una unidad de traduccion de C++',
        CXX_SOURCES,
        re.compile(r'\b(ereport|elog)\s*\('),
        'devolve un DbeStatus y dejá que la capa C llame a dbe_error_if_not_ok()',
        True,
    ),
    (
        'PG_TRY solo puede aparecer en C puro (un longjmp saltaria destructores)',
        CXX_SOURCES,
        re.compile(r'\bPG_(TRY|CATCH|END_TRY|RE_THROW)\b'),
        'poné el PG_TRY en src/pg/pgshim.c, que se compila como C',
        True,
    ),
    (
        'la capa C++ no puede usar los allocators de Postgres',
        CXX_SOURCES,
        re.compile(r'\b(palloc0?|pfree|repalloc|MemoryContextAlloc\w*)\s*\('),
        'usá dbe_shim_alloc/dbe_shim_free: palloc hace ereport al fallar',
        True,
    ),
    (
        'la capa C debe usar MemoryContext, no malloc directo',
        ['src/pg/*.c'],
        re.compile(r'(?<![\w.>])(malloc|calloc|realloc|free)\s*\('),
        'usá palloc/pfree; la excepcion es pgshim.c, que envuelve malloc a proposito',
        True,
    ),
]

EXEMPT = {
    # pgshim.c envuelve malloc/free a proposito: es el allocator del motor.
    'la capa C debe usar MemoryContext, no malloc directo': {'src/pg/pgshim.c'},
}


def main():
    failures = []

    for desc, patterns, pattern, hint, strip_strings in RULES:
        exempt = EXEMPT.get(desc, set())
        for path in sources(patterns):
            rel = path.relative_to(ROOT).as_posix()
            if rel in exempt:
                continue
            code = strip_noise(path.read_text(encoding='utf-8'), strip_strings)
            for lineno, line in enumerate(code.splitlines(), 1):
                m = pattern.search(line)
                if m:
                    failures.append((desc, rel, lineno, m.group(0).strip(), hint))

    # Invariante estructural: pgshim.c tiene que seguir siendo C.
    shim = ROOT / 'src/pg/pgshim.c'
    if not shim.exists():
        failures.append((
            'src/pg/pgshim.c debe existir', 'src/pg/pgshim.c', 0, '',
            'es el unico lugar donde vive PG_TRY',
        ))

    if not failures:
        print('check-layering: OK  (frontera C/C++ intacta)')
        return 0

    print('check-layering: FALLA — se rompio la frontera C/C++ (R2)\n',
          file=sys.stderr)
    for desc, rel, lineno, snippet, hint in failures:
        print(f'  {rel}:{lineno}: {desc}', file=sys.stderr)
        if snippet:
            print(f'      encontrado: {snippet}', file=sys.stderr)
        print(f'      sugerencia: {hint}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
