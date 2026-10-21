#!/usr/bin/env python3
"""Saca del page cache los archivos de una relación de Postgres.

No hay forma de vaciar el page cache entero sin ser root, pero sí de pedirle
al kernel que suelte las páginas de UN archivo, que es justo lo que hace
falta para medir un scan en frío.  Requiere un CHECKPOINT antes: las páginas
sucias no se sueltan.

    bench/evict.py <datadir> <ruta relativa de pg_relation_filepath>
"""

import os
import sys


def evict(path):
    """Devuelve los bytes que se pidieron soltar, o 0 si el archivo no está."""
    try:
        fd = os.open(path, os.O_RDONLY)
    except FileNotFoundError:
        return 0
    try:
        size = os.fstat(fd).st_size
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        return size
    finally:
        os.close(fd)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    datadir, relpath = sys.argv[1], sys.argv[2]
    base = os.path.join(datadir, relpath)

    # Una relación grande se parte en segmentos de 1 GB: base, base.1, base.2...
    total = evict(base)
    seg = 1
    while True:
        n = evict(f"{base}.{seg}")
        if n == 0:
            break
        total += n
        seg += 1

    print(f"{total // (1024 * 1024)} MB soltados del cache")
    return 0


if __name__ == "__main__":
    sys.exit(main())
