/*
 * bench_route.c — measure route latency and memory on a documented dataset.
 *
 *   bench_route --graph FILE.graph.bin [--queries N] [--seed S]
 *   bench_route --csv NODES.csv EDGES.csv
 *   bench_route --grid N        synthetic N x N grid (clearly labelled synthetic)
 *
 * For every random query between two nodes of the main connected component
 * it runs Dijkstra and A* (fastest mode), checks they return the same cost,
 * and reports p50/p95/max latency and nodes settled. Exits non-zero on any
 * mismatch or on a failed route inside the strongly connected component —
 * so it doubles as a correctness check on real imported data.
 */
#define _POSIX_C_SOURCE 200809L
#include "graph_bin.h"
#include "graph_io.h"
#include "geospatial.h"
#include "routing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmRSS:", 6) == 0) { kb = strtol(line + 6, NULL, 10); break; }
    fclose(f);
    return kb;
}

static uint64_t rng = 88172645463325252ULL;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static graph_t *make_grid(size_t n)
{
    graph_t *g = graph_create();
    const double step = 0.001;                     /* ~110 m */
    for (size_t r = 0; r < n; r++)
        for (size_t c = 0; c < n; c++)
            graph_add_node(g, (int64_t)(r * n + c), 20.0 + (double)r * step, 78.0 + (double)c * step, NULL);
    int64_t id = 1;
    for (size_t r = 0; r < n; r++)
        for (size_t c = 0; c < n; c++) {
            node_idx_t v = (node_idx_t)(r * n + c);
            node_idx_t nb[2] = { c + 1 < n ? v + 1 : IRA_INVALID_IDX,
                                 r + 1 < n ? (node_idx_t)(v + n) : IRA_INVALID_IDX };
            for (int k = 0; k < 2; k++) {
                if (nb[k] == IRA_INVALID_IDX) continue;
                const graph_node_t *a = &g->nodes[v], *b = &g->nodes[nb[k]];
                double d = geo_haversine_m(a->lat, a->lon, b->lat, b->lon) * (1.0 + (double)(xr() % 200) / 1000.0);
                road_class_t rc = (road_class_t)(xr() % 7);
                double t = d / (road_class_default_speed_kmh(rc) / 3.6);
                graph_add_edge(g, id, v, nb[k], d, t, rc, 0, 0);
                graph_add_edge(g, -id, nb[k], v, d, t, rc, 0, 0);
                id++;
            }
        }
    graph_finalize(g);
    for (size_t i = 0; i < g->node_count; i++) g->node_flags[i] |= NODE_FLAG_MAIN_COMPONENT;
    snprintf(g->dataset, sizeof g->dataset, "synthetic grid %zux%zu", n, n);
    return g;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *name, double *t, size_t n, double settled)
{
    if (!n) return;
    qsort(t, n, sizeof *t, cmp_d);
    printf("  %-9s p50 %8.2f ms   p95 %8.2f ms   max %8.2f ms   mean settled %.0f\n",
           name, t[n / 2], t[(size_t)((double)n * 0.95) < n ? (size_t)((double)n * 0.95) : n - 1],
           t[n - 1], settled / (double)n);
}

int main(int argc, char **argv)
{
    const char *graph_path = NULL, *nodes = NULL, *edges = NULL;
    size_t grid = 0, queries = 200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--graph") && i + 1 < argc) graph_path = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 2 < argc) { nodes = argv[++i]; edges = argv[++i]; }
        else if (!strcmp(argv[i], "--grid") && i + 1 < argc) grid = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--queries") && i + 1 < argc) queries = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rng = strtoull(argv[++i], NULL, 10) | 1;
        else { fprintf(stderr, "usage: bench_route (--graph F | --csv N E | --grid N) [--queries Q] [--seed S]\n"); return 2; }
    }
    long rss0 = rss_kb();
    double t0 = now_ms();
    graph_t *g = NULL;
    char err[256] = "";
    if (graph_path) g = graph_bin_read(graph_path, err, sizeof err);
    else if (nodes) { graph_load_report_t r; g = graph_load_csv(nodes, edges, &r); if (!g) snprintf(err, sizeof err, "%s", r.error); }
    else if (grid >= 2 && grid <= 5000) g = make_grid(grid);
    if (!g) { fprintf(stderr, "no graph: %s\n", err[0] ? err : "give --graph, --csv or --grid 2..5000"); return 2; }
    double load_ms = now_ms() - t0;

    size_t nm = 0;
    for (size_t i = 0; i < g->node_count; i++) nm += (g->node_flags[i] & NODE_FLAG_MAIN_COMPONENT) != 0;
    node_idx_t *main_nodes = malloc((nm ? nm : 1) * sizeof *main_nodes);
    size_t k = 0;
    for (size_t i = 0; i < g->node_count; i++)
        if (g->node_flags[i] & NODE_FLAG_MAIN_COMPONENT) main_nodes[k++] = (node_idx_t)i;

    route_workspace_t *ws = route_workspace_create(g->node_count);
    double *td = malloc(queries * sizeof *td), *ta = malloc(queries * sizeof *ta);
    double sd = 0, sa = 0, dist_sum = 0;
    size_t done = 0, mismatches = 0, failures = 0;
    for (size_t q = 0; q < queries && nm >= 2; q++) {
        route_request_t rq = { .source = main_nodes[xr() % nm], .target = main_nodes[xr() % nm],
                               .mode = COST_TIME };
        route_result_t a, b;
        double s = now_ms();
        rq.algorithm = ALGO_DIJKSTRA;
        route_status_t s1 = route_compute_ws(g, &rq, ws, &a);
        td[done] = now_ms() - s;
        s = now_ms();
        rq.algorithm = ALGO_ASTAR;
        route_status_t s2 = route_compute_ws(g, &rq, ws, &b);
        ta[done] = now_ms() - s;
        if (s1 != ROUTE_OK || s2 != ROUTE_OK) failures++;
        else {
            if (fabs(a.total_cost - b.total_cost) > 1e-6 * (1 + a.total_cost)) mismatches++;
            sd += (double)a.nodes_settled;
            sa += (double)b.nodes_settled;
            dist_sum += a.distance_m;
            done++;
        }
        route_result_free(&a);
        route_result_free(&b);
    }

    printf("Dataset: %s\n", g->dataset);
    printf("  nodes %zu, directed edges %zu, shape points %zu, main component %zu (%.1f%%)\n",
           g->node_count, g->edge_count, g->shape_count, nm, 100.0 * (double)nm / (double)g->node_count);
    printf("  load/build %.0f ms; graph memory %.1f MB; one search workspace %.1f MB\n", load_ms,
           (double)graph_memory_bytes(g) / 1048576.0, (double)g->node_count * 37.0 / 1048576.0);
    long rss1 = rss_kb();
    if (rss0 >= 0 && rss1 >= 0) printf("  process RSS %.1f MB (start %.1f MB)\n", (double)rss1 / 1024.0, (double)rss0 / 1024.0);
    printf("  %zu queries (fastest mode), mean route length %.1f km\n", done, done ? dist_sum / (double)done / 1000.0 : 0);
    report("Dijkstra", td, done, sd);
    report("A*", ta, done, sa);
    printf("  Dijkstra/A* cost mismatches: %zu; failed routes inside main component: %zu\n", mismatches, failures);

    free(td); free(ta); free(main_nodes);
    route_workspace_free(ws);
    graph_free(g);
    return (mismatches || failures) ? 1 : 0;
}
