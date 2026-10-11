/*
 * Dijkstra on small graphs with hand-verified answers.
 */
#include "dijkstra.h"
#include "test_util.h"

/* Builds the classic 5-node example from Cormen et al. (CLRS, Fig. 24.6):
 *   s->t 10, s->y 5, t->x 1, t->y 2, y->t 3, y->x 9, y->z 2,
 *   x->z 4, z->x 6, z->s 7
 * Known shortest distances from s: t=8, x=9, y=5, z=7.
 * Distance and time weights are equal here; coordinates are irrelevant. */
enum { S, T, X, Y, Z };
static graph_t *clrs_graph(void)
{
    graph_t *g = graph_create();
    for (int i = 0; i < 5; i++) graph_add_node(g, i, 0.0, 0.0, NULL);
    struct { int u, v; double w; } E[] = {
        {S,T,10},{S,Y,5},{T,X,1},{T,Y,2},{Y,T,3},{Y,X,9},{Y,Z,2},
        {X,Z,4},{Z,X,6},{Z,S,7},
    };
    for (int i = 0; i < 10; i++)
        graph_add_edge(g, i + 1, (node_idx_t)E[i].u, (node_idx_t)E[i].v,
                       E[i].w, E[i].w, ROAD_PRIMARY, 0, 0);
    graph_finalize(g);
    return g;
}

static route_request_t req(node_idx_t s, node_idx_t t)
{
    return (route_request_t){ .source = s, .target = t, .mode = COST_DISTANCE };
}

static void test_clrs_distances(void)
{
    graph_t *g = clrs_graph();
    double expect[5] = {0, 8, 9, 5, 7};
    for (node_idx_t t = 0; t < 5; t++) {
        route_request_t r = req(S, t);
        route_result_t res;
        CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_OK);
        CHECK_NEAR(res.total_cost, expect[t], 1e-9);
        CHECK_NEAR(res.distance_m, expect[t], 1e-9);
        route_result_free(&res);
    }
    graph_free(g);
}

static void test_clrs_path_reconstruction(void)
{
    graph_t *g = clrs_graph();
    route_request_t r = req(S, X);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_OK);
    /* s -> y -> t -> x  (5 + 3 + 1 = 9) */
    node_idx_t expect_nodes[] = {S, Y, T, X};
    CHECK(res.node_count == 4);
    CHECK(res.edge_count == 3);
    for (size_t i = 0; i < 4 && i < res.node_count; i++) CHECK(res.nodes[i] == expect_nodes[i]);
    /* each edge connects consecutive nodes */
    for (size_t i = 0; i < res.edge_count; i++) {
        CHECK(g->edges[res.edges[i]].from == res.nodes[i]);
        CHECK(g->edges[res.edges[i]].to == res.nodes[i + 1]);
    }
    route_result_free(&res);
    graph_free(g);
}

static void test_directed_asymmetry(void)
{
    graph_t *g = clrs_graph();
    /* x -> s must go x->z->s = 4 + 7 = 11 (no reverse of s->...->x). */
    route_request_t r = req(X, S);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_OK);
    CHECK_NEAR(res.total_cost, 11.0, 1e-9);
    CHECK(res.node_count == 3);
    route_result_free(&res);
    graph_free(g);
}

static void test_source_equals_target(void)
{
    graph_t *g = clrs_graph();
    route_request_t r = req(Y, Y);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_OK);
    CHECK(res.node_count == 1 && res.edge_count == 0);
    CHECK_NEAR(res.total_cost, 0.0, 0.0);
    route_result_free(&res);
    graph_free(g);
}

static void test_disconnected(void)
{
    graph_t *g = graph_create();
    graph_add_node(g, 1, 0, 0, NULL);
    graph_add_node(g, 2, 0, 0, NULL);
    graph_add_node(g, 3, 0, 0, NULL);
    graph_add_edge(g, 1, 0, 1, 1, 1, ROAD_PRIMARY, 0, 0);
    graph_finalize(g);
    route_request_t r = req(0, 2);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_ERR_NO_PATH);
    CHECK(res.status == ROUTE_ERR_NO_PATH);
    CHECK(res.nodes == NULL && res.node_count == 0);
    route_result_free(&res);
    /* reverse direction of a one-way edge also unreachable */
    r = req(1, 0);
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_ERR_NO_PATH);
    route_result_free(&res);
    graph_free(g);
}

static void test_invalid_requests(void)
{
    graph_t *g = clrs_graph();
    route_request_t r = req(S, 99);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_ERR_INVALID_REQUEST);
    route_result_free(&res);

    graph_t *unfinal = graph_create();
    graph_add_node(unfinal, 1, 0, 0, NULL);
    r = req(0, 0);
    CHECK(dijkstra_route(unfinal, &r, NULL, &res) == ROUTE_ERR_INVALID_REQUEST);
    route_result_free(&res);
    graph_free(unfinal);
    graph_free(g);
}

static void test_parallel_edges_pick_cheapest(void)
{
    graph_t *g = graph_create();
    graph_add_node(g, 1, 0, 0, NULL);
    graph_add_node(g, 2, 0, 0, NULL);
    graph_add_edge(g, 1, 0, 1, 50, 50, ROAD_PRIMARY, 0, 0);
    graph_add_edge(g, 2, 0, 1, 20, 20, ROAD_PRIMARY, 0, 0);
    graph_add_edge(g, 3, 0, 1, 70, 70, ROAD_PRIMARY, 0, 0);
    graph_finalize(g);
    route_request_t r = req(0, 1);
    route_result_t res;
    CHECK(dijkstra_route(g, &r, NULL, &res) == ROUTE_OK);
    CHECK(res.edge_count == 1 && g->edges[res.edges[0]].id == 2);
    route_result_free(&res);
    graph_free(g);
}

int main(void)
{
    printf("dijkstra tests\n");
    RUN(test_clrs_distances);
    RUN(test_clrs_path_reconstruction);
    RUN(test_directed_asymmetry);
    RUN(test_source_equals_target);
    RUN(test_disconnected);
    RUN(test_invalid_requests);
    RUN(test_parallel_edges_pick_cheapest);
    TEST_SUMMARY();
}
