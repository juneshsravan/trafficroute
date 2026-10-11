/* Traffic observations, expiry, incidents and their effect on routing. */
#include "traffic.h"
#include "graph_io.h"
#include "routing.h"
#include "spatial_index.h"
#include "test_util.h"

#include <string.h>

#define SAMPLE "../data/sample"
#define NOW 1760000000

static graph_t *G;
static traffic_state_t T;

static void setup(void)
{
    graph_load_report_t rep;
    G = graph_load_csv(SAMPLE "/nodes.csv", SAMPLE "/edges.csv", &rep);
    if (!G || !traffic_init(&T, G, NULL)) { fprintf(stderr, "setup failed\n"); exit(1); }
}
static void teardown(void) { traffic_free(&T); graph_free(G); }

static int64_t second_node_of_fastest(int64_t from, int64_t to)
{
    route_request_t rq = { .source = graph_find_node(G, from), .target = graph_find_node(G, to), .mode = COST_TIME };
    route_result_t r;
    int64_t id = -1;
    if (route_compute(G, &rq, &r) == ROUTE_OK && r.node_count > 1) id = G->nodes[r.nodes[1]].id;
    else if (r.status == ROUTE_ERR_NO_PATH) id = 0;
    route_result_free(&r);
    return id;
}

static void test_observation_changes_route_and_expires(void)
{
    setup();
    edge_idx_t ef = graph_find_edge(G, 5);
    CHECK(second_node_of_fastest(1, 4) == 5);
    CHECK(traffic_add_observation(&T, ef, 1200, 0.9, TSRC_ADMIN, NOW, NOW + 600, "test", NOW, NULL) == 0);
    CHECK_NEAR(graph_edge_time(&G->edges[ef]), 1200, 1e-3);
    CHECK(second_node_of_fastest(1, 4) == 2);          /* detour via B */
    CHECK(traffic_expire(&T, NOW + 599) == 0);
    CHECK(traffic_expire(&T, NOW + 600) == 1);         /* stale data removed */
    CHECK_NEAR(graph_edge_time(&G->edges[ef]), G->edges[ef].base_time_s, 1e-6);
    CHECK(second_node_of_fastest(1, 4) == 5);
    teardown();
}

static void test_validation_and_confidence(void)
{
    setup();
    edge_idx_t e = 0;
    CHECK(traffic_add_observation(&T, e, -5, 1, TSRC_ADMIN, NOW, NOW + 60, NULL, NOW, NULL) == -1);
    CHECK(traffic_add_observation(&T, e, 50, 1.5, TSRC_ADMIN, NOW, NOW + 60, NULL, NOW, NULL) == -1);
    CHECK(traffic_add_observation(&T, e, 50, 1, TSRC_ADMIN, NOW + 3600, NOW + 7200, NULL, NOW, NULL) == -2); /* future */
    CHECK(traffic_add_observation(&T, e, 50, 1, TSRC_ADMIN, NOW - 100, NOW - 1, NULL, NOW, NULL) == -2);    /* expired */
    CHECK(traffic_add_observation(&T, 9999, 50, 1, TSRC_ADMIN, NOW, NOW + 60, NULL, NOW, NULL) == -1);
    /* low-confidence observations are stored but not applied */
    CHECK(traffic_add_observation(&T, e, 5000, 0.2, TSRC_LIVE, NOW, NOW + 60, NULL, NOW, NULL) == 0);
    CHECK_NEAR(graph_edge_time(&G->edges[e]), G->edges[e].base_time_s, 1e-6);
    /* newest observation wins */
    CHECK(traffic_add_observation(&T, e, 300, 1, TSRC_ADMIN, NOW - 10, NOW + 60, NULL, NOW, NULL) == 0);
    CHECK(traffic_add_observation(&T, e, 400, 1, TSRC_ADMIN, NOW, NOW + 60, NULL, NOW, NULL) == 0);
    CHECK_NEAR(graph_edge_time(&G->edges[e]), 400, 1e-3);
    teardown();
}

static void test_incident_moderation(void)
{
    setup();
    edge_idx_t e9 = graph_find_edge(G, 9);   /* B->H one-way */
    uint64_t id;
    /* pending incidents never affect routing */
    CHECK(traffic_add_incident(&T, INC_CLOSURE, INC_PENDING, 17.005, 79.025, e9, true, NOW, NOW + 3600,
                               false, "test", &id) == 0);
    CHECK(!(G->edges[e9].flags & EDGE_FLAG_CLOSED));
    CHECK(traffic_set_incident_status(&T, id, INC_VERIFIED) == 0);
    CHECK(G->edges[e9].flags & EDGE_FLAG_CLOSED);
    route_request_t rq = { .source = graph_find_node(G, 2), .target = graph_find_node(G, 8), .mode = COST_TIME };
    route_result_t r;
    CHECK(route_compute(G, &rq, &r) == ROUTE_ERR_NO_PATH);   /* only way into H is closed */
    route_result_free(&r);
    CHECK(traffic_set_incident_status(&T, id, INC_REJECTED) == 0);
    CHECK(!(G->edges[e9].flags & EDGE_FLAG_CLOSED));
    CHECK(traffic_set_incident_status(&T, 424242, INC_VERIFIED) == -4);
    teardown();
}

static void test_closure_both_directions_and_expiry(void)
{
    setup();
    edge_idx_t e1 = graph_find_edge(G, 1), r1 = graph_find_edge(G, -1);
    uint64_t id;
    CHECK(traffic_add_incident(&T, INC_CLOSURE, INC_VERIFIED, 17.0, 79.01, e1, true, NOW, NOW + 60,
                               false, "", &id) == 0);
    CHECK((G->edges[e1].flags & EDGE_FLAG_CLOSED) && (G->edges[r1].flags & EDGE_FLAG_CLOSED));
    traffic_summary_t s;
    traffic_summary(&T, NOW, &s);
    CHECK(s.closed_edges == 2);
    traffic_expire(&T, NOW + 61);
    CHECK(!(G->edges[e1].flags & EDGE_FLAG_CLOSED) && !(G->edges[r1].flags & EDGE_FLAG_CLOSED));
    CHECK(traffic_find_incident(&T, id)->status == INC_EXPIRED);
    teardown();
}

static void test_accident_delay_model(void)
{
    setup();
    edge_idx_t e = graph_find_edge(G, 4);
    double base = G->edges[e].base_time_s;
    CHECK(traffic_add_incident(&T, INC_ACCIDENT, INC_VERIFIED, 17.01, 79.005, e, false, NOW, NOW + 600,
                               false, "", NULL) == 0);
    CHECK_NEAR(graph_edge_time(&G->edges[e]), base * 2.0, 1e-2);
    traffic_summary_t s;
    traffic_summary(&T, NOW, &s);
    CHECK(s.incident_model_active);
    CHECK(!s.live_available);
    teardown();
}

static void test_simulation_is_labelled(void)
{
    setup();
    spatial_index_t *si = spatial_index_build(G);
    size_t n = traffic_simulate(&T, si, 17.0, 79.03, 5000, 5, 42, NOW, 600);
    CHECK(n == 5);
    traffic_summary_t s;
    traffic_summary(&T, NOW, &s);
    CHECK(s.simulated_active && !s.live_available && s.active_by_source[TSRC_SIMULATED] == 5);
    for (size_t i = 0; i < T.obs_count; i++) CHECK(T.obs[i].source == TSRC_SIMULATED);
    CHECK(traffic_clear_source(&T, TSRC_SIMULATED) == 5);
    for (size_t i = 0; i < G->edge_count; i++) CHECK(G->edges[i].traffic_time_s == 0.0f);
    spatial_index_free(si);
    teardown();
}

static void test_live_staleness(void)
{
    setup();
    CHECK(traffic_add_observation(&T, 0, 100, 0.9, TSRC_LIVE, NOW, NOW + 3600, "feed", NOW, NULL) == 0);
    traffic_summary_t s;
    traffic_summary(&T, NOW + 60, &s);
    CHECK(s.live_available);
    traffic_summary(&T, NOW + 1000, &s);   /* > 900 s since last live update */
    CHECK(!s.live_available);
    teardown();
}

int main(void)
{
    printf("traffic tests\n");
    RUN(test_observation_changes_route_and_expires);
    RUN(test_validation_and_confidence);
    RUN(test_incident_moderation);
    RUN(test_closure_both_directions_and_expiry);
    RUN(test_accident_delay_model);
    RUN(test_simulation_is_labelled);
    RUN(test_live_staleness);
    TEST_SUMMARY();
}
