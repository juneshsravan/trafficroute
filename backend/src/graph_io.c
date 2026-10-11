/*
 * graph_io.c — CSV test-network loader (format documented in graph_io.h).
 *
 * Parsing is deliberately strict: every numeric field is checked with
 * strtod/strtoll end pointers, line length is bounded, and any malformed line
 * aborts the load with a message naming the file and line number.
 */
#include "graph_io.h"
#include "geospatial.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE   1024
#define MAX_FIELDS 12

typedef struct { int64_t id; node_idx_t idx; } id_map_entry_t;

static void set_err(graph_load_report_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof r->error, fmt, ap);
    va_end(ap);
}

/* Splits line in place on commas; trims trailing CR/LF. Returns field count,
 * or -1 if there are more than max fields. */
static int split_csv(char *line, char **fields, int max)
{
    line[strcspn(line, "\r\n")] = '\0';
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return -1;
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static bool skip_line(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    return *line == '#' || *line == '\0' || *line == '\n' || *line == '\r';
}

static bool parse_double(const char *s, double *out)
{
    if (!s || !*s) return false;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_i64(const char *s, int64_t *out)
{
    if (!s || !*s) return false;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

/* Optional numeric field: empty -> default. */
static bool parse_double_opt(const char *s, double def, double *out)
{
    if (!s || !*s) { *out = def; return true; }
    return parse_double(s, out);
}

static bool parse_bool01(const char *s, bool *out)
{
    if (!s || !*s || strcmp(s, "0") == 0) { *out = false; return true; }
    if (strcmp(s, "1") == 0) { *out = true; return true; }
    return false;
}

static int cmp_id(const void *a, const void *b)
{
    int64_t x = ((const id_map_entry_t *)a)->id, y = ((const id_map_entry_t *)b)->id;
    return (x > y) - (x < y);
}

static node_idx_t lookup(const id_map_entry_t *map, size_t n, int64_t id)
{
    id_map_entry_t key = { .id = id };
    const id_map_entry_t *hit = bsearch(&key, map, n, sizeof *map, cmp_id);
    return hit ? hit->idx : IRA_INVALID_IDX;
}

static bool load_nodes(graph_t *g, const char *path, graph_load_report_t *r)
{
    FILE *f = fopen(path, "r");
    if (!f) { set_err(r, "cannot open %s: %s", path, strerror(errno)); return false; }

    char line[MAX_LINE];
    char *fld[MAX_FIELDS];
    size_t lineno = 0;
    bool header_seen = false, ok = true;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        r->lines_read++;
        if (!strchr(line, '\n') && !feof(f)) {
            set_err(r, "%s:%zu: line too long", path, lineno); ok = false; break;
        }
        if (skip_line(line)) continue;
        if (!header_seen) { header_seen = true; continue; }

        int n = split_csv(line, fld, MAX_FIELDS);
        int64_t id; double lat, lon;
        if (n < 3 || !parse_i64(fld[0], &id) || !parse_double(fld[1], &lat) ||
            !parse_double(fld[2], &lon)) {
            set_err(r, "%s:%zu: expected id,lat,lon[,name]", path, lineno);
            ok = false; break;
        }
        graph_status_t st = graph_add_node(g, id, lat, lon, NULL);
        if (st != GRAPH_OK) {
            set_err(r, "%s:%zu: node %lld: %s", path, lineno, (long long)id,
                    graph_status_str(st));
            ok = false; break;
        }
        r->nodes_loaded++;
    }
    fclose(f);
    return ok;
}

static bool load_edges(graph_t *g, const char *path, const id_map_entry_t *map,
                       graph_load_report_t *r)
{
    FILE *f = fopen(path, "r");
    if (!f) { set_err(r, "cannot open %s: %s", path, strerror(errno)); return false; }

    char line[MAX_LINE];
    char *fld[MAX_FIELDS];
    size_t lineno = 0;
    bool header_seen = false, ok = true;

    while (ok && fgets(line, sizeof line, f)) {
        lineno++;
        r->lines_read++;
        if (!strchr(line, '\n') && !feof(f)) {
            set_err(r, "%s:%zu: line too long", path, lineno); ok = false; break;
        }
        if (skip_line(line)) continue;
        if (!header_seen) { header_seen = true; continue; }

        int n = split_csv(line, fld, MAX_FIELDS);
        if (n < 9) {
            set_err(r, "%s:%zu: expected 9 fields "
                    "(id,from,to,distance_m,road_class,speed_kmh,oneway,toll,restricted)",
                    path, lineno);
            ok = false; break;
        }

        int64_t id, from_id, to_id;
        double dist, speed;
        road_class_t rc;
        bool oneway, toll, restricted;
        if (!parse_i64(fld[0], &id) || !parse_i64(fld[1], &from_id) ||
            !parse_i64(fld[2], &to_id) || !parse_double_opt(fld[3], 0.0, &dist) ||
            !road_class_from_name(fld[4], &rc) ||
            !parse_double_opt(fld[5], 0.0, &speed) ||
            !parse_bool01(fld[6], &oneway) || !parse_bool01(fld[7], &toll) ||
            !parse_bool01(fld[8], &restricted) || dist < 0 || speed < 0) {
            set_err(r, "%s:%zu: malformed edge record", path, lineno);
            ok = false; break;
        }
        if (id <= 0) {   /* negative ids are reserved for generated reverse edges */
            set_err(r, "%s:%zu: edge id must be positive", path, lineno);
            ok = false; break;
        }

        node_idx_t a = lookup(map, g->node_count, from_id);
        node_idx_t b = lookup(map, g->node_count, to_id);
        if (a == IRA_INVALID_IDX || b == IRA_INVALID_IDX) {
            set_err(r, "%s:%zu: edge %lld references unknown node", path, lineno,
                    (long long)id);
            ok = false; break;
        }
        if (a == b) {
            set_err(r, "%s:%zu: edge %lld is a self-loop", path, lineno, (long long)id);
            ok = false; break;
        }

        /* A road can never be shorter than the straight line between its ends.
         * Enforcing this keeps the A* heuristic admissible. */
        const graph_node_t *na = &g->nodes[a], *nb = &g->nodes[b];
        double straight = geo_haversine_m(na->lat, na->lon, nb->lat, nb->lon);
        if (dist == 0.0) {
            dist = straight;
        } else if (dist < straight) {
            dist = straight;
            r->distances_clamped++;
        }
        if (dist <= 0.0) {
            set_err(r, "%s:%zu: edge %lld has zero length", path, lineno, (long long)id);
            ok = false; break;
        }

        double speed_kmh = speed > 0.0 ? speed : road_class_default_speed_kmh(rc);
        double time_s = dist / (speed_kmh / 3.6);     /* m / (m/s) = s */
        uint32_t flags = (toll ? EDGE_FLAG_TOLL : 0) |
                         (restricted ? EDGE_FLAG_RESTRICTED : 0);

        const char *name = n >= 10 ? fld[9] : NULL;
        graph_status_t st = graph_add_edge_ex(g, id, a, b, dist, time_s, rc,
                                              speed > 0.0 ? speed : 0.0, flags,
                                              name, NULL, 0);
        if (st == GRAPH_OK) r->edges_loaded++;
        if (st == GRAPH_OK && !oneway) {
            st = graph_add_edge_ex(g, -id, b, a, dist, time_s, rc,
                                   speed > 0.0 ? speed : 0.0, flags, name, NULL, 0);
            if (st == GRAPH_OK) r->edges_loaded++;
        }
        if (st != GRAPH_OK) {
            set_err(r, "%s:%zu: edge %lld: %s", path, lineno, (long long)id,
                    graph_status_str(st));
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

graph_t *graph_load_csv(const char *nodes_path, const char *edges_path,
                        graph_load_report_t *report)
{
    graph_load_report_t dummy;
    graph_load_report_t *r = report ? report : &dummy;
    memset(r, 0, sizeof *r);

    graph_t *g = graph_create();
    id_map_entry_t *map = NULL;
    if (!g) { set_err(r, "out of memory"); return NULL; }

    if (!load_nodes(g, nodes_path, r)) goto fail;
    if (g->node_count == 0) { set_err(r, "%s: no nodes", nodes_path); goto fail; }

    /* Sorted id -> index map: O(N log N) build, O(log N) lookups, and a cheap
     * duplicate check. */
    map = malloc(g->node_count * sizeof *map);
    if (!map) { set_err(r, "out of memory"); goto fail; }
    for (size_t i = 0; i < g->node_count; i++)
        map[i] = (id_map_entry_t){ g->nodes[i].id, (node_idx_t)i };
    qsort(map, g->node_count, sizeof *map, cmp_id);
    for (size_t i = 1; i < g->node_count; i++) {
        if (map[i].id == map[i - 1].id) {
            set_err(r, "%s: duplicate node id %lld", nodes_path, (long long)map[i].id);
            goto fail;
        }
    }

    if (!load_edges(g, edges_path, map, r)) goto fail;

    graph_status_t st = graph_finalize(g);
    if (st != GRAPH_OK) { set_err(r, "finalize: %s", graph_status_str(st)); goto fail; }
    snprintf(g->dataset, sizeof g->dataset, "csv:%s", edges_path);
    /* CSV test networks have no connectivity analysis; treat every node as
     * part of the main component so snapping works on them. */
    for (size_t i = 0; i < g->node_count; i++) g->node_flags[i] |= NODE_FLAG_MAIN_COMPONENT;

    free(map);
    return g;

fail:
    free(map);
    graph_free(g);
    return NULL;
}
