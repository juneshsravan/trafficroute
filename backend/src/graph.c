/*
 * graph.c — CSR road graph implementation. See graph.h for the design.
 */
#include "graph.h"
#include "geospatial.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 16

const char *graph_status_str(graph_status_t s)
{
    switch (s) {
    case GRAPH_OK:                return "ok";
    case GRAPH_ERR_NOMEM:         return "out of memory";
    case GRAPH_ERR_INVALID:       return "invalid argument";
    case GRAPH_ERR_FINALIZED:     return "graph already finalized";
    case GRAPH_ERR_NOT_FINALIZED: return "graph not finalized";
    case GRAPH_ERR_DUPLICATE:     return "duplicate id";
    case GRAPH_ERR_NOT_FOUND:     return "not found";
    case GRAPH_ERR_OVERFLOW:      return "size overflow";
    case GRAPH_ERR_IO:            return "I/O error";
    case GRAPH_ERR_FORMAT:        return "invalid file format";
    }
    return "unknown";
}

/* ---------------- name interning (build time only) ----------------
 * Open-addressing hash set of offsets into g->names, so each distinct road
 * name is stored once no matter how many edges use it. */
typedef struct {
    uint32_t *slots;     /* name offsets; 0 = empty (offset 0 is "") */
    size_t    cap;       /* power of two */
    size_t    used;
} name_index_t;

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

graph_t *graph_create(void)
{
    graph_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->names = malloc(INITIAL_CAP);
    if (!g->names) { free(g); return NULL; }
    g->names[0] = '\0';
    g->names_size = 1;
    g->names_cap = INITIAL_CAP;
    return g;
}

static void name_index_free(graph_t *g)
{
    name_index_t *ni = g->name_index;
    if (ni) { free(ni->slots); free(ni); }
    g->name_index = NULL;
}

void graph_free(graph_t *g)
{
    if (!g) return;
    free(g->nodes);
    free(g->node_flags);
    free(g->edges);
    free(g->shape);
    free(g->names);
    free(g->first_out);
    name_index_free(g);
    free(g);
}

/* Grows *buf so it can hold at least need elements of elem_size bytes.
 * Guards against size_t overflow in the multiplication. */
static graph_status_t grow(void **buf, size_t *cap, size_t need, size_t elem_size)
{
    if (need <= *cap) return GRAPH_OK;
    size_t new_cap = *cap ? *cap : INITIAL_CAP;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) return GRAPH_ERR_OVERFLOW;
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / elem_size) return GRAPH_ERR_OVERFLOW;
    void *p = realloc(*buf, new_cap * elem_size);
    if (!p) return GRAPH_ERR_NOMEM;
    *buf = p;
    *cap = new_cap;
    return GRAPH_OK;
}

static graph_status_t name_index_grow(graph_t *g, name_index_t *ni)
{
    size_t ncap = ni->cap ? ni->cap * 2 : 1024;
    uint32_t *slots = calloc(ncap, sizeof *slots);
    if (!slots) return GRAPH_ERR_NOMEM;
    for (size_t i = 0; i < ni->cap; i++) {
        uint32_t off = ni->slots[i];
        if (!off) continue;
        size_t j = (size_t)fnv1a(g->names + off) & (ncap - 1);
        while (slots[j]) j = (j + 1) & (ncap - 1);
        slots[j] = off;
    }
    free(ni->slots);
    ni->slots = slots;
    ni->cap = ncap;
    return GRAPH_OK;
}

/* Returns the offset of name in the blob, adding it if new. */
static graph_status_t intern_name(graph_t *g, const char *name, uint32_t *out)
{
    *out = 0;
    if (!name || !*name) return GRAPH_OK;
    size_t len = 0;                             /* names capped at 255 bytes */
    while (len < 255 && name[len]) len++;

    name_index_t *ni = g->name_index;
    if (!ni) {
        ni = calloc(1, sizeof *ni);
        if (!ni) return GRAPH_ERR_NOMEM;
        g->name_index = ni;
    }
    if ((ni->used + 1) * 2 > ni->cap) {
        graph_status_t st = name_index_grow(g, ni);
        if (st != GRAPH_OK) return st;
    }

    char buf[256];
    memcpy(buf, name, len);
    buf[len] = '\0';
    size_t j = (size_t)fnv1a(buf) & (ni->cap - 1);
    while (ni->slots[j]) {
        if (strcmp(g->names + ni->slots[j], buf) == 0) { *out = ni->slots[j]; return GRAPH_OK; }
        j = (j + 1) & (ni->cap - 1);
    }
    if (g->names_size + len + 1 > UINT32_MAX) return GRAPH_ERR_OVERFLOW;
    graph_status_t st = grow((void **)&g->names, &g->names_cap, g->names_size + len + 1, 1);
    if (st != GRAPH_OK) return st;
    uint32_t off = (uint32_t)g->names_size;
    memcpy(g->names + off, buf, len + 1);
    g->names_size += len + 1;
    ni->slots[j] = off;
    ni->used++;
    *out = off;
    return GRAPH_OK;
}

graph_status_t graph_add_node(graph_t *g, int64_t id, double lat, double lon,
                              node_idx_t *out_idx)
{
    if (!g) return GRAPH_ERR_INVALID;
    if (g->finalized) return GRAPH_ERR_FINALIZED;
    if (!geo_valid_coord(lat, lon)) return GRAPH_ERR_INVALID;
    /* node indices are 32-bit; reserve UINT32_MAX as the invalid marker */
    if (g->node_count >= (size_t)IRA_INVALID_IDX - 1) return GRAPH_ERR_OVERFLOW;

    graph_status_t st = grow((void **)&g->nodes, &g->node_cap,
                             g->node_count + 1, sizeof *g->nodes);
    if (st != GRAPH_OK) return st;

    g->nodes[g->node_count] = (graph_node_t){ .id = id, .lat = lat, .lon = lon };
    if (out_idx) *out_idx = (node_idx_t)g->node_count;
    g->node_count++;
    return GRAPH_OK;
}

static bool positive_finite(double x) { return isfinite(x) && x > 0.0 && x < 3.0e38; }

/* Rounds a positive double UP to float, so a stored distance is never shorter
 * than the true one (keeps the A* heuristic admissible after rounding). */
static float round_up_f(double d)
{
    float f = (float)d;
    if ((double)f < d) f = nextafterf(f, INFINITY);
    return f;
}

graph_status_t graph_add_edge_ex(graph_t *g, int64_t id, node_idx_t from,
                                 node_idx_t to, double distance_m,
                                 double base_time_s, road_class_t road_class,
                                 double speed_limit_kmh, uint32_t flags,
                                 const char *name, const graph_point_t *shape,
                                 uint32_t shape_count)
{
    if (!g) return GRAPH_ERR_INVALID;
    if (g->finalized) return GRAPH_ERR_FINALIZED;
    if (from >= g->node_count || to >= g->node_count) return GRAPH_ERR_INVALID;
    /* Dijkstra requires non-negative weights; we require strictly positive
     * so that zero-cost cycles cannot appear. */
    if (!positive_finite(distance_m) || !positive_finite(base_time_s))
        return GRAPH_ERR_INVALID;
    if ((unsigned)road_class >= ROAD_CLASS_COUNT) return GRAPH_ERR_INVALID;
    if (!isfinite(speed_limit_kmh) || speed_limit_kmh < 0) return GRAPH_ERR_INVALID;
    if (flags > 0xFF || (shape_count && !shape)) return GRAPH_ERR_INVALID;
    if (g->edge_count >= (size_t)IRA_INVALID_IDX - 1) return GRAPH_ERR_OVERFLOW;
    if (g->shape_count + shape_count > UINT32_MAX) return GRAPH_ERR_OVERFLOW;

    graph_status_t st = grow((void **)&g->edges, &g->edge_cap,
                             g->edge_count + 1, sizeof *g->edges);
    if (st != GRAPH_OK) return st;
    if (shape_count) {
        st = grow((void **)&g->shape, &g->shape_cap, g->shape_count + shape_count,
                  sizeof *g->shape);
        if (st != GRAPH_OK) return st;
    }
    uint32_t name_id;
    st = intern_name(g, name, &name_id);
    if (st != GRAPH_OK) return st;

    uint32_t goff = (uint32_t)g->shape_count;
    if (shape_count) {
        memcpy(g->shape + g->shape_count, shape, shape_count * sizeof *shape);
        g->shape_count += shape_count;
    }

    g->edges[g->edge_count++] = (graph_edge_t){
        .id = id, .from = from, .to = to,
        .distance_m = round_up_f(distance_m),
        .base_time_s = round_up_f(base_time_s),
        .traffic_time_s = 0.0f,
        .name_id = name_id, .geom_offset = goff, .geom_count = shape_count,
        .speed_limit_kmh = (uint8_t)(speed_limit_kmh > 255 ? 255 : speed_limit_kmh),
        .road_class = (uint8_t)road_class, .flags = (uint8_t)flags,
    };
    return GRAPH_OK;
}

graph_status_t graph_add_edge(graph_t *g, int64_t id, node_idx_t from,
                              node_idx_t to, double distance_m,
                              double base_time_s, road_class_t road_class,
                              double speed_limit_kmh, uint32_t flags)
{
    return graph_add_edge_ex(g, id, from, to, distance_m, base_time_s, road_class,
                             speed_limit_kmh, flags, NULL, NULL, 0);
}

double graph_edge_time(const graph_edge_t *e)
{
    return e->traffic_time_s > 0.0f ? (double)e->traffic_time_s : (double)e->base_time_s;
}

const char *graph_edge_name(const graph_t *g, const graph_edge_t *e)
{
    return (e->name_id < g->names_size) ? g->names + e->name_id : "";
}

void graph_recompute_max_speed(graph_t *g)
{
    double vmax = 0.0;
    for (size_t i = 0; i < g->edge_count; i++) {
        double v = (double)g->edges[i].distance_m / graph_edge_time(&g->edges[i]);
        if (v > vmax) vmax = v;
    }
    g->max_speed_mps = vmax;
}

/*
 * Builds the CSR structure with a stable counting sort on edge.from:
 *   1. count out-degree of every node           O(E)
 *   2. prefix-sum counts into first_out[]       O(V)
 *   3. scatter edges into their slots           O(E)
 * Total O(V + E), no comparison sort needed.
 */
graph_status_t graph_finalize(graph_t *g)
{
    if (!g) return GRAPH_ERR_INVALID;
    if (g->finalized) return GRAPH_ERR_FINALIZED;

    size_t V = g->node_count, E = g->edge_count;
    if (V + 1 > SIZE_MAX / sizeof(edge_idx_t)) return GRAPH_ERR_OVERFLOW;

    edge_idx_t *first_out = calloc(V + 1, sizeof *first_out);
    if (!first_out) return GRAPH_ERR_NOMEM;
    if (!g->node_flags && V > 0) {
        g->node_flags = calloc(V, 1);
        if (!g->node_flags) { free(first_out); return GRAPH_ERR_NOMEM; }
    }

    for (size_t i = 0; i < E; i++) first_out[g->edges[i].from + 1]++;
    for (size_t v = 0; v < V; v++) first_out[v + 1] += first_out[v];

    if (E > 0) {
        graph_edge_t *sorted = malloc(E * sizeof *sorted);
        edge_idx_t *cursor = malloc(V * sizeof *cursor);
        if (!sorted || !cursor) {
            free(sorted); free(cursor); free(first_out);
            return GRAPH_ERR_NOMEM;
        }
        memcpy(cursor, first_out, V * sizeof *cursor);
        for (size_t i = 0; i < E; i++)
            sorted[cursor[g->edges[i].from]++] = g->edges[i];
        free(g->edges);
        g->edges = sorted;
        g->edge_cap = E;
        free(cursor);
    }

    g->first_out = first_out;
    g->finalized = true;
    name_index_free(g);
    graph_recompute_max_speed(g);

    g->min_lat = g->min_lon = INFINITY;
    g->max_lat = g->max_lon = -INFINITY;
    for (size_t i = 0; i < V; i++) {
        const graph_node_t *n = &g->nodes[i];
        if (n->lat < g->min_lat) g->min_lat = n->lat;
        if (n->lat > g->max_lat) g->max_lat = n->lat;
        if (n->lon < g->min_lon) g->min_lon = n->lon;
        if (n->lon > g->max_lon) g->max_lon = n->lon;
    }
    return GRAPH_OK;
}

node_idx_t graph_find_node(const graph_t *g, int64_t id)
{
    for (size_t i = 0; g && i < g->node_count; i++)
        if (g->nodes[i].id == id) return (node_idx_t)i;
    return IRA_INVALID_IDX;
}

edge_idx_t graph_find_edge(const graph_t *g, int64_t id)
{
    for (size_t i = 0; g && i < g->edge_count; i++)
        if (g->edges[i].id == id) return (edge_idx_t)i;
    return IRA_INVALID_IDX;
}

edge_idx_t graph_find_reverse(const graph_t *g, edge_idx_t ei)
{
    if (!g || !g->finalized || ei >= g->edge_count) return IRA_INVALID_IDX;
    const graph_edge_t *e = &g->edges[ei];
    edge_idx_t fallback = IRA_INVALID_IDX;
    for (edge_idx_t j = graph_out_begin(g, e->to); j < graph_out_end(g, e->to); j++) {
        const graph_edge_t *r = &g->edges[j];
        if (r->to != e->from) continue;
        if (r->id == -e->id) return j;
        if (fallback == IRA_INVALID_IDX && r->distance_m == e->distance_m &&
            r->name_id == e->name_id)
            fallback = j;
    }
    return fallback;
}

graph_status_t graph_set_traffic_time(graph_t *g, edge_idx_t e, double time_s)
{
    if (!g) return GRAPH_ERR_INVALID;
    if (!g->finalized) return GRAPH_ERR_NOT_FINALIZED;
    if (e >= g->edge_count) return GRAPH_ERR_NOT_FOUND;
    if (!isfinite(time_s) || time_s > 1.0e7) return GRAPH_ERR_INVALID;

    graph_edge_t *edge = &g->edges[e];
    edge->traffic_time_s = time_s > 0.0 ? round_up_f(time_s) : 0.0f;

    /* Keep max_speed an upper bound so A*'s time heuristic stays admissible.
     * Raising is O(1); lowering needs a full scan (graph_recompute_max_speed). */
    double v = (double)edge->distance_m / graph_edge_time(edge);
    if (v > g->max_speed_mps) g->max_speed_mps = v;
    return GRAPH_OK;
}

graph_status_t graph_set_closed(graph_t *g, edge_idx_t e, bool closed)
{
    if (!g) return GRAPH_ERR_INVALID;
    if (!g->finalized) return GRAPH_ERR_NOT_FINALIZED;
    if (e >= g->edge_count) return GRAPH_ERR_NOT_FOUND;
    if (closed) g->edges[e].flags |= EDGE_FLAG_CLOSED;
    else        g->edges[e].flags &= (uint8_t)~EDGE_FLAG_CLOSED;
    return GRAPH_OK;
}

void graph_clear_traffic(graph_t *g)
{
    if (!g || !g->finalized) return;
    for (size_t i = 0; i < g->edge_count; i++) g->edges[i].traffic_time_s = 0.0f;
    graph_recompute_max_speed(g);
}

size_t graph_memory_bytes(const graph_t *g)
{
    if (!g) return 0;
    return g->node_cap * sizeof *g->nodes + (g->node_flags ? g->node_count : 0) +
           g->edge_cap * sizeof *g->edges + g->shape_cap * sizeof *g->shape +
           g->names_cap + (g->first_out ? (g->node_count + 1) * sizeof *g->first_out : 0);
}

/* Default speeds in km/h. These are planning assumptions for Indian roads,
 * documented in docs/data_pipeline.md, not measured values. */
static const double DEFAULT_SPEED_KMH[ROAD_CLASS_COUNT] = {
    [ROAD_MOTORWAY] = 80, [ROAD_TRUNK] = 70, [ROAD_PRIMARY] = 55,
    [ROAD_SECONDARY] = 45, [ROAD_TERTIARY] = 35, [ROAD_UNCLASSIFIED] = 25,
    [ROAD_RESIDENTIAL] = 25, [ROAD_SERVICE] = 15, [ROAD_TRACK] = 12,
};

static const char *ROAD_NAMES[ROAD_CLASS_COUNT] = {
    [ROAD_MOTORWAY] = "motorway", [ROAD_TRUNK] = "trunk",
    [ROAD_PRIMARY] = "primary", [ROAD_SECONDARY] = "secondary",
    [ROAD_TERTIARY] = "tertiary", [ROAD_UNCLASSIFIED] = "unclassified",
    [ROAD_RESIDENTIAL] = "residential", [ROAD_SERVICE] = "service",
    [ROAD_TRACK] = "track",
};

double road_class_default_speed_kmh(road_class_t c)
{
    return (unsigned)c < ROAD_CLASS_COUNT ? DEFAULT_SPEED_KMH[c] : 20.0;
}

const char *road_class_name(road_class_t c)
{
    return (unsigned)c < ROAD_CLASS_COUNT ? ROAD_NAMES[c] : "unknown";
}

bool road_class_from_name(const char *name, road_class_t *out)
{
    if (!name) return false;
    for (int i = 0; i < ROAD_CLASS_COUNT; i++) {
        if (strcmp(name, ROAD_NAMES[i]) == 0) {
            *out = (road_class_t)i;
            return true;
        }
    }
    return false;
}
