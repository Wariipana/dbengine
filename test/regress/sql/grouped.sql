-- Orden físico declarado al planner, y GROUP BY empujado al scan.
--
-- Las dos cosas dependen de lo mismo: que las filas estén realmente
-- ordenadas por la columna. Lo que se prueba acá es sobre todo lo que NO
-- tiene que pasar — que la marca de orden sobreviva a un INSERT, o que se
-- agrupe por una columna que no está ordenada.
CREATE EXTENSION dbengine;

-- La tabla de hash propia (flathash.c) agrupa por hash cualquier columna
-- entera, así que también tomaría los GROUP BY de abajo que prueban que,
-- sin orden, no se agrupa de corrido.  Se prende en su propia sección.
SET dbengine.enable_flat_hash = off;
SET max_parallel_workers_per_gather = 0;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;
SET dbengine.staging_threshold_rows = 2000;

CREATE TABLE g (k bigint, s text, n int) USING dbengine_columnar;
INSERT INTO g SELECT (g * 7919) % 500, 'v' || (g % 7), g
	FROM generate_series(1, 20000) g;
CREATE INDEX g_k ON g (k);
ANALYZE g;

-- Copia en heap: es contra esto que se comparan todos los resultados.
CREATE TABLE gh AS SELECT * FROM g;

-- ===========================================================================
-- Sin CLUSTER no hay orden que declarar
-- ===========================================================================
SELECT count(*) AS marcas FROM dbengine.relation_order
WHERE relfilenode = pg_relation_filenode('g');

EXPLAIN (COSTS off) SELECT k, count(*) FROM g GROUP BY k;

-- ===========================================================================
-- Después de CLUSTER: el orden se declara y el GROUP BY se empuja
-- ===========================================================================
CLUSTER g USING g_k;
ANALYZE g;

SELECT count(*) AS marcas FROM dbengine.relation_order
WHERE relfilenode = pg_relation_filenode('g');

EXPLAIN (COSTS off) SELECT k, count(*) FROM g GROUP BY k;
EXPLAIN (COSTS off) SELECT k FROM g ORDER BY k LIMIT 5;

-- Y los resultados tienen que ser los mismos que en heap.
SELECT (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM g GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM gh GROUP BY k) b) AS count_igual;

-- ===========================================================================
-- El mismo camino ordenado, pero en paralelo
--
-- Los stripes se reparten entre los participantes con un contador atómico
-- compartido.  El camino de agrupación ORDENADA no hacía ese reparto: cada
-- worker recorría la tabla entera y los conteos salían multiplicados por la
-- cantidad de participantes.  Sobre ClickBench, `count(*)` daba 10.000.000 y
-- el mismo `GROUP BY UserID` sumaba 30.000.000 — el líder más dos workers.
--
-- No lo agarraba nada porque todo este archivo corría con
-- max_parallel_workers_per_gather = 0.
-- ===========================================================================
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

-- Qué plan se está midiendo.  Si esto deja de decir "Parallel Custom Scan",
-- las dos comprobaciones de abajo pasan a probar el plan de Postgres y no
-- el nuestro, o sea nada.
EXPLAIN (COSTS off) SELECT k, count(*) FROM g GROUP BY k;

-- La suma de los conteos por grupo es la cantidad de filas.  Si algún
-- participante recorre de más, esto se va por arriba.
SELECT (SELECT sum(c) FROM (SELECT k, count(*) c FROM g GROUP BY k) x)
	 = (SELECT count(*) FROM g) AS suma_paralela_igual;

-- Y grupo por grupo, contra heap.
SELECT (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM g GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM gh GROUP BY k) b)
	AS count_paralelo_igual;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

SELECT (SELECT md5(string_agg(k || ':' || s || ':' || mn || ':' || mx, ',' ORDER BY k))
		FROM (SELECT k, sum(n) s, min(n) mn, max(n) mx FROM g GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || s || ':' || mn || ':' || mx, ',' ORDER BY k))
		FROM (SELECT k, sum(n) s, min(n) mn, max(n) mx FROM gh GROUP BY k) b)
	AS sum_min_max_igual;

SELECT (SELECT md5(string_agg(k || ':' || a, ',' ORDER BY k))
		FROM (SELECT k, avg(n) a FROM g GROUP BY k) x)
	 = (SELECT md5(string_agg(k || ':' || a, ',' ORDER BY k))
		FROM (SELECT k, avg(n) a FROM gh GROUP BY k) y) AS avg_igual;

-- Con WHERE: el filtro se evalúa adentro del nodo, antes de agrupar.
SELECT (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM g WHERE n % 3 = 0 GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM gh WHERE n % 3 = 0 GROUP BY k) b)
	AS con_where_igual;

-- ===========================================================================
-- Agrupar por una columna que NO está ordenada: al plan normal
-- ===========================================================================
EXPLAIN (COSTS off) SELECT s, count(*) FROM g GROUP BY s;

SELECT (SELECT md5(string_agg(s || ':' || c, ',' ORDER BY s))
		FROM (SELECT s, count(*) c FROM g GROUP BY s) a)
	 = (SELECT md5(string_agg(s || ':' || c, ',' ORDER BY s))
		FROM (SELECT s, count(*) c FROM gh GROUP BY s) b) AS otra_columna_igual;

-- ===========================================================================
-- Un INSERT rompe el orden: la marca se borra
-- ===========================================================================
-- pg_index.indisclustered sigue diciendo que se clusterizó por g_k, que es
-- justamente por lo que no alcanza con mirarlo.
INSERT INTO g VALUES (0, 'nuevo', 0);
ANALYZE g;

SELECT indisclustered FROM pg_index WHERE indexrelid = 'g_k'::regclass;
SELECT count(*) AS marcas FROM dbengine.relation_order
WHERE relfilenode = pg_relation_filenode('g');

EXPLAIN (COSTS off) SELECT k, count(*) FROM g GROUP BY k;

-- ===========================================================================
-- Y las filas siguen siendo las mismas
-- ===========================================================================
INSERT INTO gh VALUES (0, 'nuevo', 0);
SELECT (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM g GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM gh GROUP BY k) b) AS tras_insert_igual;

-- ===========================================================================
-- La fusión de stripes NO rompe el orden
-- ===========================================================================
-- Es la excepción a "cualquier escritura borra la marca": la fusión reescribe
-- stripes pero respeta el orden de las filas, así que la conserva. Si eso
-- dejara de ser cierto, este test agruparía mal.
SET dbengine.retention_window = 0;

CREATE TABLE f (k bigint, n int) USING dbengine_columnar;
CREATE INDEX f_k ON f (k);
-- Un stripe por transacción, que es la forma que la fusión sabe juntar.
DO $$
BEGIN
	FOR i IN 1..200 LOOP
		INSERT INTO f VALUES ((i * 7919) % 50, i);
		COMMIT;
	END LOOP;
END
$$;
CLUSTER f USING f_k;
ANALYZE f;
CREATE TABLE fh AS SELECT * FROM f;

SELECT count(*) AS marca_antes FROM dbengine.relation_order
WHERE relfilenode = pg_relation_filenode('f');

-- No se comprueba QUE haya fusionado: dbe_stripe_mergeable() exige que el
-- stripe preceda al xmin global, así que con otra sesión de por medio la
-- fusión no ocurre y el test sería intermitente. Lo que se comprueba es el
-- invariante, que tiene que valer haya fusionado o no.
SELECT dbengine.merge_stripes('f') >= 0 AS fusion_sin_error;

SELECT count(*) AS marca_despues FROM dbengine.relation_order
WHERE relfilenode = pg_relation_filenode('f');

-- El orden tiene que seguir siendo real, no solo declarado.
SELECT count(*) AS fuera_de_orden
FROM (SELECT k, lag(k) OVER () AS prev FROM f) s
WHERE prev IS NOT NULL AND k < prev;

SELECT (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM f GROUP BY k) a)
	 = (SELECT md5(string_agg(k || ':' || c, ',' ORDER BY k))
		FROM (SELECT k, count(*) c FROM fh GROUP BY k) b) AS tras_fusion_igual;

-- ===========================================================================
-- GROUP BY por hash: la columna NO tiene que estar ordenada
-- ===========================================================================
-- La versión de grupos contiguos necesita clustering y una tabla se ordena
-- por una sola cosa. Esta aplica a cualquier columna, pagando un hash por
-- fila. Se compara contra heap, que es la referencia.
SET work_mem = '64MB';

CREATE TABLE hh (a int, b text, n int) USING dbengine_columnar;
INSERT INTO hh SELECT i % 97, 'x' || (i % 53), i FROM generate_series(1, 50000) i;
ANALYZE hh;
CREATE TABLE hhh AS SELECT * FROM hh;

-- El camino serial con hash NO se ofrece: es el único sin un Finalize
-- arriba, así que no puede emitir la misma clave dos veces y por lo tanto no
-- puede volcar cuando se queda sin memoria. Con una estimación de grupos
-- mala eso no degrada, revienta. El que se ofrece es el paralelo, que sí
-- entrega parciales y puede volcar.
--
-- A esta escala el plan normal cuesta menos y gana, con razón; para que el
-- test pruebe el nodo se le sacan las alternativas y se abarata el
-- paralelismo.
SET enable_hashagg = off;
SET enable_sort = off;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

EXPLAIN (COSTS off) SELECT b, count(*) FROM hh GROUP BY b;

-- Checksum independiente del orden: con muchos grupos, comparar
-- md5(string_agg(...)) no es fiable.
SELECT (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hh GROUP BY b) x)
	 = (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hhh GROUP BY b) y) AS hash_count_igual;

SELECT (SELECT sum(hashtext(b || ':' || s || ':' || mn || ':' || mx || ':' || av)::bigint)
		FROM (SELECT b, sum(n) s, min(n) mn, max(n) mx, avg(n) av
			  FROM hh GROUP BY b) x)
	 = (SELECT sum(hashtext(b || ':' || s || ':' || mn || ':' || mx || ':' || av)::bigint)
		FROM (SELECT b, sum(n) s, min(n) mn, max(n) mx, avg(n) av
			  FROM hhh GROUP BY b) y) AS hash_todos_igual;

-- Dos columnas, y con WHERE.
SELECT (SELECT sum(hashtext(a || '/' || b || ':' || c)::bigint)
		FROM (SELECT a, b, count(*) c FROM hh WHERE n % 3 = 0 GROUP BY a, b) x)
	 = (SELECT sum(hashtext(a || '/' || b || ':' || c)::bigint)
		FROM (SELECT a, b, count(*) c FROM hhh WHERE n % 3 = 0 GROUP BY a, b) y)
	AS hash_dos_cols_igual;

-- HAVING: lo evalúa el Finalize, así que el parcial tiene que calcular
-- también los agregados que aparecen solo ahí (sum(n) en el segundo).
EXPLAIN (COSTS off) SELECT b, count(*) FROM hh GROUP BY b HAVING count(*) > 940;
SELECT (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hh GROUP BY b HAVING count(*) > 940) x)
	 = (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hhh GROUP BY b HAVING count(*) > 940) y)
	AS having_igual;
SELECT (SELECT sum(hashtext(a || ':' || c)::bigint)
		FROM (SELECT a, count(*) c FROM hh GROUP BY a
			  HAVING sum(n) > 12900000 AND max(n) > a * 500) x)
	 = (SELECT sum(hashtext(a || ':' || c)::bigint)
		FROM (SELECT a, count(*) c FROM hhh GROUP BY a
			  HAVING sum(n) > 12900000 AND max(n) > a * 500) y)
	AS having_agregado_aparte_igual;
SELECT count(*) FROM hh HAVING sum(n) > 0;
SELECT count(*) FROM hh HAVING sum(n) < 0;

-- length() sobre texto se cuenta sin pasar por pg_mbstrlen: los bytes que no
-- son de continuación en UTF-8.  Con caracteres de 2, 3 y 4 bytes, textos
-- más cortos y más largos que los 8 bytes de a los que se cuenta, vacíos y
-- nulos.
CREATE TABLE ln (a int, t text) USING dbengine_columnar;
INSERT INTO ln SELECT i % 5,
	CASE WHEN i % 13 = 0 THEN NULL
		 ELSE repeat(CASE i % 4 WHEN 0 THEN 'a' WHEN 1 THEN 'ñ'
							   WHEN 2 THEN '日' ELSE '𝄞' END, i % 23)
			  || CASE WHEN i % 3 = 0 THEN 'xyz' ELSE '' END END
	FROM generate_series(1, 20000) i;
ANALYZE ln;
CREATE TABLE lnh AS SELECT * FROM ln;
EXPLAIN (COSTS off) SELECT a, avg(length(t)) FROM ln GROUP BY a;
SELECT (SELECT sum(hashtext(a || ':' || s || ':' || av || ':' || mx)::bigint)
		FROM (SELECT a, sum(length(t)) s, avg(char_length(t)) av,
					 max(character_length(t)) mx FROM ln GROUP BY a) x)
	 = (SELECT sum(hashtext(a || ':' || s || ':' || av || ':' || mx)::bigint)
		FROM (SELECT a, sum(length(t)) s, avg(char_length(t)) av,
					 max(character_length(t)) mx FROM lnh GROUP BY a) y)
	AS length_igual;
DROP TABLE ln, lnh;

-- Agrupar por una columna de texto con diccionario: el string se resuelve una
-- vez por entrada del diccionario y no una por fila. El resultado tiene que
-- ser el mismo; lo que cambia es cuántas búsquedas de hash se hacen.
SELECT (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hh GROUP BY b) x)
	 = (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM hhh GROUP BY b) y) AS dict_igual;

-- Con nulos en la columna de agrupación: el NULL es un grupo más, y en el
-- cache del diccionario ocupa una ranura aparte.
CREATE TABLE dn (b text, n int) USING dbengine_columnar;
INSERT INTO dn SELECT CASE WHEN i % 11 = 0 THEN NULL ELSE 'y' || (i % 31) END, i
	FROM generate_series(1, 30000) i;
ANALYZE dn;
CREATE TABLE dnh AS SELECT * FROM dn;

SELECT (SELECT count(*) FROM (SELECT b FROM dn GROUP BY b) x)
	 = (SELECT count(*) FROM (SELECT b FROM dnh GROUP BY b) y) AS dict_nulos_grupos;
SELECT (SELECT sum(hashtext(coalesce(b, '<nulo>') || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM dn GROUP BY b) x)
	 = (SELECT sum(hashtext(coalesce(b, '<nulo>') || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM dnh GROUP BY b) y) AS dict_nulos_igual;
DROP TABLE dn, dnh;

-- ===========================================================================
-- Volcado por memoria
-- ===========================================================================
-- La tabla de hash tiene un presupuesto de work_mem; al pasarse emite lo
-- acumulado y sigue con la tabla vacía, que es válido porque el nodo entrega
-- parciales y hay un Finalize arriba.
--
-- Con work_mem al mínimo esto vuelca muchas veces, que es lo que hace falta
-- para probarlo. La primera versión de la guarda estaba puesta DESPUÉS de
-- tomar el stripe, así que al cortar lo salteaba entero y perdía filas: sobre
-- ClickBench daba 4947 donde heap daba 4948. Nada en la suite lo detectaba.
-- No se puede provocar bajando work_mem: la guarda del planner y el
-- presupuesto de ejecución miran la MISMA variable, así que con work_mem
-- chico el nodo no se ofrece y el test no prueba nada. Hay que reproducir la
-- situación real, que es una estimación mala: se analiza con pocos valores
-- distintos y después se insertan muchos más sin volver a analizar.
SET work_mem = '1MB';

CREATE TABLE sp (b text, n int) USING dbengine_columnar;
INSERT INTO sp SELECT 'k' || (i % 50), i FROM generate_series(1, 5000) i;
ANALYZE sp;
INSERT INTO sp SELECT 'k' || i, i FROM generate_series(1, 200000) i;
CREATE TABLE sph AS SELECT * FROM sp;

-- El planner sigue creyendo que hay ~50 grupos; hay más de 200.000.
EXPLAIN (COSTS off) SELECT b, count(*) FROM sp GROUP BY b;

-- Sin filas perdidas: el total tiene que dar las 200.000.
SELECT sum(c) AS filas_totales FROM (SELECT b, count(*) c FROM sp GROUP BY b) t;
SELECT count(*) AS filas_en_la_tabla FROM sph;

SELECT (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM sp GROUP BY b) x)
	 = (SELECT sum(hashtext(b || ':' || c)::bigint)
		FROM (SELECT b, count(*) c FROM sph GROUP BY b) y) AS volcado_igual;

SELECT (SELECT sum(hashtext(b || ':' || s || ':' || mx)::bigint)
		FROM (SELECT b, sum(n) s, max(n) mx FROM sp GROUP BY b) x)
	 = (SELECT sum(hashtext(b || ':' || s || ':' || mx)::bigint)
		FROM (SELECT b, sum(n) s, max(n) mx FROM sph GROUP BY b) y)
	AS volcado_sum_max_igual;

DROP TABLE sp, sph;
RESET work_mem;

-- ===========================================================================
-- count(DISTINCT)
-- ===========================================================================
-- Se cuenta con un conjunto de hash sobre la columna, sin pasar por la
-- función de transición. Los nulos no cuentan, igual que count().
EXPLAIN (COSTS off) SELECT count(DISTINCT b) FROM hh;

SELECT (SELECT count(DISTINCT b) FROM hh) = (SELECT count(DISTINCT b) FROM hhh)
	AS distinct_texto_igual;
SELECT (SELECT count(DISTINCT a) FROM hh) = (SELECT count(DISTINCT a) FROM hhh)
	AS distinct_int_igual;
SELECT (SELECT count(DISTINCT n) FROM hh WHERE n % 7 = 0)
	 = (SELECT count(DISTINCT n) FROM hhh WHERE n % 7 = 0) AS distinct_where_igual;

CREATE TABLE nulos (x int) USING dbengine_columnar;
INSERT INTO nulos SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i % 10 END
	FROM generate_series(1, 1000) i;
CREATE TABLE nulosh AS SELECT * FROM nulos;
SELECT (SELECT count(DISTINCT x) FROM nulos) = (SELECT count(DISTINCT x) FROM nulosh)
	AS distinct_con_nulos_igual;

-- Con GROUP BY no se empuja: haría falta un conjunto por grupo.  Queda el
-- plan normal, con el scan descartando las filas repetidas por stripe.
RESET enable_hashagg;
RESET enable_sort;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
EXPLAIN (COSTS off) SELECT a, count(DISTINCT b) FROM hh GROUP BY a;

DROP TABLE hh, hhh, nulos, nulosh;

-- ===========================================================================
-- Tabla de hash propia (flathash.c)
-- ===========================================================================
-- Claves de ancho fijo y count, sum, avg, min y max de enteros: un agregado
-- completo y serial que vuelca a disco por su cuenta.  Todo contra heap, con
-- nulos en claves y argumentos, y un checksum independiente del orden.
SET dbengine.enable_flat_hash = on;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE fx (a int2, b int4, c int8, d date, t timestamp, f bool,
				 v int4, w int2, s text) USING dbengine_columnar;
INSERT INTO fx SELECT
	CASE WHEN i % 17 = 0 THEN NULL ELSE i % 5 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE i % 300 END,
	(i % 7) * 10000000000,
	date '2020-01-01' + (i % 40),
	timestamp '2020-01-01' + (i % 11) * interval '1 hour',
	CASE WHEN i % 19 = 0 THEN NULL ELSE i % 2 = 0 END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE i * 37 % 1000 - 500 END,
	(i % 100)::int2,
	CASE WHEN i % 29 = 0 THEN NULL ELSE 'x' || i % 3 END
	FROM generate_series(1, 30000) i;
ANALYZE fx;
CREATE TABLE fxh AS SELECT * FROM fx;

EXPLAIN (COSTS OFF)
SELECT a, b, count(*), sum(v), avg(w) FROM fx GROUP BY a, b;

CREATE FUNCTION fx_check(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	x bigint;
	y bigint;
BEGIN
	EXECUTE format('SELECT sum(hashtext(r::text)::bigint) FROM (%s) r', q) INTO x;
	EXECUTE format('SELECT sum(hashtext(r::text)::bigint) FROM (%s) r',
				   replace(q, 'FROM fx', 'FROM fxh')) INTO y;
	RETURN x IS NOT DISTINCT FROM y;
END $$;

SELECT fx_check('SELECT a, b, count(*), count(v), count(s), sum(v), sum(w), avg(v), avg(w), min(v), max(w) FROM fx GROUP BY a, b') AS dos_claves;
SELECT fx_check('SELECT c, d, min(c), max(d), min(t), max(t), count(*) FROM fx GROUP BY c, d') AS int8_fecha;
SELECT fx_check('SELECT t, f, count(*), sum(v), avg(v) FROM fx GROUP BY t, f') AS timestamp_bool;
SELECT fx_check('SELECT b, count(*), avg(w) FROM fx WHERE s = ''x1'' AND v > 0 GROUP BY b') AS con_where;
-- Un grupo sin ningún valor no nulo: sum, avg, min y max dan NULL, count 0.
SELECT fx_check('SELECT a, count(v), sum(v), avg(v), min(v), max(v) FROM fx WHERE v IS NULL GROUP BY a') AS todo_nulo;
SELECT a, count(v), sum(v), avg(v), min(v), max(v) FROM fx WHERE v IS NULL GROUP BY a ORDER BY a;

-- Una columna que los stripes viejos no tienen: vale el DEFAULT, como
-- clave y como argumento.
ALTER TABLE fx ADD COLUMN k int4 DEFAULT 7;
ALTER TABLE fxh ADD COLUMN k int4 DEFAULT 7;
INSERT INTO fx (a, b, k, v) SELECT 1, i % 3, i % 4, i FROM generate_series(1, 100) i;
INSERT INTO fxh (a, b, k, v) SELECT 1, i % 3, i % 4, i FROM generate_series(1, 100) i;
SELECT fx_check('SELECT k, count(*), sum(k), min(k) FROM fx GROUP BY k') AS columna_nueva;
SELECT k, count(*) FROM fx GROUP BY k ORDER BY k;

-- Volcado: con work_mem al mínimo y una estimación mala (se analiza con
-- pocos grupos y se insertan muchos más), varios niveles de particiones.
CREATE TABLE fxs (g int8, h int4, v int4) USING dbengine_columnar;
INSERT INTO fxs SELECT i % 10, i % 3, i FROM generate_series(1, 2000) i;
ANALYZE fxs;
INSERT INTO fxs SELECT i, i % 3, i % 1000 FROM generate_series(1, 200000) i;
INSERT INTO fxs SELECT i * 7, NULL, NULL FROM generate_series(1, 50000) i;
CREATE TABLE fxsh AS SELECT * FROM fxs;
SET work_mem = '64kB';
EXPLAIN (COSTS OFF) SELECT g, h, count(*), sum(v) FROM fxs GROUP BY g, h;
SELECT count(*) AS grupos, sum(c) AS filas
FROM (SELECT g, h, count(*) c, sum(v) FROM fxs GROUP BY g, h) x;
SELECT count(*) AS grupos, sum(c) AS filas
FROM (SELECT g, h, count(*) c, sum(v) FROM fxsh GROUP BY g, h) x;
SELECT (SELECT sum(hashtext(g || ':' || coalesce(h, -1) || ':' || c || ':' || coalesce(s, -1) || ':' || coalesce(a::text, '-'))::bigint)
		FROM (SELECT g, h, count(*) c, sum(v) s, avg(v) a FROM fxs GROUP BY g, h) x)
	 = (SELECT sum(hashtext(g || ':' || coalesce(h, -1) || ':' || c || ':' || coalesce(s, -1) || ':' || coalesce(a::text, '-'))::bigint)
		FROM (SELECT g, h, count(*) c, sum(v) s, avg(v) a FROM fxsh GROUP BY g, h) y)
	AS volcado_igual;
RESET work_mem;

-- Rescan: del lado de adentro de un nested loop, la tabla se vacía y se
-- llena de nuevo por cada fila de afuera.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT k, (SELECT count(*) FROM (SELECT b, count(*) FROM fx WHERE a = k GROUP BY b) s)
FROM generate_series(0, 4) k ORDER BY k;
SELECT k, (SELECT count(*) FROM (SELECT b, count(*) FROM fxh WHERE a = k GROUP BY b) s)
FROM generate_series(0, 4) k ORDER BY k;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- Claves de texto: una sola, con diccionario, se busca una vez por código y
-- stripe; con work_mem al mínimo los grupos que no entran se vuelcan como un
-- estado parcial por código (registros de estados) mezclados con filas
-- sueltas de los stripes sin diccionario.  Con nulos, multibyte y varchar.
CREATE TABLE ft (u text, v varchar(20), n int4) USING dbengine_columnar;
INSERT INTO ft SELECT 'u' || (i % 50), 'v' || (i % 7), i FROM generate_series(1, 2000) i;
ANALYZE ft;
INSERT INTO ft SELECT CASE WHEN i % 97 = 0 THEN NULL
						   ELSE 'url/' || (i % 30000) || CASE WHEN i % 5 = 0 THEN 'ñ日' ELSE '' END END,
	'v' || (i % 7), i % 1000
	FROM generate_series(1, 150000) i;
INSERT INTO ft SELECT md5(i::text), NULL, i FROM generate_series(1, 20000) i;
CREATE TABLE fth AS SELECT * FROM ft;
SET work_mem = '64kB';
EXPLAIN (COSTS OFF) SELECT u, count(*), sum(n) FROM ft GROUP BY u;
SELECT (SELECT sum(hashtext(coalesce(u, '-') || ':' || c || ':' || s || ':' || a)::bigint)
		FROM (SELECT u, count(*) c, sum(n) s, avg(n) a FROM ft GROUP BY u) x)
	 = (SELECT sum(hashtext(coalesce(u, '-') || ':' || c || ':' || s || ':' || a)::bigint)
		FROM (SELECT u, count(*) c, sum(n) s, avg(n) a FROM fth GROUP BY u) y)
	AS texto_volcado_igual;
SELECT (SELECT count(*) FROM (SELECT u FROM ft GROUP BY u) x)
	 = (SELECT count(*) FROM (SELECT u FROM fth GROUP BY u) y) AS texto_grupos;
SELECT fx_check('SELECT v, count(*), min(n), max(n) FROM ft GROUP BY v') AS varchar_igual;
-- Una constante en el SELECT sale tal cual (q35: SELECT 1, URL, count(*)).
EXPLAIN (COSTS OFF) SELECT 1, u, count(*) FROM ft GROUP BY 1, u;
SELECT (SELECT sum(hashtext(k || ':' || coalesce(u, '-') || ':' || c)::bigint)
		FROM (SELECT 1 AS k, u, count(*) c FROM ft GROUP BY 1, u) x)
	 = (SELECT sum(hashtext(k || ':' || coalesce(u, '-') || ':' || c)::bigint)
		FROM (SELECT 1 AS k, u, count(*) c FROM fth GROUP BY 1, u) y)
	AS constante_igual;
-- Un LIMIT corta con particiones sin procesar: se tienen que cerrar al
-- terminar el nodo, sin WARNING de archivos abiertos.
SELECT count(*) FROM (SELECT u, count(*) FROM ft GROUP BY u LIMIT 5) x;
RESET work_mem;
-- Texto junto a otra clave: sin caché por código, al plan de siempre.
EXPLAIN (COSTS OFF) SELECT u, n, count(*) FROM ft GROUP BY u, n;
DROP TABLE ft, fth;

-- En paralelo: los participantes se reparten baldes de grupos por el hash de
-- la clave, cada uno recorre la tabla entera y emite grupos completos.  El
-- Gather solo concatena.  Con volcado, con WHERE, con texto, y con menos
-- participantes de los planeados: sin workers el líder toma todos los
-- baldes, y sin el líder los toman los workers.
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
-- Con la estimación buena (250K grupos en 252K filas) el agregado parcial
-- no reduce y no se ofrece: queda la tabla propia, serial o repartida.
ANALYZE fxs;
CREATE TABLE fpt (k text, v int4) USING dbengine_columnar;
INSERT INTO fpt SELECT 'k' || (i % 40000) || CASE WHEN i % 3 = 0 THEN 'ñ' ELSE '' END,
	i % 100 FROM generate_series(1, 120000) i;
ANALYZE fpt;
CREATE TABLE fpth AS SELECT * FROM fpt;
SET work_mem = '64kB';
EXPLAIN (COSTS OFF) SELECT g, h, count(*), sum(v) FROM fxs GROUP BY g, h;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(v) FROM fpt GROUP BY k;
SELECT (SELECT sum(hashtext(g || ':' || coalesce(h, -1) || ':' || c || ':' || coalesce(s, -1) || ':' || coalesce(a::text, '-'))::bigint)
		FROM (SELECT g, h, count(*) c, sum(v) s, avg(v) a FROM fxs GROUP BY g, h) x)
	 = (SELECT sum(hashtext(g || ':' || coalesce(h, -1) || ':' || c || ':' || coalesce(s, -1) || ':' || coalesce(a::text, '-'))::bigint)
		FROM (SELECT g, h, count(*) c, sum(v) s, avg(v) a FROM fxsh GROUP BY g, h) y)
	AS paralelo_volcado_igual;
SELECT fx_check('SELECT b, count(*), avg(w) FROM fx WHERE s = ''x1'' AND v > 0 GROUP BY b') AS paralelo_where;
SELECT (SELECT sum(hashtext(k || ':' || c || ':' || s)::bigint)
		FROM (SELECT k, count(*) c, sum(v) s FROM fpt GROUP BY k) x)
	 = (SELECT sum(hashtext(k || ':' || c || ':' || s)::bigint)
		FROM (SELECT k, count(*) c, sum(v) s FROM fpth GROUP BY k) y)
	AS paralelo_texto;
SET max_parallel_workers = 0;
SELECT count(*) AS grupos, sum(c) AS filas
FROM (SELECT g, h, count(*) c FROM fxs GROUP BY g, h) x;
RESET max_parallel_workers;
SET parallel_leader_participation = off;
SELECT count(*) AS grupos, sum(c) AS filas
FROM (SELECT g, h, count(*) c FROM fxs GROUP BY g, h) x;
RESET parallel_leader_participation;
RESET work_mem;
DROP TABLE fpt, fpth;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

-- Lo que no entra: un float como clave (0 y -0 son iguales), sum(int8) y un
-- HAVING, que la tabla no puede dejarle a un Finalize.
EXPLAIN (COSTS OFF) SELECT v::float8, count(*) FROM fx GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT a, sum(c) FROM fx GROUP BY a;
EXPLAIN (COSTS OFF) SELECT a, count(*) FROM fx GROUP BY a HAVING count(*) > 1;

-- Apagada, el plan de antes.
SET dbengine.enable_flat_hash = off;
EXPLAIN (COSTS OFF) SELECT a, b, count(*), sum(v), avg(w) FROM fx GROUP BY a, b;

DROP FUNCTION fx_check(text);
DROP TABLE fx, fxh, fxs, fxsh;
RESET max_parallel_workers_per_gather;
DROP TABLE f, fh;
DROP TABLE g, gh;
DROP EXTENSION dbengine;
