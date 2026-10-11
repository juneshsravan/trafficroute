/*
 * routing.h — shared request/result types and helpers for all algorithms.
 */
#ifndef IRA_ROUTING_H
#define IRA_ROUTING_H

#include "graph.h"
#include "heap.h"

typedef enum {
    COST_DISTANCE = 0,   /* "shortest": minimise sum of distance_m */
    COST_TIME            /* "fastest":  minimise sum of effective time_s */
} cost_mode_t;

typedef enum {
    ALGO_DIJKSTRA = 0,
    ALGO_ASTAR
} algorithm_t;

/* Sparse per-edge cost multipliers (>= 1.0) used by the alternative-route
 * penalty method. A hash map instead of an E-sized array, because a full
 * array would cost 4 bytes x every edge in India per request. */
typedef struct {
    edge_idx_t *keys;    /* IRA_INVALID_IDX = empty slot */
    float      *factor;
    size_t      cap;     /* power of two */
    size_t      used;
} route_penalty_t;

bool  route_penalty_init(route_penalty_t *p, size_t expected);
void  route_penalty_free(route_penalty_t *p);
bool  route_penalty_multiply(route_penalty_t *p, edge_idx_t e, float factor);
float route_penalty_get(const route_penalty_t *p, edge_idx_t e);

/*
 * Search workspace: per-node scratch arrays reused across queries.
 * "Generation stamps" mean nothing has to be reset between queries: a node's
 * gcost/pred are valid only if stamp[v] == gen, so starting a new query is
 * O(1) instead of O(V). One workspace serves one query at a time; a server
 * keeps a small pool (memory ~37 bytes per graph node per workspace).
 */
typedef struct {
    size_t      capacity;    /* node count it was built for */
    double     *gcost;
    edge_idx_t *pred;
    uint32_t   *stamp;       /* reached in generation gen */
    uint32_t   *settled;     /* settled in generation gen */
    uint32_t    gen;
    min_heap_t *heap;
} route_workspace_t;

route_workspace_t *route_workspace_create(size_t node_count);
void               route_workspace_free(route_workspace_t *ws);

typedef struct {
    node_idx_t  source;
    node_idx_t  target;
    cost_mode_t mode;
    algorithm_t algorithm;
    bool        avoid_tolls;
    bool        avoid_restricted;
    const route_penalty_t *penalty;   /* optional, NULL = none */
    size_t      max_settled;          /* 0 = unlimited; guards request time */
} route_request_t;

typedef enum {
    ROUTE_OK = 0,
    ROUTE_ERR_INVALID_REQUEST,  /* bad node index, graph not finalized */
    ROUTE_ERR_NO_PATH,          /* target unreachable with current filters */
    ROUTE_ERR_NOMEM,
    ROUTE_ERR_LIMIT             /* search exceeded max_settled */
} route_status_t;

typedef struct {
    route_status_t status;
    node_idx_t    *nodes;        /* ordered node indices, source..target */
    size_t         node_count;
    edge_idx_t    *edges;        /* ordered edge indices, node_count-1 of them */
    size_t         edge_count;
    double         total_cost;   /* real (unpenalised) cost in requested unit */
    double         distance_m;   /* always filled */
    double         duration_s;   /* always filled (effective times) */
    size_t         nodes_settled;/* search effort, for comparing algorithms */
} route_result_t;

const char *route_status_str(route_status_t s);

/* Dispatches to Dijkstra or A* according to req->algorithm.
 * ws may be NULL (a temporary workspace is allocated).
 * Caller must route_result_free() the result. */
route_status_t route_compute(const graph_t *g, const route_request_t *req,
                             route_result_t *out);
route_status_t route_compute_ws(const graph_t *g, const route_request_t *req,
                                route_workspace_t *ws, route_result_t *out);

void route_result_free(route_result_t *r);

/* ---- helpers used by algorithm implementations ---- */

/* Whether the edge may be used under the request's filters. Closed edges are
 * never usable, regardless of options. */
bool route_edge_usable(const graph_edge_t *e, const route_request_t *req);

/* Cost of traversing the edge in the requested mode (always >= 0). */
double route_edge_cost(const graph_edge_t *e, cost_mode_t mode);

/* Heuristic callback: lower bound on remaining cost from node to req->target.
 * NULL heuristic == plain Dijkstra. */
typedef double (*route_heuristic_fn)(const graph_t *g, const route_request_t *req,
                                     node_idx_t node);

/* Shared best-first search used by both Dijkstra and A*. Keeping one loop
 * guarantees the two algorithms differ ONLY in the heuristic, which is what
 * makes the A*==Dijkstra equivalence tests meaningful. */
route_status_t route_search(const graph_t *g, const route_request_t *req,
                            route_heuristic_fn heuristic, route_workspace_t *ws,
                            route_result_t *out);

#endif
