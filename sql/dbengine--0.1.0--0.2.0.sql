-- dbengine 0.1.0 -> 0.2.0
\echo Use "ALTER EXTENSION dbengine UPDATE TO '0.2.0'" to load this file. \quit

-- Agregados precalculados por segmento (SMA).  Los stripes escritos antes
-- quedan con NULL y se siguen leyendo fila por fila: no hace falta
-- reescribir nada.
ALTER TABLE stripe_column ADD COLUMN sum_value numeric;

COMMENT ON COLUMN stripe_column.sum_value IS
	'Suma exacta de los valores no nulos del segmento, para contestar '
	'sum/avg/count sin leer datos. Solo para smallint, integer y bigint; '
	'NULL en los demás tipos y en los stripes escritos antes de 0.2.0.';
