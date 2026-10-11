/* Alternative routes: distinct, valid, bounded stretch, respect filters. */
#include "route_alternatives.h"
#include "graph_io.h"
#include "test_util.h"

#define SAMPLE "../data/sample"

static graph_t *load(void)
{
    graph_load_report_t rep;
    graph_t *g = graph_load_csv(SAMPLE "/nodes.csv", SAMPLE "/edges.csv", &rep);
    if (!g) { fprintf(stderr, "%s\n", rep.error); exit(1); }
    return g;
}

/* Consecutive edges must connect and start/end at the request endpoints. */
static bool valid_path(const graph_t *g, const route_request_t *rq, const route_result_t *r)
{
    if (r->node_count == 0 || r->nodes[0] != rq->source || r->nodes[r->node_count - 1] != rq->target)
        return false;
    for (size_t i = 0; i < r->edge_count; i++) {
        const graph_edge_t *e = &g->edges[r->edges[i]];
        if (e->from != r->nodes[i] || e->to != r->nodes[i + 1]) return false;
        if (!route_edge_usable(e, rq)) return false;
    }
    return true;
}

static void test_sample_alternatives(void)
{
    graph_t *g = load();
    route_request_t rq = { .source = graph_find_node(g, 1), .target = graph_find_node(g, 4),
                           .mode = COST_TIME };
    route_result_t best, alts[MAX_ALTERNATIVES];
    size_t n = 0;
    CHECK(route_with_alternatives(g, &rq, NULL, NULL, 3, &best, alts, &n) == ROUTE_OK);
    CHECK(n >= 1);
    CHECK(valid_path(g, &rq, &best));
    for (size_t i = 0; i < n; i++) {
        CHECK(valid_path(g, &rq, &alts[i]));
        CHECK(alts[i].total_cost >= best.total_cost - 1e-6);
        CHECK(alts[i].total_cost <= best.total_cost * 1.5 + 1e-6);
        CHECK(route_overlap_fraction(g, &alts[i], &best) <= 0.6 + 1e-9);
        for (size_t j = 0; j < i; j++) CHECK(route_overlap_fraction(g, &alts[i], &alts[j]) <= 0.6 + 1e-9);
    }
    /* best is the plain fastest route A-E-F-D; the first alternative is A-B-H-D (~700 s) */
    CHECK(best.node_count == 4 && g->nodes[best.nodes[1]].id == 5);
    if (n) CHECK(g->nodes[alts[0].nodes[1]].id == 2);
    route_result_free(&best);
    for (size_t i = 0; i < n; i++) route_result_free(&alts[i]);
    graph_free(g);
}

static void test_alternatives_respect_filters(void)
{
    graph_t *g = load();
    route_request_t rq = { .source = graph_find_node(g, 1), .target = graph_find_node(g, 4),
                           .mode = COST_TIME, .avoid_tolls = true };
    graph_set_closed(g, graph_find_edge(g, 9), true);
    route_result_t best, alts[MAX_ALTERNATIVES];
    size_t n = 0;
    CHECK(route_with_alternatives(g, &rq, NULL, NULL, 3, &best, alts, &n) == ROUTE_OK);
    CHECK(valid_path(g, &rq, &best));
    for (size_t i = 0; i < n; i++) CHECK(valid_path(g, &rq, &alts[i]));
    route_result_free(&best);
    for (size_t i = 0; i < n; i++) route_result_free(&alts[i]);
    graph_free(g);
}

static void test_no_alternative_on_single_path(void)
{
    graph_t *g = graph_create();
    node_idx_t a, b, c;
    graph_add_node(g, 1, 17, 79, &a);
    graph_add_node(g, 2, 17, 79.01, &b);
    graph_add_node(g, 3, 17, 79.02, &c);
    graph_add_edge(g, 1, a, b, 1100, 100, ROAD_PRIMARY, 0, 0);
    graph_add_edge(g, 2, b, c, 1100, 100, ROAD_PRIMARY, 0, 0);
    graph_finalize(g);
    route_request_t rq = { .source = a, .target = c, .mode = COST_TIME };
    route_result_t best, alts[MAX_ALTERNATIVES];
    size_t n = 99;
    CHECK(route_with_alternatives(g, &rq, NULL, NULL, 2, &best, alts, &n) == ROUTE_OK);
    CHECK(n == 0);   /* the only path cannot be "meaningfully different" */
    route_result_free(&best);
    graph_free(g);
}

static void test_penalty_map(void)
{
    route_penalty_t p;
    CHECK(route_penalty_init(&p, 4));
    for (edge_idx_t e = 0; e < 1000; e++) CHECK(route_penalty_multiply(&p, e * 7, 1.5f));
    CHECK(route_penalty_multiply(&p, 7, 2.0f));
    CHECK_NEAR(route_penalty_get(&p, 7), 3.0, 1e-6);
    CHECK_NEAR(route_penalty_get(&p, 14), 1.5, 1e-6);
    CHECK_NEAR(route_penalty_get(&p, 15), 1.0, 1e-6);
    CHECK(!route_penalty_multiply(&p, 3, 0.5f));    /* factors < 1 rejected (would break Dijkstra/A*) */
    route_penalty_free(&p);
}

int main(void)
{
    printf("alternative route tests\n");
    RUN(test_sample_alternatives);
    RUN(test_alternatives_respect_filters);
    RUN(test_no_alternative_on_single_path);
    RUN(test_penalty_map);
    TEST_SUMMARY();
}
