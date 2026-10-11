/*
 * spatial_index.h — uniform lat/lon grid for nearest-node and nearest-edge
 * lookups ("snap a clicked point to the road network").
 *
 * A grid (rather than a k-d tree) because road nodes are fairly evenly spread
 * over populated land, the build is a single counting sort, and a lookup only
 * touches a handful of neighbouring cells. Cell size grows automatically so
 * the cell table stays bounded for a country-sized bbox.
 */
#ifndef IRA_SPATIAL_INDEX_H
#define IRA_SPATIAL_INDEX_H

#include "graph.h"

typedef struct spatial_grid spatial_grid_t;

typedef struct {
    spatial_grid_t *nodes;   /* graph nodes with at least one edge */
    spatial_grid_t *edges;   /* one entry per edge vertex (ends + shape points) */
    double          max_seg_m; /* longest distance between consecutive edge vertices */
} spatial_index_t;

spatial_index_t *spatial_index_build(const graph_t *g);
void             spatial_index_free(spatial_index_t *si);
size_t           spatial_index_memory_bytes(const spatial_index_t *si);

/* Nearest routable node within max_m metres. If main_only, nodes outside the
 * largest strongly connected component are ignored first (falls back to any
 * node if none qualifies). Returns false if nothing is within max_m. */
bool spatial_snap_node(const spatial_index_t *si, const graph_t *g,
                       double lat, double lon, double max_m,
                       node_idx_t *out, double *out_dist_m);

/* Nearest edge (by distance to its polyline) within max_m metres. */
bool spatial_nearest_edge(const spatial_index_t *si, const graph_t *g,
                          double lat, double lon, double max_m,
                          edge_idx_t *out, double *out_dist_m);

/* Distance in metres from a point to an edge's full polyline. */
double spatial_point_edge_distance(const graph_t *g, edge_idx_t e,
                                   double lat, double lon);

/* Calls fn for each edge with any vertex inside the bbox (may repeat edges).
 * Stops early if fn returns false. */
void spatial_edges_in_bbox(const spatial_index_t *si, double min_lat, double min_lon,
                           double max_lat, double max_lon,
                           bool (*fn)(void *ctx, edge_idx_t e), void *ctx);

#endif
