-- F2: compresión adaptativa por segmento y zone maps.

CREATE EXTENSION dbengine;

-- ===========================================================================
-- Elección de codificación
--
-- La heurística arma las candidatas, mide y se queda con la más chica, con
-- el formato crudo siempre disponible como piso.  A esta altura los datos
-- están todos en memoria, así que medir sale casi gratis y no se equivoca.
-- ===========================================================================

CREATE TABLE k (
	seq    int,		-- correlativo: rango chico -> frame of reference
	cat    text,	-- 4 valores distintos -> diccionario
	konst  int,		-- un solo valor -> corridas
	rnd    int,		-- alta entropía -> no hay nada que ganar, queda crudo
	vacia  int		-- todo nulo
) USING dbengine_columnar;

INSERT INTO k
SELECT g,
	   (ARRAY['norte','sur','este','oeste'])[1 + g % 4],
	   42,
	   (g * 2654435761) % 2147483647,
	   NULL
FROM generate_series(1, 20000) g;

SELECT a.attname,
	   CASE sc.encoding
			WHEN 0 THEN 'crudo'
			WHEN 1 THEN 'RLE'
			WHEN 2 THEN 'FOR'
			WHEN 3 THEN 'diccionario'
	   END AS codificacion,
	   sc.null_count,
	   sc.min_value IS NOT NULL AS tiene_zonemap
FROM dbengine.stripe_column sc
JOIN pg_attribute a ON a.attrelid = 'k'::regclass AND a.attnum = sc.attnum
WHERE sc.relfilenode = pg_relation_filenode('k')
ORDER BY sc.attnum;

-- Comprimir no puede cambiar ni un valor.
SELECT count(*), sum(seq), sum(konst), sum(rnd::bigint), count(vacia),
	   count(DISTINCT cat), min(cat), max(cat)
FROM k;

-- Cada codificación devuelve exactamente lo que se guardó.
SELECT seq, cat, konst, rnd FROM k WHERE seq IN (1, 9999, 20000) ORDER BY seq;

-- El segmento de la columna constante tiene que ser diminuto.
SELECT seg_length < 1000 AS constante_es_minima
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('k') AND attnum = 3;

-- ===========================================================================
-- Zone maps (R7)
--
-- El mínimo y el máximo se calculan con la comparación btree del tipo, no
-- comparando bytes: para text con collation o para numeric, comparar bytes
-- daría un rango incorrecto y el pruning descartaría bloques necesarios.
-- ===========================================================================

-- int4 se guarda little-endian; se decodifica a mano para compararlo.
SELECT get_byte(min_value, 0) + get_byte(min_value, 1) * 256
	 + get_byte(min_value, 2) * 65536 + get_byte(min_value, 3) * 16777216
		 AS zonemap_min,
	   get_byte(max_value, 0) + get_byte(max_value, 1) * 256
	 + get_byte(max_value, 2) * 65536 + get_byte(max_value, 3) * 16777216
		 AS zonemap_max,
	   (SELECT min(seq) FROM k) AS min_real,
	   (SELECT max(seq) FROM k) AS max_real
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('k') AND attnum = 1;

-- text: se guarda el varlena entero, para poder reconstruir el Datum sin
-- copiar.  El header puede ser de 1 byte (bit bajo en 1) o de 4, según de
-- dónde venía el valor; las dos formas son válidas.
SELECT CASE WHEN get_byte(min_value, 0) & 1 = 1
			THEN encode(substr(min_value, 2), 'escape')
			ELSE encode(substr(min_value, 5), 'escape') END AS zonemap_min,
	   CASE WHEN get_byte(max_value, 0) & 1 = 1
			THEN encode(substr(max_value, 2), 'escape')
			ELSE encode(substr(max_value, 5), 'escape') END AS zonemap_max,
	   (SELECT min(cat) FROM k) AS min_real,
	   (SELECT max(cat) FROM k) AS max_real
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('k') AND attnum = 2;

-- Una columna toda nula no tiene zone map: no hay ningún valor que acotar.
SELECT min_value IS NULL AND max_value IS NULL AS sin_zonemap, null_count
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('k') AND attnum = 5;

-- ===========================================================================
-- Nulos mezclados: no participan del zone map ni rompen la codificación
-- ===========================================================================

CREATE TABLE kn (a int, t text) USING dbengine_columnar;
INSERT INTO kn
SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END,
	   CASE WHEN g % 5 = 0 THEN NULL ELSE 'grupo-' || (g % 3) END
FROM generate_series(1, 5000) g;

SELECT count(*), count(a), count(t), min(a), max(a), min(t), max(t) FROM kn;

SELECT attnum, null_count, min_value IS NOT NULL AS tiene_zonemap
FROM dbengine.stripe_column
WHERE relfilenode = pg_relation_filenode('kn')
ORDER BY attnum;

-- ===========================================================================
-- Apagar la compresión deja todo crudo, sin cambiar ningún valor
-- ===========================================================================

SET dbengine.enable_compression = off;
CREATE TABLE kc (seq int, cat text) USING dbengine_columnar;
INSERT INTO kc SELECT g, (ARRAY['a','b','c'])[1 + g % 3]
FROM generate_series(1, 20000) g;
RESET dbengine.enable_compression;

SELECT bool_and(encoding = 0) AS todo_crudo
FROM dbengine.stripe_column WHERE relfilenode = pg_relation_filenode('kc');

SELECT count(*), sum(seq), count(DISTINCT cat) FROM kc;

-- Mismos datos, comprimido, ocupa menos.
SELECT pg_relation_size('kc') > pg_relation_size('k') AS crudo_ocupa_mas;

DROP TABLE k, kn, kc;
DROP EXTENSION dbengine CASCADE;
