/* Route geometry, instructions, bearings, polyline encoding. */
#include "route_geometry.h"
#include "graph_io.h"
#include "test_util.h"

#include <string.h>

#define SAMPLE "../data/sample"

static void test_polyline_reference(void)
{
    /* Example from Google's polyline algorithm documentation. */
    geo_pt_t pts[] = { {38.5, -120.2}, {40.7, -120.95}, {43.252, -126.453} };
    char *s = polyline_encode(pts, 3);
    CHECK(s && strcmp(s, "_p~iF~ps|U_ulLnnqC_mqNvxq`@") == 0);
    free(s);
}

static void test_bearing(void)
{
    CHECK_NEAR(geo_bearing_deg(17, 79, 17.1, 79), 0, 1e-6);
    CHECK_NEAR(geo_bearing_deg(17, 79, 17, 79.1), 90, 0.1);
    CHECK_NEAR(geo_bearing_deg(17, 79, 16.9, 79), 180, 1e-6);
    CHECK_NEAR(geo_bearing_deg(17, 79, 17, 78.9), 270, 0.1);
}

static void test_sample_route_instructions(void)
{
    graph_load_report_t rep;
    graph_t *g = graph_load_csv(SAMPLE "/nodes.csv", SAMPLE "/edges.csv", &rep);
    CHECK(g != NULL);
    if (!g) return;

    /* Shortest A->D: three edges all named "Village Road" -> one leg. */
    route_request_t rq = { .source = graph_find_node(g, 1), .target = graph_find_node(g, 4), .mode = COST_DISTANCE };
    route_result_t r;
    route_geometry_t geo;
    CHECK(route_compute(g, &rq, &r) == ROUTE_OK);
    CHECK(route_geometry_build(g, &r, &geo));
    CHECK(geo.point_count == 4);
    CHECK(geo.instruction_count == 2);
    CHECK(geo.instructions[0].type == INSTR_DEPART);
    CHECK(strstr(geo.instructions[0].text, "Head east on Village Road") != NULL);
    CHECK_NEAR(geo.instructions[0].distance_m, r.distance_m, 0.01);
    CHECK(geo.instructions[1].type == INSTR_ARRIVE);
    route_geometry_free(&geo);
    route_result_free(&r);

    /* Fastest A->D via E,F: trunk, toll trunk, trunk -> 3 legs + arrive. */
    rq.mode = COST_TIME;
    CHECK(route_compute(g, &rq, &r) == ROUTE_OK);
    CHECK(route_geometry_build(g, &r, &geo));
    CHECK(geo.instruction_count == 4);
    CHECK(strstr(geo.instructions[0].text, "Head northeast on Sample Trunk Highway") != NULL);
    /* A->E bears ~25.5 deg, E->F due east (90): a 64.5 deg change = "Turn right" */
    CHECK(geo.instructions[1].type == INSTR_RIGHT);
    CHECK(strstr(geo.instructions[1].text, "(Toll)") != NULL);
    /* F->D bears ~154.5 deg after heading east: another 64.5 deg right turn */
    CHECK(geo.instructions[2].type == INSTR_RIGHT);
    double sum = 0;
    for (size_t i = 0; i < geo.instruction_count; i++) sum += geo.instructions[i].distance_m;
    CHECK_NEAR(sum, r.distance_m, 0.01);
    /* geometry starts/ends at the route's end nodes */
    CHECK_NEAR(geo.points[0].lat, 17.00, 1e-9);
    CHECK_NEAR(geo.points[geo.point_count - 1].lon, 79.06, 1e-9);
    route_geometry_free(&geo);
    route_result_free(&r);

    /* source == target */
    rq.target = rq.source;
    CHECK(route_compute(g, &rq, &r) == ROUTE_OK);
    CHECK(route_geometry_build(g, &r, &geo));
    CHECK(geo.instruction_count == 1 && geo.instructions[0].type == INSTR_ARRIVE);
    route_geometry_free(&geo);
    route_result_free(&r);
    graph_free(g);
}

static void test_shape_points_included(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b;
    graph_add_node(g, 1, 17.0, 79.0, &a);
    graph_add_node(g, 2, 17.0, 79.02, &b);
    graph_point_t shape[2] = { {17.005f, 79.005f}, {17.005f, 79.015f} };
    graph_add_edge_ex(g, 1, a, b, 2500, 200, ROAD_PRIMARY, 0, 0, "Curvy Road", shape, 2);
    graph_finalize(g);
    route_request_t rq = { .source = a, .target = b, .mode = COST_TIME };
    route_result_t r;
    route_geometry_t geo;
    CHECK(route_compute(g, &rq, &r) == ROUTE_OK);
    CHECK(route_geometry_build(g, &r, &geo));
    CHECK(geo.point_count == 4);
    CHECK_NEAR(geo.points[1].lat, 17.005, 1e-5);
    route_geometry_free(&geo);
    route_result_free(&r);
    graph_free(g);
}

int main(void)
{
    printf("geometry tests\n");
    RUN(test_polyline_reference);
    RUN(test_bearing);
    RUN(test_sample_route_instructions);
    RUN(test_shape_points_included);
    TEST_SUMMARY();
}
