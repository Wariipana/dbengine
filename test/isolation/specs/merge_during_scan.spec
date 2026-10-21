# Fusión de stripes mientras un scan está a mitad de camino.
#
# El cursor lista los stripes al empezar y carga los segmentos de cada uno
# recién cuando llega a él.  Si en el medio commitea una fusión, los
# metadatos de los stripes viejos ya no existían para SnapshotSelf y el
# resto del scan salía con todas las columnas en NULL.

# Un setup por INSERT: cada uno commitea solo y deja su propio stripe.
setup { CREATE TABLE mg (a int, b text) USING dbengine_columnar; }
setup { INSERT INTO mg VALUES (1, 'v1'); }
setup { INSERT INTO mg VALUES (2, 'v2'); }
setup { INSERT INTO mg VALUES (3, 'v3'); }
setup { INSERT INTO mg VALUES (4, 'v4'); }
setup { INSERT INTO mg VALUES (5, 'v5'); }
setup { INSERT INTO mg VALUES (6, 'v6'); }

teardown
{
	DROP TABLE mg;
}

session s1
# Los dos caminos de lectura: el seq scan del Table AM y el Custom Scan.
step s1tam	{ SET dbengine.enable_vectorized = off; }
step s1cs	{ SET dbengine.enable_vectorized = on; }
step s1d	{ BEGIN; DECLARE c CURSOR FOR SELECT a, b FROM mg; FETCH 2 FROM c; }
step s1f	{ FETCH ALL FROM c; COMMIT; }

session s2
setup		{ SET dbengine.retention_window = 0; }
step s2m	{ SELECT dbengine.merge_stripes('mg') AS fusionados; }
step s2n	{ SELECT count(*) AS stripes FROM dbengine.stripe WHERE relfilenode = pg_relation_filenode('mg'); }

permutation s1tam s1d s2m s1f s2n
permutation s1cs s1d s2m s1f s2n
