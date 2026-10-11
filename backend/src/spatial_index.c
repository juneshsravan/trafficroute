/*
 * spatial_index.c — grid index implementation (see spatial_index.h).
 */
#include "spatial_index.h"
#include "geospatial.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG_M          111195.0      /* metres per degree of latitude */
#define MAX_CELLS      (16u * 1024u * 1024u)
#define DEFAULT_CELL   0.01          /* ~1.1 km */

struct spatial_grid {
    double    min_lat, min_lon, cell_deg;
    uint32_t  rows, cols;
    uint32_t *cell_start;    /* rows*cols + 1 offsets into entries */
    uint32_t *payload;       /* node or edge index */
    float    *lat, *lon;
    size_t    count;
};

static void grid_free(spatial_grid_t *g)
{
    if (!g) return;
    free(g->cell_start); free(g->payload); free(g->lat); free(g->lon);
    free(g);
}

static inline uint32_t clampu(double v, uint32_t max)
{
    if (!(v > 0)) return 0;
    if (v >= (double)max) return max - 1;
    return (uint32_t)v;
}

static inline uint32_t cell_of(const spatial_grid_t *g, double lat, double lon)
{
    uint32_t r = clampu((lat - g->min_lat) / g->cell_deg, g->rows);
    uint32_t c = clampu((lon - g->min_lon) / g->cell_deg, g->cols);
    return r * g->cols + c;
}

/* Builds a grid from parallel arrays via a counting sort on cell id. */
static spatial_grid_t *grid_build(const float *lat, const float *lon,
                                  const uint32_t *payload, size_t n)
{
    spatial_grid_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    double mnla = 90, mnlo = 180, mxla = -90, mxlo = -180;
    for (size_t i = 0; i < n; i++) {
        if (lat[i] < mnla) mnla = lat[i];
        if (lat[i] > mxla) mxla = lat[i];
        if (lon[i] < mnlo) mnlo = lon[i];
        if (lon[i] > mxlo) mxlo = lon[i];
    }
    if (n == 0) { mnla = mnlo = 0; mxla = mxlo = 0; }
    g->min_lat = mnla; g->min_lon = mnlo;
    g->cell_deg = DEFAULT_CELL;
    for (;;) {
        double rows = floor((mxla - mnla) / g->cell_deg) + 1;
        double cols = floor((mxlo - mnlo) / g->cell_deg) + 1;
        if (rows * cols <= MAX_CELLS) { g->rows = (uint32_t)rows; g->cols = (uint32_t)cols; break; }
        g->cell_deg *= 1.5;
    }
    size_t cells = (size_t)g->rows * g->cols;
    g->cell_start = calloc(cells + 1, sizeof *g->cell_start);
    g->payload = malloc((n ? n : 1) * sizeof *g->payload);
    g->lat = malloc((n ? n : 1) * sizeof *g->lat);
    g->lon = malloc((n ? n : 1) * sizeof *g->lon);
    uint32_t *cursor = malloc(cells * sizeof *cursor);
    if (!g->cell_start || !g->payload || !g->lat || !g->lon || !cursor) {
        free(cursor); grid_free(g); return NULL;
    }
    for (size_t i = 0; i < n; i++) g->cell_start[cell_of(g, lat[i], lon[i]) + 1]++;
    for (size_t c = 0; c < cells; c++) g->cell_start[c + 1] += g->cell_start[c];
    memcpy(cursor, g->cell_start, cells * sizeof *cursor);
    for (size_t i = 0; i < n; i++) {
        uint32_t k = cursor[cell_of(g, lat[i], lon[i])]++;
        g->payload[k] = payload[i];
        g->lat[k] = lat[i];
        g->lon[k] = lon[i];
    }
    free(cursor);
    g->count = n;
    return g;
}

/* Visits grid entries in square rings of cells around the query point, ring
 * by ring, until the ring is provably farther than max_m or than the best
 * candidate found so far (*best_m, updated by the visitor). */
typedef void (*visit_fn)(void *ctx, uint32_t payload, float lat, float lon);

static void grid_rings(const spatial_grid_t *g, double lat, double lon, double max_m,
                       const double *best_m, visit_fn fn, void *ctx)
{
    if (!g || g->count == 0) return;
    /* Smallest metric size of a cell near this latitude (longitude shrinks). */
    double coslat = cos((fabs(lat) + g->cell_deg) * M_PI / 180.0);
    if (coslat < 0.05) coslat = 0.05;
    double cell_m = g->cell_deg * DEG_M * coslat;

    long qr = (long)floor((lat - g->min_lat) / g->cell_deg);
    long qc = (long)floor((lon - g->min_lon) / g->cell_deg);
    long max_ring = (long)ceil(max_m / cell_m) + 1;
    long limit = (long)(g->rows > g->cols ? g->rows : g->cols) + 1;
    if (max_ring > limit + labs(qr) + labs(qc)) max_ring = limit + labs(qr) + labs(qc);

    for (long ring = 0; ring <= max_ring; ring++) {
        double ring_min_m = (double)(ring - 1) * cell_m;
        if (ring > 0 && (ring_min_m > max_m || ring_min_m > *best_m)) break;
        for (long r = qr - ring; r <= qr + ring; r++) {
            if (r < 0 || r >= (long)g->rows) continue;
            bool edge_row = (r == qr - ring || r == qr + ring);
            long step = edge_row ? 1 : 2 * ring;
            for (long c = qc - ring; c <= qc + ring; c += (step ? step : 1)) {
                if (c < 0 || c >= (long)g->cols) continue;
                size_t cell = (size_t)r * g->cols + (size_t)c;
                for (uint32_t k = g->cell_start[cell]; k < g->cell_start[cell + 1]; k++)
                    fn(ctx, g->payload[k], g->lat[k], g->lon[k]);
                if (step == 0) break;
            }
        }
    }
}

spatial_index_t *spatial_index_build(const graph_t *gr)
{
    if (!gr || !gr->finalized) return NULL;
    spatial_index_t *si = calloc(1, sizeof *si);
    if (!si) return NULL;

    /* Nodes that touch at least one edge (in or out). */
    size_t V = gr->node_count;
    uint8_t *has_edge = calloc(V ? V : 1, 1);
    if (!has_edge) { free(si); return NULL; }
    for (size_t i = 0; i < gr->edge_count; i++) {
        has_edge[gr->edges[i].from] = 1;
        has_edge[gr->edges[i].to] = 1;
    }
    size_t nn = 0;
    for (size_t i = 0; i < V; i++) nn += has_edge[i];
    float *la = malloc((nn ? nn : 1) * sizeof *la), *lo = malloc((nn ? nn : 1) * sizeof *lo);
    uint32_t *pl = malloc((nn ? nn : 1) * sizeof *pl);
    if (!la || !lo || !pl) goto fail_nodes;
    size_t k = 0;
    for (size_t i = 0; i < V; i++)
        if (has_edge[i]) {
            la[k] = (float)gr->nodes[i].lat; lo[k] = (float)gr->nodes[i].lon;
            pl[k++] = (uint32_t)i;
        }
    si->nodes = grid_build(la, lo, pl, nn);
    free(la); free(lo); free(pl); free(has_edge);
    if (!si->nodes) { free(si); return NULL; }

    /* Edge vertices: from-node, shape points, to-node. */
    size_t ne = 0;
    for (size_t i = 0; i < gr->edge_count; i++) ne += 2 + gr->edges[i].geom_count;
    if (ne >= UINT32_MAX) { spatial_index_free(si); return NULL; }
    la = malloc((ne ? ne : 1) * sizeof *la); lo = malloc((ne ? ne : 1) * sizeof *lo);
    pl = malloc((ne ? ne : 1) * sizeof *pl);
    if (!la || !lo || !pl) { free(la); free(lo); free(pl); spatial_index_free(si); return NULL; }
    k = 0;
    for (size_t i = 0; i < gr->edge_count; i++) {
        const graph_edge_t *e = &gr->edges[i];
        la[k] = (float)gr->nodes[e->from].lat; lo[k] = (float)gr->nodes[e->from].lon; pl[k++] = (uint32_t)i;
        for (uint32_t s = 0; s < e->geom_count; s++) {
            la[k] = gr->shape[e->geom_offset + s].lat; lo[k] = gr->shape[e->geom_offset + s].lon;
            pl[k++] = (uint32_t)i;
        }
        la[k] = (float)gr->nodes[e->to].lat; lo[k] = (float)gr->nodes[e->to].lon; pl[k++] = (uint32_t)i;
        for (size_t q = k - e->geom_count - 2; q + 1 < k; q++) {
            double d = geo_haversine_m(la[q], lo[q], la[q + 1], lo[q + 1]);
            if (d > si->max_seg_m) si->max_seg_m = d;
        }
    }
    si->edges = grid_build(la, lo, pl, ne);
    free(la); free(lo); free(pl);
    if (!si->edges) { spatial_index_free(si); return NULL; }
    return si;

fail_nodes:
    free(la); free(lo); free(pl); free(has_edge); free(si);
    return NULL;
}

void spatial_index_free(spatial_index_t *si)
{
    if (!si) return;
    grid_free(si->nodes);
    grid_free(si->edges);
    free(si);
}

static size_t grid_mem(const spatial_grid_t *g)
{
    if (!g) return 0;
    return ((size_t)g->rows * g->cols + 1) * 4 + g->count * (4 + 4 + 4);
}

size_t spatial_index_memory_bytes(const spatial_index_t *si)
{
    return si ? grid_mem(si->nodes) + grid_mem(si->edges) : 0;
}

/* ---------------- nearest node ---------------- */

typedef struct {
    const graph_t *g;
    double lat, lon;
    bool main_only;
    double best_m;
    node_idx_t best;
} node_ctx_t;

static void visit_node(void *vctx, uint32_t payload, float lat, float lon)
{
    node_ctx_t *c = vctx;
    if (c->main_only && !(c->g->node_flags[payload] & NODE_FLAG_MAIN_COMPONENT)) return;
    (void)lat; (void)lon;
    const graph_node_t *n = &c->g->nodes[payload];
    double d = geo_haversine_m(c->lat, c->lon, n->lat, n->lon);
    if (d < c->best_m || (d == c->best_m && payload < c->best)) { c->best_m = d; c->best = payload; }
}

bool spatial_snap_node(const spatial_index_t *si, const graph_t *g,
                       double lat, double lon, double max_m,
                       node_idx_t *out, double *out_dist_m)
{
    if (!si || !g || !geo_valid_coord(lat, lon)) return false;
    for (int pass = 0; pass < 2; pass++) {
        node_ctx_t c = { .g = g, .lat = lat, .lon = lon, .main_only = (pass == 0),
                         .best_m = INFINITY, .best = IRA_INVALID_IDX };
        grid_rings(si->nodes, lat, lon, max_m, &c.best_m, visit_node, &c);
        if (c.best != IRA_INVALID_IDX && c.best_m <= max_m) {
            if (out) *out = c.best;
            if (out_dist_m) *out_dist_m = c.best_m;
            return true;
        }
    }
    return false;
}

/* ---------------- nearest edge ---------------- */

/* Distance from P to segment AB in a local equirectangular projection
 * centred on P (accurate to well under 1% for segments of a few km). */
static double seg_dist_m(double plat, double plon, double alat, double alon,
                         double blat, double blon)
{
    double k = cos(plat * M_PI / 180.0);
    double ax = (alon - plon) * k * DEG_M, ay = (alat - plat) * DEG_M;
    double bx = (blon - plon) * k * DEG_M, by = (blat - plat) * DEG_M;
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double x = ax + t * dx, y = ay + t * dy;
    return sqrt(x * x + y * y);
}

double spatial_point_edge_distance(const graph_t *g, edge_idx_t ei, double lat, double lon)
{
    const graph_edge_t *e = &g->edges[ei];
    double plat = g->nodes[e->from].lat, plon = g->nodes[e->from].lon;
    double best = INFINITY;
    for (uint32_t s = 0; s <= e->geom_count; s++) {
        double qlat, qlon;
        if (s < e->geom_count) {
            qlat = g->shape[e->geom_offset + s].lat;
            qlon = g->shape[e->geom_offset + s].lon;
        } else {
            qlat = g->nodes[e->to].lat;
            qlon = g->nodes[e->to].lon;
        }
        double d = seg_dist_m(lat, lon, plat, plon, qlat, qlon);
        if (d < best) best = d;
        plat = qlat; plon = qlon;
    }
    return best;
}

typedef struct {
    const graph_t *g;
    double lat, lon;
    double best_m;
    edge_idx_t best;
    edge_idx_t last;     /* consecutive entries often share an edge */
} edge_ctx_t;

static void visit_edge(void *vctx, uint32_t payload, float lat, float lon)
{
    edge_ctx_t *c = vctx;
    (void)lat; (void)lon;
    if (payload == c->last) return;
    c->last = payload;
    double d = spatial_point_edge_distance(c->g, payload, c->lat, c->lon);
    if (d < c->best_m || (d == c->best_m && payload < c->best)) { c->best_m = d; c->best = payload; }
}

bool spatial_nearest_edge(const spatial_index_t *si, const graph_t *g,
                          double lat, double lon, double max_m,
                          edge_idx_t *out, double *out_dist_m)
{
    if (!si || !g || !geo_valid_coord(lat, lon)) return false;
    edge_ctx_t c = { .g = g, .lat = lat, .lon = lon, .best_m = INFINITY,
                     .best = IRA_INVALID_IDX, .last = IRA_INVALID_IDX };
    /* The closest point on an edge can be up to half its longest segment away
     * from any indexed vertex, so widen the vertex search by that much. */
    double search = max_m + si->max_seg_m / 2.0 + 1.0;
    double best_guard = INFINITY;
    grid_rings(si->edges, lat, lon, search, &best_guard, visit_edge, &c);
    if (c.best == IRA_INVALID_IDX || c.best_m > max_m) return false;
    if (out) *out = c.best;
    if (out_dist_m) *out_dist_m = c.best_m;
    return true;
}

void spatial_edges_in_bbox(const spatial_index_t *si, double min_lat, double min_lon,
                           double max_lat, double max_lon,
                           bool (*fn)(void *ctx, edge_idx_t e), void *ctx)
{
    if (!si || !si->edges || si->edges->count == 0) return;
    const spatial_grid_t *g = si->edges;
    long r0 = (long)floor((min_lat - g->min_lat) / g->cell_deg);
    long r1 = (long)floor((max_lat - g->min_lat) / g->cell_deg);
    long c0 = (long)floor((min_lon - g->min_lon) / g->cell_deg);
    long c1 = (long)floor((max_lon - g->min_lon) / g->cell_deg);
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= (long)g->rows) r1 = (long)g->rows - 1;
    if (c1 >= (long)g->cols) c1 = (long)g->cols - 1;
    edge_idx_t last = IRA_INVALID_IDX;
    for (long r = r0; r <= r1; r++)
        for (long c = c0; c <= c1; c++) {
            size_t cell = (size_t)r * g->cols + (size_t)c;
            for (uint32_t k = g->cell_start[cell]; k < g->cell_start[cell + 1]; k++) {
                if (g->lat[k] < min_lat || g->lat[k] > max_lat ||
                    g->lon[k] < min_lon || g->lon[k] > max_lon) continue;
                if (g->payload[k] == last) continue;
                last = g->payload[k];
                if (!fn(ctx, last)) return;
            }
        }
}
