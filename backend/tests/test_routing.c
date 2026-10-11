/*
 * Routing behaviour on the documented sample network (data/sample) and
 * randomized cross-checks:
 *   - Dijkstra vs an independent Bellman-Ford oracle
 *   - A* vs Dijkstra (same optimal cost, fewer or equal settled nodes)
 *
 * Expected sample routes are derived by hand in data/README.md.
 */
#include "routing.h"
#include "graph_io.h"
#include "geospatial.h"
#include "test_util.h"

#include <string.h>

#ifndef SAMPLE_DIR
#define SAMPLE_DIR "../data/sample"
#endif

static graph_t *G;   /* sample graph, reloaded per test so state never leaks */

static void load(void)
{
    graph_load_report_t rep;
    G = graph_load_csv(SAMPLE_DIR "/nodes.csv", SAMPLE_DIR "/edges.csv", &rep);
    if (!G) { fprintf(stderr, "cannot load sample: %s\n", rep.error); exit(1); }
}
static void unload(void) { graph_free(G); G = NULL; }

/* Routes between external node ids with both algorithms, checks they agree,
 * and compares the node sequence with `expect` (terminated by 0). */
static void expect_route(int64_t from, int64_t to, cost_mode_t mode,
                         bool avoid_tolls, bool avoid_restricted,
                         const int64_t *expect)
{
    double costs[2] = {0, 0};
    for (int algo = 0; algo < 2; algo++) {
        route_request_t r = {
            .source = graph_find_node(G, from), .target = graph_find_node(G, to),
            .mode = mode, .algorithm = (algorithm_t)algo,
            .avoid_tolls = avoid_tolls, .avoid_restricted = avoid_restricted,
        };
        route_result_t res;
        route_status_t st = route_compute(G, &r, &res);
        if (!expect) {
            CHECK(st == ROUTE_ERR_NO_PATH);
        } else {
            CHECK(st == ROUTE_OK);
            size_t n = 0;
            while (expect[n]) n++;
            CHECK(res.node_count == n);
            for (size_t i = 0; i < n && i < res.node_count; i++)
                CHECK(G->nodes[res.nodes[i]].id == expect[i]);
            costs[algo] = res.total_cost;
            /* totals consistent with the selected mode */
            CHECK_NEAR(res.total_cost, mode == COST_TIME ? res.duration_s : res.distance_m, 1e-6);
        }
        route_result_free(&res);
    }
    if (expect) CHECK_NEAR(costs[0], costs[1], 1e-6);
}

static void test_shortest_vs_fastest(void)
{
    load();
    /* Shortest distance: straight along the residential road A-B-C-D. */
    expect_route(1, 4, COST_DISTANCE, false, false, (int64_t[]){1, 2, 3, 4, 0});
    /* Fastest: longer but quicker trunk road via E and F. */
    expect_route(1, 4, COST_TIME, false, false, (int64_t[]){1, 5, 6, 4, 0});
    unload();
}

static void test_avoid_tolls(void)
{
    load();
    /* E-F is tolled; next fastest uses one-way secondary B->H->D. */
    expect_route(1, 4, COST_TIME, true, false, (int64_t[]){1, 2, 8, 4, 0});
    unload();
}

static void test_one_way_respected(void)
{
    load();
    /* D->A cannot use the one-way B->H->D road backwards. */
    expect_route(4, 1, COST_TIME, true, false, (int64_t[]){4, 3, 2, 1, 0});
    /* D->H: H is only enterable from B, so the route must detour D-C-B-H
     * instead of taking the (one-way, opposite direction) edge H->D. */
    expect_route(4, 8, COST_DISTANCE, false, false, (int64_t[]){4, 3, 2, 8, 0});
    expect_route(4, 8, COST_TIME, false, false, (int64_t[]){4, 3, 2, 8, 0});
    unload();
}

static void test_traffic_update_changes_route(void)
{
    load();
    expect_route(1, 4, COST_TIME, false, false, (int64_t[]){1, 5, 6, 4, 0});
    /* Simulated congestion on E->F: 1200 s instead of ~247 s. */
    CHECK(graph_set_traffic_time(G, graph_find_edge(G, 5), 1200.0) == GRAPH_OK);
    expect_route(1, 4, COST_TIME, false, false, (int64_t[]){1, 2, 8, 4, 0});
    /* Distance mode is unaffected by travel-time changes. */
    expect_route(1, 4, COST_DISTANCE, false, false, (int64_t[]){1, 2, 3, 4, 0});
    graph_clear_traffic(G);
    expect_route(1, 4, COST_TIME, false, false, (int64_t[]){1, 5, 6, 4, 0});
    unload();
}

static void test_closure_blocks_segment(void)
{
    load();
    edge_idx_t e9 = graph_find_edge(G, 9);
    CHECK(graph_set_closed(G, e9, true) == GRAPH_OK);
    expect_route(1, 4, COST_TIME, true, false, (int64_t[]){1, 2, 3, 4, 0});
    /* closure is respected even with no avoid options */
    route_request_t r = { .source = graph_find_node(G, 2),
                          .target = graph_find_node(G, 8), .mode = COST_TIME };
    route_result_t res;
    CHECK(route_compute(G, &r, &res) == ROUTE_ERR_NO_PATH);
    route_result_free(&res);
    graph_set_closed(G, e9, false);
    expect_route(1, 4, COST_TIME, true, false, (int64_t[]){1, 2, 8, 4, 0});
    unload();
}

static void test_restricted_roads(void)
{
    load();
    expect_route(7, 3, COST_DISTANCE, false, false, (int64_t[]){7, 3, 0});
    expect_route(7, 3, COST_DISTANCE, false, true, (int64_t[]){7, 2, 3, 0});
    unload();
}

static void test_disconnected_component(void)
{
    load();
    expect_route(1, 9, COST_TIME, false, false, NULL);
    expect_route(9, 10, COST_DISTANCE, false, false, (int64_t[]){9, 10, 0});
    unload();
}

/* ---------- randomized cross-checks ---------- */

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }
static double rnd01(void) { return (double)(rnd() & 0xFFFF) / 65535.0; }

/* Random geometric graph in a ~50 km box: edge length = straight line * [1,1.5],
 * random road class speed, ~10% one-way, ~5% closed, ~10% toll. */
static graph_t *random_graph(size_t V, size_t E)
{
    graph_t *g = graph_create();
    for (size_t i = 0; i < V; i++)
        graph_add_node(g, (int64_t)i, 17.0 + 0.45 * rnd01(), 79.0 + 0.45 * rnd01(), NULL);
    for (size_t i = 0; i < E; i++) {
        node_idx_t a = (node_idx_t)(rnd() % V), b = (node_idx_t)(rnd() % V);
        if (a == b) continue;
        const graph_node_t *na = &g->nodes[a], *nb = &g->nodes[b];
        double d = geo_haversine_m(na->lat, na->lon, nb->lat, nb->lon) * (1.0 + 0.5 * rnd01());
        if (d <= 0) continue;
        road_class_t rc = (road_class_t)(rnd() % ROAD_CLASS_COUNT);
        double t = d / (road_class_default_speed_kmh(rc) / 3.6);
        uint32_t flags = (rnd() % 10 == 0) ? EDGE_FLAG_TOLL : 0;
        graph_add_edge(g, (int64_t)i + 1, a, b, d, t, rc, 0, flags);
        if (rnd() % 10 != 0) graph_add_edge(g, -(int64_t)i - 1, b, a, d, t, rc, 0, flags);
    }
    graph_finalize(g);
    for (size_t i = 0; i < g->edge_count; i++) {
        if (rnd() % 20 == 0) graph_set_closed(g, (edge_idx_t)i, true);
        if (rnd() % 15 == 0)   /* random traffic, sometimes faster than baseline */
            graph_set_traffic_time(g, (edge_idx_t)i, g->edges[i].base_time_s * (0.7 + 2.0 * rnd01()));
    }
    return g;
}

/* Independent oracle: Bellman-Ford, O(V*E), no heap involved. */
static double bellman_ford(const graph_t *g, const route_request_t *r)
{
    size_t V = g->node_count;
    double *d = malloc(V * sizeof *d);
    for (size_t i = 0; i < V; i++) d[i] = INFINITY;
    d[r->source] = 0;
    for (size_t it = 0; it + 1 < V; it++) {
        bool changed = false;
        for (size_t i = 0; i < g->edge_count; i++) {
            const graph_edge_t *e = &g->edges[i];
            if (!route_edge_usable(e, r) || isinf(d[e->from])) continue;
            double nd = d[e->from] + route_edge_cost(e, r->mode);
            if (nd < d[e->to]) { d[e->to] = nd; changed = true; }
        }
        if (!changed) break;
    }
    double out = d[r->target];
    free(d);
    return out;
}

static void test_random_against_bellman_ford_and_astar(void)
{
    int mismatches = 0, astar_worse_effort = 0, runs = 0;
    size_t settled_dij = 0, settled_ast = 0;
    for (int gi = 0; gi < 40; gi++) {
        graph_t *g = random_graph(60 + (size_t)(rnd() % 140), 200 + (size_t)(rnd() % 400));
        for (int q = 0; q < 5; q++) {
            route_request_t r = {
                .source = (node_idx_t)(rnd() % g->node_count),
                .target = (node_idx_t)(rnd() % g->node_count),
                .mode = (q & 1) ? COST_TIME : COST_DISTANCE,
                .avoid_tolls = (q == 4),
            };
            double oracle = bellman_ford(g, &r);
            route_result_t a, b;
            r.algorithm = ALGO_DIJKSTRA; route_status_t sa = route_compute(g, &r, &a);
            r.algorithm = ALGO_ASTAR;    route_status_t sb = route_compute(g, &r, &b);
            runs++;
            if (isinf(oracle)) {
                if (sa != ROUTE_ERR_NO_PATH || sb != ROUTE_ERR_NO_PATH) mismatches++;
            } else {
                double tol = 1e-6 * (1.0 + oracle);
                if (sa != ROUTE_OK || sb != ROUTE_OK ||
                    fabs(a.total_cost - oracle) > tol || fabs(b.total_cost - oracle) > tol)
                    mismatches++;
                if (b.nodes_settled > a.nodes_settled) astar_worse_effort++;
                settled_dij += a.nodes_settled;
                settled_ast += b.nodes_settled;
            }
            route_result_free(&a);
            route_result_free(&b);
        }
        graph_free(g);
    }
    CHECK(mismatches == 0);
    CHECK(astar_worse_effort == 0);
    printf("    (%d random queries; nodes settled on routable queries: "
           "Dijkstra %zu, A* %zu)\n", runs, settled_dij, settled_ast);
}

int main(void)
{
    printf("routing tests\n");
    RUN(test_shortest_vs_fastest);
    RUN(test_avoid_tolls);
    RUN(test_one_way_respected);
    RUN(test_traffic_update_changes_route);
    RUN(test_closure_blocks_segment);
    RUN(test_restricted_roads);
    RUN(test_disconnected_component);
    RUN(test_random_against_bellman_ford_and_astar);
    TEST_SUMMARY();
}
