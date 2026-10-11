/*
 * route_cli.c — command-line front end to the Phase 1 routing engine.
 *
 * Usage:
 *   route_cli <nodes.csv> <edges.csv> <from_id> <to_id> [options]
 * Options:
 *   --mode fastest|shortest     (default fastest)
 *   --algo dijkstra|astar       (default dijkstra)
 *   --avoid-tolls
 *   --avoid-restricted
 *   --close <edge_id>           close an edge (repeatable)
 *   --traffic <edge_id>=<sec>   override an edge's travel time (repeatable;
 *                               values given here are MANUAL/SIMULATED input)
 */
#include "graph_io.h"
#include "routing.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <nodes.csv> <edges.csv> <from_id> <to_id>\n"
        "          [--mode fastest|shortest] [--algo dijkstra|astar]\n"
        "          [--avoid-tolls] [--avoid-restricted]\n"
        "          [--close EDGE_ID]... [--traffic EDGE_ID=SECONDS]...\n", prog);
}

static bool parse_id(const char *s, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || *end) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 5) { usage(argv[0]); return 2; }

    long long from_id, to_id;
    if (!parse_id(argv[3], &from_id) || !parse_id(argv[4], &to_id)) {
        fprintf(stderr, "error: from_id/to_id must be integers\n");
        return 2;
    }

    graph_load_report_t rep;
    graph_t *g = graph_load_csv(argv[1], argv[2], &rep);
    if (!g) { fprintf(stderr, "load failed: %s\n", rep.error); return 1; }
    printf("Loaded %zu nodes, %zu directed edges", rep.nodes_loaded, rep.edges_loaded);
    if (rep.distances_clamped)
        printf(" (%zu distances raised to straight-line length)", rep.distances_clamped);
    printf("\n");

    route_request_t req = { .mode = COST_TIME, .algorithm = ALGO_DIJKSTRA };
    bool manual_traffic = false;
    int rc = 1;

    for (int i = 5; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--mode") == 0 && val) {
            if (strcmp(val, "fastest") == 0) req.mode = COST_TIME;
            else if (strcmp(val, "shortest") == 0) req.mode = COST_DISTANCE;
            else { fprintf(stderr, "error: bad --mode\n"); goto out; }
            i++;
        } else if (strcmp(a, "--algo") == 0 && val) {
            if (strcmp(val, "dijkstra") == 0) req.algorithm = ALGO_DIJKSTRA;
            else if (strcmp(val, "astar") == 0) req.algorithm = ALGO_ASTAR;
            else { fprintf(stderr, "error: bad --algo\n"); goto out; }
            i++;
        } else if (strcmp(a, "--avoid-tolls") == 0) {
            req.avoid_tolls = true;
        } else if (strcmp(a, "--avoid-restricted") == 0) {
            req.avoid_restricted = true;
        } else if (strcmp(a, "--close") == 0 && val) {
            long long eid;
            edge_idx_t e;
            if (!parse_id(val, &eid) || (e = graph_find_edge(g, eid)) == IRA_INVALID_IDX) {
                fprintf(stderr, "error: unknown edge %s\n", val); goto out;
            }
            graph_set_closed(g, e, true);
            i++;
        } else if (strcmp(a, "--traffic") == 0 && val) {
            char buf[64];
            snprintf(buf, sizeof buf, "%s", val);
            char *eq = strchr(buf, '=');
            long long eid;
            char *end;
            edge_idx_t e;
            if (!eq) { fprintf(stderr, "error: --traffic needs ID=SECONDS\n"); goto out; }
            *eq = '\0';
            double secs = strtod(eq + 1, &end);
            if (!parse_id(buf, &eid) || *end || secs <= 0 ||
                (e = graph_find_edge(g, eid)) == IRA_INVALID_IDX) {
                fprintf(stderr, "error: bad --traffic %s\n", val); goto out;
            }
            graph_set_traffic_time(g, e, secs);
            manual_traffic = true;
            i++;
        } else {
            usage(argv[0]); goto out;
        }
    }

    req.source = graph_find_node(g, from_id);
    req.target = graph_find_node(g, to_id);
    if (req.source == IRA_INVALID_IDX || req.target == IRA_INVALID_IDX) {
        fprintf(stderr, "error: unknown node id\n");
        goto out;
    }

    route_result_t res;
    route_status_t st = route_compute(g, &req, &res);
    printf("Mode: %s | Algorithm: %s | Traffic: %s\n",
           req.mode == COST_TIME ? "fastest" : "shortest",
           req.algorithm == ALGO_ASTAR ? "A*" : "Dijkstra",
           manual_traffic ? "manual/simulated overrides (not live data)"
                          : "none (baseline estimates)");
    if (st != ROUTE_OK) {
        printf("No route: %s (settled %zu nodes)\n", route_status_str(st), res.nodes_settled);
        route_result_free(&res);
        rc = 3;
        goto out;
    }

    printf("Route nodes:");
    for (size_t i = 0; i < res.node_count; i++)
        printf(" %lld", (long long)g->nodes[res.nodes[i]].id);
    printf("\nRoute edges:");
    for (size_t i = 0; i < res.edge_count; i++)
        printf(" %lld", (long long)g->edges[res.edges[i]].id);
    printf("\nDistance: %.1f m (%.2f km)\n", res.distance_m, res.distance_m / 1000.0);
    printf("Duration: %.1f s (%.1f min)\n", res.duration_s, res.duration_s / 60.0);
    printf("Nodes settled: %zu\n", res.nodes_settled);
    route_result_free(&res);
    rc = 0;

out:
    graph_free(g);
    return rc;
}
