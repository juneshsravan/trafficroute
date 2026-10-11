#include "graph.h"
#include "graph_io.h"
#include "geospatial.h"
#include "test_util.h"

#include <string.h>

#ifndef SAMPLE_DIR
#define SAMPLE_DIR "../data/sample"
#endif

static void test_build_and_csr(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b, c;
    CHECK(graph_add_node(g, 10, 17.0, 79.0, &a) == GRAPH_OK);
    CHECK(graph_add_node(g, 20, 17.0, 79.1, &b) == GRAPH_OK);
    CHECK(graph_add_node(g, 30, 17.1, 79.0, &c) == GRAPH_OK);
    /* Insert out of source order to exercise the counting sort. */
    CHECK(graph_add_edge(g, 3, c, a, 100, 10, ROAD_PRIMARY, 0, 0) == GRAPH_OK);
    CHECK(graph_add_edge(g, 1, a, b, 100, 10, ROAD_PRIMARY, 0, 0) == GRAPH_OK);
    CHECK(graph_add_edge(g, 2, a, c, 100, 10, ROAD_PRIMARY, 0, 0) == GRAPH_OK);
    CHECK(graph_finalize(g) == GRAPH_OK);

    CHECK(graph_out_end(g, a) - graph_out_begin(g, a) == 2);
    CHECK(graph_out_end(g, b) - graph_out_begin(g, b) == 0);
    CHECK(graph_out_end(g, c) - graph_out_begin(g, c) == 1);
    for (node_idx_t v = 0; v < 3; v++)
        for (edge_idx_t e = graph_out_begin(g, v); e < graph_out_end(g, v); e++)
            CHECK(g->edges[e].from == v);
    CHECK(graph_find_node(g, 20) == b);
    CHECK(graph_find_node(g, 99) == IRA_INVALID_IDX);

    /* Topology is frozen after finalize. */
    CHECK(graph_add_node(g, 40, 0, 0, NULL) == GRAPH_ERR_FINALIZED);
    graph_free(g);
}

static void test_rejects_invalid_input(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b;
    CHECK(graph_add_node(g, 1, 91.0, 0, NULL) == GRAPH_ERR_INVALID);
    CHECK(graph_add_node(g, 1, 0, 181.0, NULL) == GRAPH_ERR_INVALID);
    CHECK(graph_add_node(g, 1, NAN, 0, NULL) == GRAPH_ERR_INVALID);
    graph_add_node(g, 1, 17, 79, &a);
    graph_add_node(g, 2, 17, 79.01, &b);
    CHECK(graph_add_edge(g, 1, a, b, -5, 10, ROAD_PRIMARY, 0, 0) == GRAPH_ERR_INVALID);
    CHECK(graph_add_edge(g, 1, a, b, 5, 0, ROAD_PRIMARY, 0, 0) == GRAPH_ERR_INVALID);
    CHECK(graph_add_edge(g, 1, a, b, INFINITY, 1, ROAD_PRIMARY, 0, 0) == GRAPH_ERR_INVALID);
    CHECK(graph_add_edge(g, 1, a, 7, 5, 1, ROAD_PRIMARY, 0, 0) == GRAPH_ERR_INVALID);
    CHECK(graph_add_edge(g, 1, a, b, 5, 1, ROAD_CLASS_COUNT, 0, 0) == GRAPH_ERR_INVALID);
    CHECK(graph_set_traffic_time(g, 0, 5) == GRAPH_ERR_NOT_FINALIZED);
    graph_free(g);
}

static void test_haversine(void)
{
    /* 0.01 deg of latitude ~ 1111.95 m on a sphere of mean radius. */
    CHECK_NEAR(geo_haversine_m(17.0, 79.0, 17.01, 79.0), 1111.95, 0.5);
    CHECK_NEAR(geo_haversine_m(17.0, 79.0, 17.0, 79.0), 0.0, 1e-9);
    /* Symmetry */
    CHECK_NEAR(geo_haversine_m(13.0, 80.2, 12.9, 77.6),
               geo_haversine_m(12.9, 77.6, 13.0, 80.2), 1e-6);
    CHECK(geo_in_india_bbox(13.08, 80.27));
    CHECK(!geo_in_india_bbox(51.5, -0.12));
}

static void test_traffic_and_max_speed(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b;
    graph_add_node(g, 1, 17, 79, &a);
    graph_add_node(g, 2, 17, 79.01, &b);
    graph_add_edge(g, 1, a, b, 1000, 100, ROAD_PRIMARY, 0, 0);   /* 10 m/s */
    graph_finalize(g);
    CHECK_NEAR(g->max_speed_mps, 10.0, 1e-9);
    CHECK(graph_set_traffic_time(g, 0, 50) == GRAPH_OK);           /* 20 m/s */
    CHECK_NEAR(graph_edge_time(&g->edges[0]), 50.0, 1e-9);
    CHECK_NEAR(g->max_speed_mps, 20.0, 1e-9);
    CHECK(graph_set_traffic_time(g, 0, 400) == GRAPH_OK);          /* congestion */
    CHECK_NEAR(graph_edge_time(&g->edges[0]), 400.0, 1e-9);
    CHECK(g->max_speed_mps >= 20.0);   /* still an upper bound */
    graph_clear_traffic(g);
    CHECK_NEAR(graph_edge_time(&g->edges[0]), 100.0, 1e-9);
    CHECK_NEAR(g->max_speed_mps, 10.0, 1e-9);
    CHECK(graph_set_closed(g, 0, true) == GRAPH_OK);
    CHECK(g->edges[0].flags & EDGE_FLAG_CLOSED);
    CHECK(graph_set_closed(g, 5, true) == GRAPH_ERR_NOT_FOUND);
    graph_free(g);
}

static void test_load_sample(void)
{
    graph_load_report_t rep;
    graph_t *g = graph_load_csv(SAMPLE_DIR "/nodes.csv", SAMPLE_DIR "/edges.csv", &rep);
    CHECK(g != NULL);
    if (!g) { fprintf(stderr, "  load error: %s\n", rep.error); return; }
    CHECK(rep.nodes_loaded == 10);
    /* 12 roads, 2 of them one-way -> 10*2 + 2 = 22 directed edges */
    CHECK(rep.edges_loaded == 22);
    CHECK(rep.distances_clamped == 0);
    /* every edge at least as long as the straight line between its ends */
    for (size_t i = 0; i < g->edge_count; i++) {
        const graph_edge_t *e = &g->edges[i];
        const graph_node_t *u = &g->nodes[e->from], *v = &g->nodes[e->to];
        CHECK(e->distance_m + 1e-6 >= geo_haversine_m(u->lat, u->lon, v->lat, v->lon));
    }
    /* time = distance / (km/h / 3.6) for edge 4 (3000 m at 70 km/h) */
    edge_idx_t e4 = graph_find_edge(g, 4);
    CHECK(e4 != IRA_INVALID_IDX);
    CHECK_NEAR(g->edges[e4].base_time_s, 3000.0 / (70.0 / 3.6), 1e-3);
    /* one-way edge 9 has no reverse twin */
    CHECK(graph_find_edge(g, -9) == IRA_INVALID_IDX);
    CHECK(graph_find_edge(g, -1) != IRA_INVALID_IDX);
    graph_free(g);
}

static void test_load_errors(void)
{
    graph_load_report_t rep;
    CHECK(graph_load_csv("/nonexistent/n.csv", "/nonexistent/e.csv", &rep) == NULL);
    CHECK(strstr(rep.error, "cannot open") != NULL);
}

int main(void)
{
    printf("graph tests\n");
    RUN(test_build_and_csr);
    RUN(test_rejects_invalid_input);
    RUN(test_haversine);
    RUN(test_traffic_and_max_speed);
    RUN(test_load_sample);
    RUN(test_load_errors);
    TEST_SUMMARY();
}
