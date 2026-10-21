# Unicidad con inserciones concurrentes.
#
# La fila de s1 vive en su buffer de escritura hasta el commit, pero su TID
# ya está en el índice.  El chequeo de unicidad de s2 encontraba la entrada,
# no encontraba la fila en ningún stripe y dejaba pasar el duplicado: la
# PRIMARY KEY terminaba con dos filas iguales.  Lo correcto, igual que en
# heap, es que s2 espere a s1 y después falle o siga según cómo terminó.

setup
{
	CREATE TABLE uq (id int PRIMARY KEY, v text) USING dbengine_columnar;
}

teardown
{
	DROP TABLE uq;
}

session s1
step s1b	{ BEGIN; }
step s1i	{ INSERT INTO uq VALUES (1, 's1'); }
step s1m	{ INSERT INTO uq SELECT g, 's1' FROM generate_series(10, 20) g; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2i	{ INSERT INTO uq VALUES (1, 's2'); }
step s2k	{ INSERT INTO uq VALUES (15, 's2'); }
step s2o	{ INSERT INTO uq VALUES (2, 's2'); }
step s2s	{ SELECT id, v FROM uq ORDER BY id, v; }

# s1 commitea: s2 espera y después falla por clave duplicada.
permutation s1b s1i s2i s1c s2s

# s1 aborta: s2 espera y después inserta.
permutation s1b s1i s2i s1a s2s

# El conflicto no es la primera fila del INSERT de s1.
permutation s1b s1m s2k s1c s2s

# Claves distintas: nadie espera a nadie.
permutation s1b s1i s2o s1c s2s
