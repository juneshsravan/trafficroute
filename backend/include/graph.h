/*
 * graph.h — directed, weighted road graph in compressed sparse row (CSR) form.
 *
 * Life cycle:
 *   graph_create()                -> empty, mutable "builder" state
 *   graph_add_node()/add_edge*()  -> append records
 *   graph_finalize()              -> sorts edges by source node and builds the
 *                                    first_out[] offset array; topology frozen
 *   graph_set_traffic_time()/graph_set_closed() -> runtime weight/flag changes
 *   graph_free()
 *
 * Why CSR instead of linked adjacency lists: all outgoing edges of a node sit
 * in one contiguous slice edges[first_out[v] .. first_out[v+1]), which is
 * cache friendly, needs no per-edge pointers, and is written to graph.bin
 * unchanged. An adjacency matrix is never used: it would need V*V cells.
 *
 * Road geometry: an edge runs between two graph nodes (junctions). The
 * intermediate OSM points that only describe the road's shape are stored in a
 * shared shape[] array (edge.geom_offset, edge.geom_count) instead of being
 * graph nodes, which keeps the routing graph much smaller.
 *
 * Units: distances in METRES, times in SECONDS, speeds in KM/H only at the
 * input boundary (converted with km/h / 3.6 = m/s). Per-edge values are
 * stored as float (4 bytes) to halve memory on large networks; path totals
 * are always accumulated in double.
 */
#ifndef IRA_GRAPH_H
#define IRA_GRAPH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t node_idx_t;   /* dense 0..node_count-1 index */
typedef uint32_t edge_idx_t;   /* dense 0..edge_count-1 index (after finalize) */
#define IRA_INVALID_IDX UINT32_MAX

/* Edge flag bits */
#define EDGE_FLAG_TOLL        (1u << 0)
#define EDGE_FLAG_RESTRICTED  (1u << 1)  /* access=destination/delivery/permit... */
#define EDGE_FLAG_CLOSED      (1u << 2)  /* closure / incident: never traversable */

/* Node flag bits */
#define NODE_FLAG_MAIN_COMPONENT (1u << 0)  /* in the largest strongly connected component */

typedef enum {
    ROAD_MOTORWAY = 0,
    ROAD_TRUNK,
    ROAD_PRIMARY,
    ROAD_SECONDARY,
    ROAD_TERTIARY,
    ROAD_UNCLASSIFIED,
    ROAD_RESIDENTIAL,
    ROAD_SERVICE,
    ROAD_TRACK,
    ROAD_CLASS_COUNT
} road_class_t;

typedef struct {
    int64_t id;          /* external/source id (e.g. CSV id or OSM node id) */
    double  lat;         /* WGS84 degrees */
    double  lon;
} graph_node_t;

typedef struct {
    float lat;
    float lon;
} graph_point_t;

typedef struct {
    int64_t    id;              /* external edge id (reverse twins use -id) */
    node_idx_t from;
    node_idx_t to;
    float      distance_m;      /* > 0, never shorter than straight line */
    float      base_time_s;     /* baseline travel time, > 0 */
    float      traffic_time_s;  /* <= 0 means "no traffic override" */
    uint32_t   name_id;         /* offset into names blob; 0 = unnamed */
    uint32_t   geom_offset;     /* first intermediate point in shape[] */
    uint32_t   geom_count;      /* number of intermediate points */
    uint8_t    speed_limit_kmh; /* 0 = unknown */
    uint8_t    road_class;      /* road_class_t */
    uint8_t    flags;           /* EDGE_FLAG_* */
    uint8_t    reserved;
} graph_edge_t;

typedef struct {
    graph_node_t  *nodes;
    uint8_t       *node_flags;   /* NODE_FLAG_*, one per node */
    size_t         node_count;
    size_t         node_cap;

    graph_edge_t  *edges;
    size_t         edge_count;
    size_t         edge_cap;

    graph_point_t *shape;        /* intermediate geometry points */
    size_t         shape_count;
    size_t         shape_cap;

    char          *names;        /* blob of NUL-terminated strings; names[0]='\0' */
    size_t         names_size;
    size_t         names_cap;
    void          *name_index;   /* build-time intern table (freed at finalize) */

    edge_idx_t    *first_out;    /* node_count + 1 offsets, valid after finalize */
    bool           finalized;

    /* Fastest effective speed over all edges (m/s). Used by A* time heuristic.
     * Must be an upper bound so the heuristic never overestimates. */
    double         max_speed_mps;

    /* Dataset metadata (filled by loaders) */
    char           dataset[128];
    int64_t        dataset_time;  /* unix seconds; 0 = unknown */
    double         min_lat, min_lon, max_lat, max_lon;   /* bbox after finalize */
} graph_t;

typedef enum {
    GRAPH_OK = 0,
    GRAPH_ERR_NOMEM,
    GRAPH_ERR_INVALID,     /* bad argument / value */
    GRAPH_ERR_FINALIZED,   /* mutation after finalize */
    GRAPH_ERR_NOT_FINALIZED,
    GRAPH_ERR_DUPLICATE,
    GRAPH_ERR_NOT_FOUND,
    GRAPH_ERR_OVERFLOW,
    GRAPH_ERR_IO,
    GRAPH_ERR_FORMAT
} graph_status_t;

const char *graph_status_str(graph_status_t s);

graph_t *graph_create(void);
void     graph_free(graph_t *g);

/* Appends a node; returns its dense index via out_idx. Coordinates validated. */
graph_status_t graph_add_node(graph_t *g, int64_t id, double lat, double lon,
                              node_idx_t *out_idx);

/* Appends one DIRECTED edge between dense node indices. A two-way road must be
 * added as two edges. distance_m and base_time_s must be finite and > 0. */
graph_status_t graph_add_edge(graph_t *g, int64_t id, node_idx_t from,
                              node_idx_t to, double distance_m,
                              double base_time_s, road_class_t road_class,
                              double speed_limit_kmh, uint32_t flags);

/* Extended form with road name and intermediate shape points (may be NULL/0). */
graph_status_t graph_add_edge_ex(graph_t *g, int64_t id, node_idx_t from,
                                 node_idx_t to, double distance_m,
                                 double base_time_s, road_class_t road_class,
                                 double speed_limit_kmh, uint32_t flags,
                                 const char *name, const graph_point_t *shape,
                                 uint32_t shape_count);

/* Builds the CSR index. After this, edge indices refer to the sorted order. */
graph_status_t graph_finalize(graph_t *g);

/* Linear lookups by external id (tests / admin tools only, O(n)). */
node_idx_t graph_find_node(const graph_t *g, int64_t id);
edge_idx_t graph_find_edge(const graph_t *g, int64_t id);

/* Reverse twin of a two-way road (edge to->from with same length), or
 * IRA_INVALID_IDX for one-way roads. */
edge_idx_t graph_find_reverse(const graph_t *g, edge_idx_t e);

/* Effective travel time used by routing: traffic override if set, else base. */
double graph_edge_time(const graph_edge_t *e);
const char *graph_edge_name(const graph_t *g, const graph_edge_t *e);

/* Runtime weight updates (topology unchanged). time_s <= 0 clears override. */
graph_status_t graph_set_traffic_time(graph_t *g, edge_idx_t e, double time_s);
graph_status_t graph_set_closed(graph_t *g, edge_idx_t e, bool closed);
void           graph_clear_traffic(graph_t *g);
void           graph_recompute_max_speed(graph_t *g);

/* Approximate heap memory used by the graph, in bytes. */
size_t graph_memory_bytes(const graph_t *g);

/* Iteration helpers */
static inline edge_idx_t graph_out_begin(const graph_t *g, node_idx_t v) {
    return g->first_out[v];
}
static inline edge_idx_t graph_out_end(const graph_t *g, node_idx_t v) {
    return g->first_out[v + 1];
}

/* Default speed (km/h) for a road class, used when no speed is supplied. */
double road_class_default_speed_kmh(road_class_t c);
const char *road_class_name(road_class_t c);
bool road_class_from_name(const char *name, road_class_t *out);

#endif /* IRA_GRAPH_H */
