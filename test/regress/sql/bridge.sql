-- F0 / R2: la frontera C <-> C++.
--
-- Dos invariantes:
--   1. Ninguna excepción de C++ escapa a un frame de C.
--   2. Ningún longjmp de ereport atraviesa un frame de C++ sin correr sus
--      destructores.
--
-- El contador de destructores de cada caso es la evidencia de (2): si el
-- longjmp cruzara, quedaría en 0.

CREATE EXTENSION dbengine;

-- Todos los casos tienen que pasar.
SELECT test_name, passed FROM dbengine.bridge_selftest() ORDER BY test_name;

-- Y ninguno puede fallar.  Esta es la aserción dura de la fase.
SELECT count(*) AS fallidos
FROM dbengine.bridge_selftest()
WHERE NOT passed;

-- Un error nacido en C++ llega a SQL como un error normal de Postgres,
-- con mensaje legible...
\set ON_ERROR_STOP off
SELECT dbengine.raise_from_engine(1);   -- std::runtime_error
SELECT dbengine.raise_from_engine(2);   -- std::bad_alloc
SELECT dbengine.raise_from_engine(3);   -- excepción no estándar
SELECT dbengine.raise_from_engine(99);  -- kind inválido
\set ON_ERROR_STOP on

-- ...y con el SQLSTATE que le corresponde a cada clase de error.
DO $$
DECLARE
	k int;
BEGIN
	FOREACH k IN ARRAY ARRAY[1, 2, 3, 99] LOOP
		BEGIN
			PERFORM dbengine.raise_from_engine(k);
			RAISE NOTICE 'kind=% no lanzó error (se esperaba uno)', k;
		EXCEPTION WHEN OTHERS THEN
			RAISE NOTICE 'kind=% -> SQLSTATE %', k, SQLSTATE;
		END;
	END LOOP;
END
$$;

-- El camino sin error devuelve el contador de destructores, no un error.
SELECT dbengine.raise_from_engine(0) AS destructores;

-- Un error del motor aborta la transacción como cualquier otro error, y la
-- sesión queda usable después del rollback.  Si el airlock dejara estado
-- corrupto, esto es lo que lo delataría.
BEGIN;
\set ON_ERROR_STOP off
SELECT dbengine.raise_from_engine(1);
SELECT dbengine.version();  -- transacción abortada: debe fallar con 25P02
\set ON_ERROR_STOP on
ROLLBACK;

SELECT dbengine.version() AS motor_vivo_despues_del_rollback;

-- Y un error atrapado en una subtransacción no rompe la transacción externa.
BEGIN;
DO $$
BEGIN
	PERFORM dbengine.raise_from_engine(1);
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'subtransacción abortada limpiamente';
END
$$;
SELECT dbengine.version() AS motor_vivo_en_la_misma_transaccion;
COMMIT;

-- La batería completa vuelve a pasar después de todo lo anterior.
SELECT count(*) AS fallidos_al_final
FROM dbengine.bridge_selftest()
WHERE NOT passed;

DROP EXTENSION dbengine;
