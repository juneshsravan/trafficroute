/*
 * graph_io.h — loader for the documented CSV test-network format.
 *
 * nodes.csv:  id,lat,lon,name
 * edges.csv:  id,from_id,to_id,distance_m,road_class,speed_kmh,oneway,toll,restricted[,name]
 *   - distance_m: metres; if empty or 0 it is computed by haversine.
 *   - speed_kmh: if empty or 0, the road-class default speed is used.
 *   - oneway=1 -> one directed edge from->to; oneway=0 -> two directed edges
 *     (the reverse edge gets external id = -id).
 * Lines starting with '#' and the header line are ignored.
 *
 * This format is for small, hand-checkable networks (Phase 1–3). Real data
 * comes from the OSM importer (Phase 4).
 */
#ifndef IRA_GRAPH_IO_H
#define IRA_GRAPH_IO_H

#include <stdio.h>
#include "graph.h"

typedef struct {
    size_t lines_read;
    size_t nodes_loaded;
    size_t edges_loaded;        /* directed edges */
    size_t distances_clamped;   /* given distance < straight-line, raised */
    char   error[256];          /* human-readable message on failure */
} graph_load_report_t;

/* Loads and finalizes a graph. Returns NULL on error (see report->error). */
graph_t *graph_load_csv(const char *nodes_path, const char *edges_path,
                        graph_load_report_t *report);

#endif
