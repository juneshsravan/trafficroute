#ifndef IRA_DIJKSTRA_H
#define IRA_DIJKSTRA_H

#include "routing.h"

/* Single-source, single-target Dijkstra with early termination.
 * Preconditions: graph finalized, all edge costs non-negative (enforced at
 * insertion time by graph_add_edge / graph_set_traffic_time).
 * ws may be NULL. */
route_status_t dijkstra_route(const graph_t *g, const route_request_t *req,
                              route_workspace_t *ws, route_result_t *out);

#endif
