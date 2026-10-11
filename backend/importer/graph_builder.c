/*
 * graph_builder.c — two-pass OSM import (see graph_builder.h).
 */
#define _POSIX_C_SOURCE 200809L
#include "graph_builder.h"
#include "geospatial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COORD_MISSING INT32_MIN

typedef struct {
    int64_t  id;
    size_t   ref_off;
    uint32_t ref_n;
    way_attrs_t a;
} way_rec_t;

typedef struct {
    const import_options_t *opt;
    import_stats_t *st;

    int64_t   *refs;   size_t refs_n, refs_cap;
    way_rec_t *ways;   size_t ways_n, ways_cap;
    import_place_t *places; size_t places_n, places_cap;

    int64_t  *uids;    /* sorted unique referenced node ids */
    uint8_t  *ucount;  /* saturating usage count */
    int32_t  *ulat, *ulon;   /* 1e-7 degrees */
    size_t    un;
    bool      failed;
} ctx_t;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool grow(void **buf, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap : 1024;
    while (nc < need) { if (nc > SIZE_MAX / 2) return false; nc *= 2; }
    if (nc > SIZE_MAX / elem) return false;
    void *p = realloc(*buf, nc * elem);
    if (!p) return false;
    *buf = p;
    *cap = nc;
    return true;
}

void import_default_options(import_options_t *o)
{
    memset(o, 0, sizeof *o);
    o->rules.include_tracks = true;
    o->max_edge_m = 1000.0;
    o->early_stop = true;
}

/* ---------------- pass 1 ---------------- */

static int p1_node(const void *ud, const readosm_node *n)
{
    ctx_t *c = (ctx_t *)(uintptr_t)ud;
    c->st->nodes_seen++;
    if (n->tag_count == 0) return READOSM_OK;
    const char *place = osm_tag(n->tags, n->tag_count, "place");
    int rank = osm_place_rank(place);
    if (rank < 0) return READOSM_OK;
    const char *name = osm_tag(n->tags, n->tag_count, "name");
    const char *name_en = osm_tag(n->tags, n->tag_count, "name:en");
    if (!name && !name_en) return READOSM_OK;
    if (!geo_valid_coord(n->latitude, n->longitude)) return READOSM_OK;
    if (!grow((void **)&c->places, &c->places_cap, c->places_n + 1, sizeof *c->places)) {
        c->failed = true;
        return READOSM_ABORT;
    }
    import_place_t *p = &c->places[c->places_n++];
    memset(p, 0, sizeof *p);
    p->osm_id = n->id;
    p->lat = n->latitude;
    p->lon = n->longitude;
    p->rank = rank;
    snprintf(p->type, sizeof p->type, "%s", place);
    snprintf(p->name, sizeof p->name, "%s", name ? name : name_en);
    if (name_en) snprintf(p->name_en, sizeof p->name_en, "%s", name_en);
    const char *state = osm_tag(n->tags, n->tag_count, "addr:state");
    if (!state) state = osm_tag(n->tags, n->tag_count, "is_in:state");
    const char *district = osm_tag(n->tags, n->tag_count, "addr:district");
    if (!district) district = osm_tag(n->tags, n->tag_count, "is_in:district");
    if (state) snprintf(p->state, sizeof p->state, "%s", state);
    if (district) snprintf(p->district, sizeof p->district, "%s", district);
    const char *pop = osm_tag(n->tags, n->tag_count, "population");
    if (pop) {
        char *end;
        long long v = strtoll(pop, &end, 10);
        if (end != pop && v > 0) p->population = v;
    }
    return READOSM_OK;
}

static int p1_way(const void *ud, const readosm_way *w)
{
    ctx_t *c = (ctx_t *)(uintptr_t)ud;
    c->st->ways_seen++;
    way_attrs_t a;
    way_reason_t r = osm_classify_way(w->tags, w->tag_count, &c->opt->rules, &a);
    if (r != WAY_OK) { c->st->ways_dropped[r]++; return READOSM_OK; }
    if (w->node_ref_count < 2) { c->st->ways_dropped[WAY_NOT_ROUTABLE_TYPE]++; return READOSM_OK; }
    if (!grow((void **)&c->refs, &c->refs_cap, c->refs_n + (size_t)w->node_ref_count, sizeof *c->refs) ||
        !grow((void **)&c->ways, &c->ways_cap, c->ways_n + 1, sizeof *c->ways)) {
        c->failed = true;
        return READOSM_ABORT;
    }
    way_rec_t *rec = &c->ways[c->ways_n++];
    rec->id = w->id;
    rec->ref_off = c->refs_n;
    rec->ref_n = (uint32_t)w->node_ref_count;
    rec->a = a;
    for (int i = 0; i < w->node_ref_count; i++) c->refs[c->refs_n++] = w->node_refs[i];
    c->st->ways_routable++;
    return READOSM_OK;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* Builds uids/ucount from all way refs. */
static bool build_unique_ids(ctx_t *c)
{
    if (c->refs_n == 0) return true;
    int64_t *tmp = malloc(c->refs_n * sizeof *tmp);
    if (!tmp) return false;
    memcpy(tmp, c->refs, c->refs_n * sizeof *tmp);
    qsort(tmp, c->refs_n, sizeof *tmp, cmp_i64);
    size_t u = 0;
    for (size_t i = 0; i < c->refs_n; i++)
        if (i == 0 || tmp[i] != tmp[i - 1]) u++;
    c->uids = malloc(u * sizeof *c->uids);
    c->ucount = calloc(u, 1);
    c->ulat = malloc(u * sizeof *c->ulat);
    c->ulon = malloc(u * sizeof *c->ulon);
    if (!c->uids || !c->ucount || !c->ulat || !c->ulon) { free(tmp); return false; }
    size_t k = 0;
    for (size_t i = 0; i < c->refs_n; i++) {
        if (i == 0 || tmp[i] != tmp[i - 1]) c->uids[k++] = tmp[i];
        if (c->ucount[k - 1] < 255) c->ucount[k - 1]++;
    }
    free(tmp);
    c->un = u;
    for (size_t i = 0; i < u; i++) c->ulat[i] = c->ulon[i] = COORD_MISSING;
    return true;
}

static size_t find_uid(const ctx_t *c, int64_t id)
{
    size_t lo = 0, hi = c->un;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->uids[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    return (lo < c->un && c->uids[lo] == id) ? lo : SIZE_MAX;
}

/* ---------------- pass 2 ---------------- */

static int p2_node(const void *ud, const readosm_node *n)
{
    ctx_t *c = (ctx_t *)(uintptr_t)ud;
    size_t k = find_uid(c, n->id);
    if (k != SIZE_MAX && geo_valid_coord(n->latitude, n->longitude)) {
        if (c->ulat[k] == COORD_MISSING) c->st->coords_found++;
        c->ulat[k] = (int32_t)lround(n->latitude * 1e7);
        c->ulon[k] = (int32_t)lround(n->longitude * 1e7);
    }
    return READOSM_OK;
}

static int p2_way_stop(const void *ud, const readosm_way *w)
{
    (void)ud; (void)w;
    return READOSM_ABORT;     /* sorted files: all nodes come before ways */
}

/* ---------------- build ---------------- */

static inline double ulat_d(const ctx_t *c, size_t k) { return c->ulat[k] / 1e7; }
static inline double ulon_d(const ctx_t *c, size_t k) { return c->ulon[k] / 1e7; }

typedef struct {
    graph_t   *g;
    uint32_t  *gidx;            /* uid index -> graph node */
    graph_point_t *shape;       /* scratch */
    size_t     shape_cap;
    int64_t    next_edge_id;
} build_t;

static node_idx_t graph_node_for(build_t *b, const ctx_t *c, size_t k)
{
    if (b->gidx[k] != IRA_INVALID_IDX) return b->gidx[k];
    node_idx_t idx;
    if (graph_add_node(b->g, c->uids[k], ulat_d(c, k), ulon_d(c, k), &idx) != GRAPH_OK)
        return IRA_INVALID_IDX;
    b->gidx[k] = idx;
    return idx;
}

/* Emits the segment uk[from..to] (indices into the way's uid list). */
static bool emit_segment(build_t *b, ctx_t *c, const way_rec_t *w, const size_t *uk,
                         size_t from, size_t to, double dist)
{
    import_stats_t *st = c->st;
    if (uk[from] == uk[to]) { st->self_loops_skipped++; return true; }
    if (!(dist > 0.05)) { st->zero_length_skipped++; return true; }
    node_idx_t a = graph_node_for(b, c, uk[from]);
    node_idx_t z = graph_node_for(b, c, uk[to]);
    if (a == IRA_INVALID_IDX || z == IRA_INVALID_IDX) return false;

    size_t ns = to - from - 1;
    if (ns > b->shape_cap) {
        graph_point_t *p = realloc(b->shape, ns * sizeof *p);
        if (!p) return false;
        b->shape = p;
        b->shape_cap = ns;
    }
    for (size_t i = 0; i < ns; i++)
        b->shape[i] = (graph_point_t){ (float)ulat_d(c, uk[from + 1 + i]), (float)ulon_d(c, uk[from + 1 + i]) };

    double time_s = dist / (w->a.speed_kmh / 3.6);
    uint32_t flags = (w->a.toll ? EDGE_FLAG_TOLL : 0) | (w->a.restricted ? EDGE_FLAG_RESTRICTED : 0);
    int64_t id = ++b->next_edge_id;
    graph_status_t gs = GRAPH_OK;
    if (w->a.oneway >= 0)
        gs = graph_add_edge_ex(b->g, id, a, z, dist, time_s, w->a.cls, w->a.maxspeed_kmh, flags,
                               w->a.name, b->shape, (uint32_t)ns);
    if (gs == GRAPH_OK && w->a.oneway <= 0) {
        /* reverse direction: reversed shape */
        for (size_t i = 0; i < ns / 2; i++) {
            graph_point_t t = b->shape[i];
            b->shape[i] = b->shape[ns - 1 - i];
            b->shape[ns - 1 - i] = t;
        }
        gs = graph_add_edge_ex(b->g, -id, z, a, dist, time_s, w->a.cls, w->a.maxspeed_kmh, flags,
                               w->a.name, b->shape, (uint32_t)ns);
    }
    if (gs != GRAPH_OK) {
        snprintf(st->error, sizeof st->error, "edge insert failed: %s", graph_status_str(gs));
        return false;
    }
    st->segments++;
    if (w->a.oneway != 0) st->oneway_segments++;
    st->road_km += dist / 1000.0;
    return true;
}

static bool build_graph(ctx_t *c, graph_t *g)
{
    build_t b = { .g = g };
    b.gidx = malloc((c->un ? c->un : 1) * sizeof *b.gidx);
    size_t max_ref = 0;
    for (size_t i = 0; i < c->ways_n; i++) if (c->ways[i].ref_n > max_ref) max_ref = c->ways[i].ref_n;
    size_t *uk = malloc((max_ref ? max_ref : 1) * sizeof *uk);
    if (!b.gidx || !uk) { free(b.gidx); free(uk); return false; }
    memset(b.gidx, 0xFF, c->un * sizeof *b.gidx);

    bool ok = true;
    for (size_t wi = 0; wi < c->ways_n && ok; wi++) {
        const way_rec_t *w = &c->ways[wi];
        /* Resolve refs; split the way into runs of nodes with known coords. */
        size_t n = 0;
        for (uint32_t r = 0; r <= w->ref_n && ok; r++) {
            size_t k = SIZE_MAX;
            if (r < w->ref_n) {
                k = find_uid(c, c->refs[w->ref_off + r]);
                if (k != SIZE_MAX && c->ulat[k] == COORD_MISSING) { c->st->refs_missing++; k = SIZE_MAX; }
            }
            if (k != SIZE_MAX) {
                if (n > 0 && uk[n - 1] == k) continue;      /* duplicate consecutive ref */
                uk[n++] = k;
                continue;
            }
            /* end of a run: segment it */
            if (n >= 2) {
                size_t start = 0;
                double dist = 0;
                for (size_t i = 1; i < n; i++) {
                    dist += geo_haversine_m(ulat_d(c, uk[i - 1]), ulon_d(c, uk[i - 1]),
                                            ulat_d(c, uk[i]), ulon_d(c, uk[i]));
                    bool junction = (i == n - 1) || c->ucount[uk[i]] >= 2;
                    bool too_long = c->opt->max_edge_m > 0 && dist >= c->opt->max_edge_m;
                    if (junction || too_long) {
                        if (!emit_segment(&b, c, w, uk, start, i, dist)) { ok = false; break; }
                        start = i;
                        dist = 0;
                    }
                }
            }
            n = 0;
        }
    }
    free(b.gidx);
    free(b.shape);
    free(uk);
    return ok;
}

/* Fills `near` of small places with the nearest city/town (within 50 km). */
static int cmp_place_lat(const void *a, const void *b)
{
    double x = (*(const import_place_t *const *)a)->lat, y = (*(const import_place_t *const *)b)->lat;
    return (x > y) - (x < y);
}

static void assign_nearest_towns(ctx_t *c)
{
    size_t nt = 0;
    for (size_t i = 0; i < c->places_n; i++) if (c->places[i].rank == 1 || c->places[i].rank == 3) nt++;
    if (!nt) return;
    import_place_t **towns = malloc(nt * sizeof *towns);
    if (!towns) return;
    size_t k = 0;
    for (size_t i = 0; i < c->places_n; i++)
        if (c->places[i].rank == 1 || c->places[i].rank == 3) towns[k++] = &c->places[i];
    qsort(towns, nt, sizeof *towns, cmp_place_lat);
    const double win = 0.5;   /* degrees latitude (~55 km) */
    for (size_t i = 0; i < c->places_n; i++) {
        import_place_t *p = &c->places[i];
        if (p->rank < 4) continue;
        size_t lo = 0, hi = nt;
        while (lo < hi) { size_t m = (lo + hi) / 2; if (towns[m]->lat < p->lat - win) lo = m + 1; else hi = m; }
        double best = 50000.0;
        const import_place_t *bt = NULL;
        for (size_t j = lo; j < nt && towns[j]->lat <= p->lat + win; j++) {
            double d = geo_haversine_m(p->lat, p->lon, towns[j]->lat, towns[j]->lon);
            if (d < best) { best = d; bt = towns[j]; }
        }
        if (bt) snprintf(p->near, sizeof p->near, "%s", bt->name_en[0] ? bt->name_en : bt->name);
    }
    free(towns);
}

static void ctx_free(ctx_t *c)
{
    free(c->refs); free(c->ways); free(c->uids); free(c->ucount); free(c->ulat); free(c->ulon);
}

bool import_osm(const char *path, const import_options_t *opt, import_result_t *out,
                import_stats_t *st)
{
    memset(out, 0, sizeof *out);
    memset(st, 0, sizeof *st);
    ctx_t c = { .opt = opt, .st = st };
    const void *h = NULL;

    double t0 = now_s();
    int rc = readosm_open(path, &h);
    if (rc != READOSM_OK) {
        snprintf(st->error, sizeof st->error, "cannot open %s (readosm error %d)", path, rc);
        readosm_close(h);
        return false;
    }
    rc = readosm_parse(h, &c, p1_node, p1_way, NULL);
    readosm_close(h);
    if (rc != READOSM_OK || c.failed) {
        snprintf(st->error, sizeof st->error, c.failed ? "out of memory in pass 1"
                                                       : "parse error in pass 1 (readosm error %d)", rc);
        ctx_free(&c); free(c.places);
        return false;
    }
    st->way_refs = c.refs_n;
    if (!build_unique_ids(&c)) {
        snprintf(st->error, sizeof st->error, "out of memory building node index");
        ctx_free(&c); free(c.places);
        return false;
    }
    st->referenced_nodes = c.un;
    st->seconds_pass1 = now_s() - t0;
    if (opt->verbose)
        fprintf(stderr, "pass 1: %zu nodes, %zu ways seen, %zu routable ways, %zu referenced nodes (%.1fs)\n",
                st->nodes_seen, st->ways_seen, st->ways_routable, c.un, st->seconds_pass1);

    t0 = now_s();
    h = NULL;
    rc = readosm_open(path, &h);
    if (rc == READOSM_OK)
        rc = readosm_parse(h, &c, p2_node, opt->early_stop ? p2_way_stop : NULL, NULL);
    readosm_close(h);
    if (rc != READOSM_OK && rc != READOSM_ABORT) {
        snprintf(st->error, sizeof st->error, "parse error in pass 2 (readosm error %d)", rc);
        ctx_free(&c); free(c.places);
        return false;
    }
    st->seconds_pass2 = now_s() - t0;
    if (opt->verbose)
        fprintf(stderr, "pass 2: coordinates for %zu of %zu referenced nodes (%.1fs)\n",
                st->coords_found, c.un, st->seconds_pass2);

    t0 = now_s();
    graph_t *g = graph_create();
    if (!g || !build_graph(&c, g)) {
        if (!st->error[0]) snprintf(st->error, sizeof st->error, "graph build failed (out of memory?)");
        graph_free(g);
        ctx_free(&c); free(c.places);
        return false;
    }
    ctx_free(&c);
    graph_status_t gs = graph_finalize(g);
    if (gs != GRAPH_OK) {
        snprintf(st->error, sizeof st->error, "finalize failed: %s", graph_status_str(gs));
        graph_free(g); free(c.places);
        return false;
    }
    assign_nearest_towns(&c);
    st->seconds_build = now_s() - t0;
    out->graph = g;
    out->places = c.places;
    out->place_count = c.places_n;
    return true;
}

void import_result_free(import_result_t *r)
{
    if (!r) return;
    graph_free(r->graph);
    free(r->places);
    memset(r, 0, sizeof *r);
}
