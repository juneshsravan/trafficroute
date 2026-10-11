/*
 * dijkstra.c — Dijkstra's algorithm.
 *
 * Implemented as route_search() with no heuristic: nodes are settled strictly
 * in order of their cost from the source using the indexed binary heap,
 * giving O((V + E) log V). Correct only for non-negative edge costs, which
 * graph.c enforces (distance_m > 0, time_s > 0, penalties >= 1).
 */
#include "dijkstra.h"

route_status_t dijkstra_route(const graph_t *g, const route_request_t *req,
                              route_workspace_t *ws, route_result_t *out)
{
    return route_search(g, req, NULL, ws, out);
}
