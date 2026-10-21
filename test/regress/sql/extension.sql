-- F0: la extensión se crea, se identifica y se puede eliminar limpiamente.

CREATE EXTENSION dbengine;

SELECT dbengine.version();

-- dbengine.preloaded() tiene que coincidir con la realidad del cluster,
-- corra installcheck con o sin shared_preload_libraries.
SELECT dbengine.preloaded()
	 = (current_setting('shared_preload_libraries') LIKE '%dbengine%')
	AS preload_coherente;

-- Las GUCs quedan definidas y con sus valores por defecto.
SELECT name, setting, unit, context
FROM pg_settings
WHERE name LIKE 'dbengine.%'
ORDER BY name;

-- El prefijo está reservado: una GUC inventada bajo dbengine.* falla.
DO $$
BEGIN
	PERFORM set_config('dbengine.no_existe', 'x', false);
	RAISE EXCEPTION 'se esperaba un error: el prefijo dbengine no está reservado';
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'prefijo dbengine reservado (SQLSTATE %)', SQLSTATE;
END
$$;

-- Objetos que instala la extensión.
SELECT p.proname, pg_get_function_result(p.oid) AS returns
FROM pg_depend d
JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'dbengine'
JOIN pg_proc p ON p.oid = d.objid
WHERE d.classid = 'pg_proc'::regclass
ORDER BY p.proname;

SELECT c.relname FROM pg_depend d
JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'dbengine'
JOIN pg_class c ON c.oid = d.objid
WHERE d.classid = 'pg_class'::regclass AND c.relkind = 'r'
ORDER BY c.relname;

SELECT amname, amtype FROM pg_am WHERE amname LIKE 'dbengine%' ORDER BY amname;

DROP EXTENSION dbengine;

-- No queda nada colgado: ni el esquema, ni el access method.
SELECT count(*) AS esquemas_huerfanos
FROM pg_namespace WHERE nspname = 'dbengine';
SELECT count(*) AS ams_huerfanos
FROM pg_am WHERE amname LIKE 'dbengine%';
