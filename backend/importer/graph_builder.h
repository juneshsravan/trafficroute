/*
 * graph_builder.h — OSM file -> routing graph + place list.
 *
 * Two streaming passes with readosm (works for .osm.pbf and .osm XML):
 *   pass 1: keep routable ways (node refs + attributes) and place nodes;
 *           then sort/unique all referenced node ids and count how many
 *           times each is used (a node used twice or more is a junction);
 *   pass 2: read coordinates of only the referenced nodes
 *           (binary search in the sorted id array, no hash map).
 * Then every way is split at junctions (and optionally into pieces no
 * longer than max_edge_m); each piece becomes one edge (two for two-way
 * roads) whose intermediate points become shape geometry.
 */
#ifndef IRA_GRAPH_BUILDER_H
#define IRA_GRAPH_BUILDER_H

#include "graph.h"
#include "osm_parser.h"

typedef struct {
    osm_rules_t rules;
    double      max_edge_m;      /* split long segments (default 1000, 0 = off) */
    bool        early_stop;      /* stop pass 2 at first way (sorted PBF) */
    bool        verbose;
} import_options_t;

typedef struct {
    int64_t osm_id;
    double  lat, lon;
    int     rank;
    char    type[24];
    char    name[160];
    char    name_en[160];
    char    state[96];
    char    district[96];
    char    near[160];
    int64_t population;
} import_place_t;

typedef struct {
    /* pass 1 */
    size_t nodes_seen, ways_seen, ways_routable;
    size_t ways_dropped[WAY_REASON_COUNT];
    size_t way_refs;
    size_t referenced_nodes;
    /* pass 2 */
    size_t coords_found;
    size_t refs_missing;          /* refs to nodes not present in the extract */
    /* build */
    size_t segments;
    size_t oneway_segments;
    size_t self_loops_skipped;
    size_t zero_length_skipped;
    double road_km;               /* undirected */
    double seconds_pass1, seconds_pass2, seconds_build;
    char   error[256];
} import_stats_t;

typedef struct {
    graph_t        *graph;
    import_place_t *places;
    size_t          place_count;
} import_result_t;

void import_default_options(import_options_t *o);
bool import_osm(const char *path, const import_options_t *opt, import_result_t *out,
                import_stats_t *stats);
void import_result_free(import_result_t *r);

#endif
