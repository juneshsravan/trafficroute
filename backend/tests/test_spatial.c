/* Spatial index vs brute force on random graphs. */
#include "spatial_index.h"
#include "geospatial.h"
#include "test_util.h"

static uint32_t rs = 777;
static double rnd01(void) { rs = rs * 1103515245u + 12345u; return (double)((rs >> 8) & 0xFFFF) / 65535.0; }

static graph_t *random_graph(size_t V, size_t E, double lat0, double lon0, double span)
{
    graph_t *g = graph_create();
    for (size_t i = 0; i < V; i++)
        graph_add_node(g, (int64_t)i, lat0 + span * rnd01(), lon0 + span * rnd01(), NULL);
    for (size_t i = 0; i < E; i++) {
        node_idx_t a = (node_idx_t)(rnd01() * (double)(V - 1)), b = (node_idx_t)(rnd01() * (double)(V - 1));
        if (a == b) continue;
        double d = geo_haversine_m(g->nodes[a].lat, g->nodes[a].lon, g->nodes[b].lat, g->nodes[b].lon);
        if (d <= 0) continue;
        graph_point_t mid = { (float)((g->nodes[a].lat + g->nodes[b].lat) / 2 + 0.001 * (rnd01() - 0.5)),
                              (float)((g->nodes[a].lon + g->nodes[b].lon) / 2) };
        graph_add_edge_ex(g, (int64_t)i + 1, a, b, d * 1.2, d, ROAD_PRIMARY, 0, 0, NULL, &mid, 1);
    }
    graph_finalize(g);
    for (size_t i = 0; i < g->node_count; i++) g->node_flags[i] |= NODE_FLAG_MAIN_COMPONENT;
    return g;
}

static void test_nearest_node_matches_brute_force(void)
{
    graph_t *g = random_graph(800, 1500, 17.0, 79.0, 0.3);
    spatial_index_t *si = spatial_index_build(g);
    CHECK(si != NULL);
    bool *has = calloc(g->node_count, 1);
    for (size_t i = 0; i < g->edge_count; i++) { has[g->edges[i].from] = 1; has[g->edges[i].to] = 1; }
    int bad = 0;
    for (int q = 0; q < 300; q++) {
        double lat = 16.98 + 0.34 * rnd01(), lon = 78.98 + 0.34 * rnd01();
        double best = INFINITY;
        for (size_t i = 0; i < g->node_count; i++) {
            if (!has[i]) continue;
            double d = geo_haversine_m(lat, lon, g->nodes[i].lat, g->nodes[i].lon);
            if (d < best) best = d;
        }
        node_idx_t n; double d;
        bool ok = spatial_snap_node(si, g, lat, lon, 50000, &n, &d);
        if (!ok || fabs(d - best) > 1e-6) bad++;
    }
    CHECK(bad == 0);
    /* nothing within a tiny radius far from the graph */
    node_idx_t n; double d;
    CHECK(!spatial_snap_node(si, g, 10.0, 70.0, 1000, &n, &d));
    free(has);
    spatial_index_free(si);
    graph_free(g);
}

static void test_nearest_edge_matches_brute_force(void)
{
    graph_t *g = random_graph(300, 500, 12.9, 77.5, 0.05);
    spatial_index_t *si = spatial_index_build(g);
    int bad = 0;
    for (int q = 0; q < 200; q++) {
        double lat = 12.9 + 0.05 * rnd01(), lon = 77.5 + 0.05 * rnd01();
        double best = INFINITY;
        for (size_t i = 0; i < g->edge_count; i++) {
            double d = spatial_point_edge_distance(g, (edge_idx_t)i, lat, lon);
            if (d < best) best = d;
        }
        edge_idx_t e; double d;
        bool ok = spatial_nearest_edge(si, g, lat, lon, 2000, &e, &d);
        if (best <= 2000 && (!ok || fabs(d - best) > 1e-6)) bad++;
    }
    CHECK(bad == 0);
    spatial_index_free(si);
    graph_free(g);
}

static void test_main_component_preferred(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b, c, d;
    graph_add_node(g, 1, 17.0, 79.0, &a);
    graph_add_node(g, 2, 17.0, 79.001, &b);
    graph_add_node(g, 3, 17.01, 79.0, &c);
    graph_add_node(g, 4, 17.01, 79.001, &d);
    graph_add_edge(g, 1, a, b, 200, 20, ROAD_PRIMARY, 0, 0);
    graph_add_edge(g, 2, c, d, 200, 20, ROAD_PRIMARY, 0, 0);
    graph_finalize(g);
    g->node_flags[c] |= NODE_FLAG_MAIN_COMPONENT;
    g->node_flags[d] |= NODE_FLAG_MAIN_COMPONENT;
    spatial_index_t *si = spatial_index_build(g);
    node_idx_t n; double dist;
    /* query right on node a: main-component node c (1.1 km away) is preferred */
    CHECK(spatial_snap_node(si, g, 17.0, 79.0, 5000, &n, &dist) && n == c);
    /* if main component is out of range, fall back to the nearest node */
    CHECK(spatial_snap_node(si, g, 17.0, 79.0, 500, &n, &dist) && n == a);
    spatial_index_free(si);
    graph_free(g);
}

int main(void)
{
    printf("spatial index tests\n");
    RUN(test_nearest_node_matches_brute_force);
    RUN(test_nearest_edge_matches_brute_force);
    RUN(test_main_component_preferred);
    TEST_SUMMARY();
}
