/*
 * astar.c — A* search with geographic heuristics (see astar.h for the
 * admissibility argument).
 *
 * Consistency (needed so settled nodes are never reopened): for any edge
 * u->v with cost c, h(u) <= c + h(v). By the triangle inequality on the
 * sphere, hav(u,t) <= hav(u,v) + hav(v,t), and hav(u,v) <= distance_m(u,v)
 * <= v_max * time(u,v). Dividing by v_max gives the time-mode case.
 */
#include "astar.h"
#include "geospatial.h"

static double heuristic(const graph_t *g, const route_request_t *req, node_idx_t n)
{
    const graph_node_t *a = &g->nodes[n];
    const graph_node_t *t = &g->nodes[req->target];
    double d = geo_haversine_m(a->lat, a->lon, t->lat, t->lon);
    /* Shrink by a relative 1e-6 so float/double rounding can never make the
     * estimate exceed the true remaining cost. */
    d *= (1.0 - 1e-6);
    if (req->mode == COST_DISTANCE) return d;
    /* Time mode: straight line at the fastest speed present in the graph. */
    return g->max_speed_mps > 0.0 ? d / g->max_speed_mps : 0.0;
}

route_status_t astar_route(const graph_t *g, const route_request_t *req,
                           route_workspace_t *ws, route_result_t *out)
{
    return route_search(g, req, heuristic, ws, out);
}
