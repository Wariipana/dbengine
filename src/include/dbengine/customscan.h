/*-------------------------------------------------------------------------
 *
 * customscan.h
 *	  Custom Scan vectorizado sobre el storage columnar (F3).
 *
 * Es el único lugar del que se pueden conseguir dos cosas que el Table AM
 * no da (R4 y ADR-003):
 *
 *   - qué columnas pide la query, para no decodificar las demás;
 *   - qué predicados tiene, para descartar stripes con los zone maps.
 *
 * El seq scan del Table AM sigue existiendo y sigue siendo correcto: es el
 * camino al que se cae cuando esto no aplica.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBENGINE_CUSTOMSCAN_H
#define DBENGINE_CUSTOMSCAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

extern void dbe_customscan_init(void);
extern void dbe_vectoragg_init(void);

/*
 * El mismo scan que `path` —el Custom Scan, serial o paralelo, o una
 * proyección sobre él— descartando las filas repetidas dentro de cada
 * stripe; `ratio` es la fracción de filas que se estima que sobreviven.
 * NULL si `path` no es un Custom Scan de dbengine o si sus columnas no se
 * pueden deduplicar.  Ver rowdedup.h.
 */
extern Path *dbe_scan_dedup_path(PlannerInfo *root, Path *path, double ratio);

#endif							/* DBENGINE_CUSTOMSCAN_H */
