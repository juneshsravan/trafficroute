#ifndef IRA_ASTAR_H
#define IRA_ASTAR_H

#include "routing.h"

/* A* search. Heuristic:
 *   COST_DISTANCE: haversine(node, target)            [metres]
 *   COST_TIME:     haversine(node, target) / v_max    [seconds]
 * Both are admissible provided every edge's distance_m is >= the straight-line
 * distance between its endpoints (enforced by the loaders/importer) and v_max
 * is the fastest effective speed in the graph (maintained by graph.c).
 * Penalty multipliers are >= 1, so they never break admissibility.
 * With an admissible + consistent heuristic A* returns the same optimal cost
 * as Dijkstra while usually settling far fewer nodes. ws may be NULL. */
route_status_t astar_route(const graph_t *g, const route_request_t *req,
                           route_workspace_t *ws, route_result_t *out);

#endif
