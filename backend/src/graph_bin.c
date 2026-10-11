/*
 * graph_bin.c — binary graph writer/reader with validation (see graph_bin.h).
 */
#include "graph_bin.h"
#include "geospatial.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME  1099511628211ULL

typedef struct {
    FILE    *f;
    uint64_t hash;
    bool     ok;
} hfile_t;

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
    const unsigned char *b = p;
    uint64_t x = *h;
    for (size_t i = 0; i < n; i++) { x ^= b[i]; x *= FNV_PRIME; }
    *h = x;
}

static void hwrite(hfile_t *hf, const void *p, size_t elem, size_t count)
{
    if (!hf->ok || count == 0) return;
    if (fwrite(p, elem, count, hf->f) != count) { hf->ok = false; return; }
    hash_bytes(&hf->hash, p, elem * count);
}

static bool hread(hfile_t *hf, void *p, size_t elem, size_t count)
{
    if (!hf->ok) return false;
    if (count == 0) return true;
    if (fread(p, elem, count, hf->f) != count) { hf->ok = false; return false; }
    hash_bytes(&hf->hash, p, elem * count);
    return true;
}

graph_status_t graph_bin_write(const graph_t *g, const char *path)
{
    if (!g || !path || !g->finalized) return GRAPH_ERR_INVALID;

    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return GRAPH_ERR_INVALID;
    FILE *f = fopen(tmp, "wb");
    if (!f) return GRAPH_ERR_IO;

    graph_bin_header_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, GRAPH_BIN_MAGIC, 8);
    h.version = GRAPH_BIN_VERSION;
    h.endian_check = 0x01020304u;
    h.sizeof_node = (uint32_t)sizeof(graph_node_t);
    h.sizeof_edge = (uint32_t)sizeof(graph_edge_t);
    h.sizeof_point = (uint32_t)sizeof(graph_point_t);
    h.node_count = g->node_count;
    h.edge_count = g->edge_count;
    h.shape_count = g->shape_count;
    h.names_size = g->names_size;
    h.dataset_time = g->dataset_time;
    h.max_speed_mps = g->max_speed_mps;
    snprintf(h.dataset, sizeof h.dataset, "%s", g->dataset);

    /* Traffic overrides and closures are runtime state: never persist them. */
    hfile_t hf = { .f = f, .hash = FNV_OFFSET, .ok = true };
    hwrite(&hf, &h, sizeof h, 1);
    hwrite(&hf, g->nodes, sizeof *g->nodes, g->node_count);
    hwrite(&hf, g->node_flags, 1, g->node_count);
    hwrite(&hf, g->first_out, sizeof *g->first_out, g->node_count + 1);
    for (size_t i = 0; i < g->edge_count && hf.ok; i++) {
        graph_edge_t e = g->edges[i];
        e.traffic_time_s = 0.0f;
        e.flags &= (uint8_t)~EDGE_FLAG_CLOSED;
        hwrite(&hf, &e, sizeof e, 1);
    }
    hwrite(&hf, g->shape, sizeof *g->shape, g->shape_count);
    hwrite(&hf, g->names, 1, g->names_size);
    uint64_t sum = hf.hash;
    if (hf.ok && fwrite(&sum, sizeof sum, 1, f) != 1) hf.ok = false;
    if (fclose(f) != 0) hf.ok = false;

    if (!hf.ok) { remove(tmp); return GRAPH_ERR_IO; }
    /* rename() is atomic on POSIX filesystems; on Windows remove target first. */
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return GRAPH_ERR_IO; }
    return GRAPH_OK;
}

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* Allocates count*elem bytes with overflow check (returns NULL on 0 count
 * only if count is 0 — callers handle that). */
static void *alloc_array(size_t count, size_t elem)
{
    if (count == 0) return malloc(1);
    if (count > SIZE_MAX / elem) return NULL;
    return malloc(count * elem);
}

graph_t *graph_bin_read(const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, err_len, "cannot open %s: %s", path, strerror(errno)); return NULL; }

    graph_t *g = graph_create();
    hfile_t hf = { .f = f, .hash = FNV_OFFSET, .ok = true };
    graph_bin_header_t h;
    if (!g) { set_err(err, err_len, "out of memory"); fclose(f); return NULL; }

    if (!hread(&hf, &h, sizeof h, 1)) { set_err(err, err_len, "truncated header"); goto fail; }
    if (memcmp(h.magic, GRAPH_BIN_MAGIC, 8) != 0) { set_err(err, err_len, "not a graph.bin file"); goto fail; }
    if (h.version != GRAPH_BIN_VERSION) {
        set_err(err, err_len, "graph.bin version %u, expected %u (re-run the importer)",
                h.version, GRAPH_BIN_VERSION);
        goto fail;
    }
    if (h.endian_check != 0x01020304u || h.sizeof_node != sizeof(graph_node_t) ||
        h.sizeof_edge != sizeof(graph_edge_t) || h.sizeof_point != sizeof(graph_point_t)) {
        set_err(err, err_len, "graph.bin was built on an incompatible platform");
        goto fail;
    }
    if (h.node_count == 0 || h.node_count >= IRA_INVALID_IDX ||
        h.edge_count >= IRA_INVALID_IDX || h.shape_count > UINT32_MAX ||
        h.names_size == 0 || h.names_size > UINT32_MAX) {
        set_err(err, err_len, "graph.bin header has invalid counts");
        goto fail;
    }

    size_t V = (size_t)h.node_count, E = (size_t)h.edge_count;
    size_t S = (size_t)h.shape_count, N = (size_t)h.names_size;
    free(g->names);
    g->nodes      = alloc_array(V, sizeof *g->nodes);
    g->node_flags = alloc_array(V, 1);
    g->first_out  = alloc_array(V + 1, sizeof *g->first_out);
    g->edges      = alloc_array(E, sizeof *g->edges);
    g->shape      = alloc_array(S, sizeof *g->shape);
    g->names      = alloc_array(N, 1);
    if (!g->nodes || !g->node_flags || !g->first_out || !g->edges || !g->shape || !g->names) {
        set_err(err, err_len, "out of memory loading graph (%zu nodes, %zu edges)", V, E);
        goto fail;
    }
    g->node_count = g->node_cap = V;
    g->edge_count = g->edge_cap = E;
    g->shape_count = g->shape_cap = S;
    g->names_size = g->names_cap = N;

    if (!hread(&hf, g->nodes, sizeof *g->nodes, V) ||
        !hread(&hf, g->node_flags, 1, V) ||
        !hread(&hf, g->first_out, sizeof *g->first_out, V + 1) ||
        !hread(&hf, g->edges, sizeof *g->edges, E) ||
        !hread(&hf, g->shape, sizeof *g->shape, S) ||
        !hread(&hf, g->names, 1, N)) {
        set_err(err, err_len, "graph.bin is truncated");
        goto fail;
    }
    uint64_t stored;
    if (fread(&stored, sizeof stored, 1, f) != 1 || stored != hf.hash) {
        set_err(err, err_len, "graph.bin checksum mismatch (file corrupted)");
        goto fail;
    }

    /* Structural validation: never trust offsets read from disk. */
    if (g->first_out[0] != 0 || g->first_out[V] != E) {
        set_err(err, err_len, "invalid CSR offsets"); goto fail;
    }
    for (size_t v = 0; v < V; v++) {
        if (g->first_out[v] > g->first_out[v + 1]) { set_err(err, err_len, "invalid CSR offsets"); goto fail; }
        if (!geo_valid_coord(g->nodes[v].lat, g->nodes[v].lon)) {
            set_err(err, err_len, "invalid coordinate at node %zu", v); goto fail;
        }
        for (edge_idx_t e = g->first_out[v]; e < g->first_out[v + 1]; e++)
            if (g->edges[e].from != v) { set_err(err, err_len, "edge %u not sorted", e); goto fail; }
    }
    if (g->names[N - 1] != '\0' || g->names[0] != '\0') {
        set_err(err, err_len, "invalid names table"); goto fail;
    }
    for (size_t i = 0; i < E; i++) {
        graph_edge_t *e = &g->edges[i];
        if (e->to >= V || !(e->distance_m > 0.0f) || !(e->base_time_s > 0.0f) ||
            !isfinite(e->distance_m) || !isfinite(e->base_time_s) ||
            e->road_class >= ROAD_CLASS_COUNT || e->name_id >= N ||
            (uint64_t)e->geom_offset + e->geom_count > S) {
            set_err(err, err_len, "invalid edge record %zu", i); goto fail;
        }
        e->traffic_time_s = 0.0f;
        e->flags &= (uint8_t)~EDGE_FLAG_CLOSED;
    }

    fclose(f);
    g->finalized = true;
    memcpy(g->dataset, h.dataset, sizeof g->dataset);
    g->dataset[sizeof g->dataset - 1] = '\0';
    g->dataset_time = h.dataset_time;
    graph_recompute_max_speed(g);
    g->min_lat = g->min_lon = INFINITY;
    g->max_lat = g->max_lon = -INFINITY;
    for (size_t i = 0; i < V; i++) {
        if (g->nodes[i].lat < g->min_lat) g->min_lat = g->nodes[i].lat;
        if (g->nodes[i].lat > g->max_lat) g->max_lat = g->nodes[i].lat;
        if (g->nodes[i].lon < g->min_lon) g->min_lon = g->nodes[i].lon;
        if (g->nodes[i].lon > g->max_lon) g->max_lon = g->nodes[i].lon;
    }
    return g;

fail:
    fclose(f);
    graph_free(g);
    return NULL;
}
