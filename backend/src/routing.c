/*
 * routing.c — request dispatch, edge filtering/costing, workspaces, penalty
 * maps, the shared search loop, and path reconstruction.
 */
#include "routing.h"
#include "dijkstra.h"
#include "astar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *route_status_str(route_status_t s)
{
    switch (s) {
    case ROUTE_OK:                  return "ok";
    case ROUTE_ERR_INVALID_REQUEST: return "invalid request";
    case ROUTE_ERR_NO_PATH:         return "no route between the given points";
    case ROUTE_ERR_NOMEM:           return "out of memory";
    case ROUTE_ERR_LIMIT:           return "route search exceeded the configured effort limit";
    }
    return "unknown";
}

void route_result_free(route_result_t *r)
{
    if (!r) return;
    free(r->nodes);
    free(r->edges);
    memset(r, 0, sizeof *r);
}

/* ---------------- penalty map ---------------- */

static size_t hash_edge(edge_idx_t e, size_t mask)
{
    uint64_t x = (uint64_t)e * 0x9E3779B97F4A7C15ULL;
    return (size_t)(x >> 32) & mask;
}

bool route_penalty_init(route_penalty_t *p, size_t expected)
{
    memset(p, 0, sizeof *p);
    size_t cap = 64;
    while (cap < expected * 2) cap *= 2;
    p->keys = malloc(cap * sizeof *p->keys);
    p->factor = malloc(cap * sizeof *p->factor);
    if (!p->keys || !p->factor) { route_penalty_free(p); return false; }
    memset(p->keys, 0xFF, cap * sizeof *p->keys);
    p->cap = cap;
    return true;
}

void route_penalty_free(route_penalty_t *p)
{
    if (!p) return;
    free(p->keys);
    free(p->factor);
    memset(p, 0, sizeof *p);
}

static bool penalty_grow(route_penalty_t *p)
{
    route_penalty_t n;
    if (!route_penalty_init(&n, p->cap)) return false;   /* doubles capacity */
    for (size_t i = 0; i < p->cap; i++)
        if (p->keys[i] != IRA_INVALID_IDX) {
            size_t j = hash_edge(p->keys[i], n.cap - 1);
            while (n.keys[j] != IRA_INVALID_IDX) j = (j + 1) & (n.cap - 1);
            n.keys[j] = p->keys[i];
            n.factor[j] = p->factor[i];
            n.used++;
        }
    route_penalty_free(p);
    *p = n;
    return true;
}

bool route_penalty_multiply(route_penalty_t *p, edge_idx_t e, float factor)
{
    if (!p || !p->cap || !(factor >= 1.0f)) return false;
    if ((p->used + 1) * 2 > p->cap && !penalty_grow(p)) return false;
    size_t j = hash_edge(e, p->cap - 1);
    while (p->keys[j] != IRA_INVALID_IDX && p->keys[j] != e) j = (j + 1) & (p->cap - 1);
    if (p->keys[j] == IRA_INVALID_IDX) {
        p->keys[j] = e;
        p->factor[j] = factor;
        p->used++;
    } else {
        p->factor[j] *= factor;
    }
    return true;
}

float route_penalty_get(const route_penalty_t *p, edge_idx_t e)
{
    if (!p || !p->cap) return 1.0f;
    size_t j = hash_edge(e, p->cap - 1);
    while (p->keys[j] != IRA_INVALID_IDX) {
        if (p->keys[j] == e) return p->factor[j];
        j = (j + 1) & (p->cap - 1);
    }
    return 1.0f;
}

/* ---------------- workspace ---------------- */

route_workspace_t *route_workspace_create(size_t n)
{
    if (n == 0 || n >= HEAP_NOT_IN) return NULL;
    route_workspace_t *ws = calloc(1, sizeof *ws);
    if (!ws) return NULL;
    ws->capacity = n;
    ws->gcost   = malloc(n * sizeof *ws->gcost);
    ws->pred    = malloc(n * sizeof *ws->pred);
    ws->stamp   = calloc(n, sizeof *ws->stamp);
    ws->settled = calloc(n, sizeof *ws->settled);
    ws->heap    = heap_create(n);
    if (!ws->gcost || !ws->pred || !ws->stamp || !ws->settled || !ws->heap) {
        route_workspace_free(ws);
        return NULL;
    }
    ws->gen = 0;
    return ws;
}

void route_workspace_free(route_workspace_t *ws)
{
    if (!ws) return;
    free(ws->gcost);
    free(ws->pred);
    free(ws->stamp);
    free(ws->settled);
    heap_free(ws->heap);
    free(ws);
}

static void ws_begin(route_workspace_t *ws)
{
    heap_clear(ws->heap);
    if (++ws->gen == 0) {     /* stamp counter wrapped: do a real reset once */
        memset(ws->stamp, 0, ws->capacity * sizeof *ws->stamp);
        memset(ws->settled, 0, ws->capacity * sizeof *ws->settled);
        ws->gen = 1;
    }
}

static inline double ws_cost(const route_workspace_t *ws, node_idx_t v)
{
    return ws->stamp[v] == ws->gen ? ws->gcost[v] : INFINITY;
}

/* ---------------- edge filters / costs ---------------- */

bool route_edge_usable(const graph_edge_t *e, const route_request_t *req)
{
    if (e->flags & EDGE_FLAG_CLOSED) return false;
    if (req->avoid_tolls && (e->flags & EDGE_FLAG_TOLL)) return false;
    if (req->avoid_restricted && (e->flags & EDGE_FLAG_RESTRICTED)) return false;
    return true;
}

double route_edge_cost(const graph_edge_t *e, cost_mode_t mode)
{
    return mode == COST_DISTANCE ? (double)e->distance_m : graph_edge_time(e);
}

/* ---------------- dispatch ---------------- */

route_status_t route_compute_ws(const graph_t *g, const route_request_t *req,
                                route_workspace_t *ws, route_result_t *out)
{
    if (!out) return ROUTE_ERR_INVALID_REQUEST;
    memset(out, 0, sizeof *out);
    if (!g || !req) return out->status = ROUTE_ERR_INVALID_REQUEST;

    switch (req->algorithm) {
    case ALGO_DIJKSTRA: return dijkstra_route(g, req, ws, out);
    case ALGO_ASTAR:    return astar_route(g, req, ws, out);
    }
    return out->status = ROUTE_ERR_INVALID_REQUEST;
}

route_status_t route_compute(const graph_t *g, const route_request_t *req,
                             route_result_t *out)
{
    return route_compute_ws(g, req, NULL, out);
}

/* ---------------- path reconstruction ---------------- */

static route_status_t reconstruct(const graph_t *g, const route_request_t *req,
                                  const route_workspace_t *ws, route_result_t *out)
{
    /* First pass: count edges on the path (bounded by V to catch corruption). */
    size_t n_edges = 0;
    node_idx_t v = req->target;
    while (v != req->source) {
        if (ws->stamp[v] != ws->gen) return ROUTE_ERR_NO_PATH;
        edge_idx_t ei = ws->pred[v];
        if (ei == IRA_INVALID_IDX || n_edges >= g->node_count)
            return ROUTE_ERR_NO_PATH;
        v = g->edges[ei].from;
        n_edges++;
    }

    out->nodes = malloc((n_edges + 1) * sizeof *out->nodes);
    out->edges = n_edges ? malloc(n_edges * sizeof *out->edges) : NULL;
    if (!out->nodes || (n_edges && !out->edges)) {
        free(out->nodes); free(out->edges);
        out->nodes = NULL; out->edges = NULL;
        return ROUTE_ERR_NOMEM;
    }

    /* Second pass: fill backwards so arrays read source -> target. */
    out->node_count = n_edges + 1;
    out->edge_count = n_edges;
    out->distance_m = 0.0;
    out->duration_s = 0.0;
    v = req->target;
    out->nodes[n_edges] = v;
    for (size_t i = n_edges; i > 0; i--) {
        edge_idx_t ei = ws->pred[v];
        const graph_edge_t *e = &g->edges[ei];
        out->edges[i - 1] = ei;
        out->distance_m += (double)e->distance_m;
        out->duration_s += graph_edge_time(e);
        v = e->from;
        out->nodes[i - 1] = v;
    }
    /* Report the real cost, not the penalised one used during search. */
    out->total_cost = req->mode == COST_DISTANCE ? out->distance_m : out->duration_s;
    return ROUTE_OK;
}

/* ---------------- the search loop ---------------- */

route_status_t route_search(const graph_t *g, const route_request_t *req,
                            route_heuristic_fn heuristic, route_workspace_t *ws,
                            route_result_t *out)
{
    memset(out, 0, sizeof *out);
    if (!g || !req || !g->finalized || g->node_count == 0 ||
        req->source >= g->node_count || req->target >= g->node_count ||
        (req->mode != COST_DISTANCE && req->mode != COST_TIME) ||
        (ws && ws->capacity < g->node_count))
        return out->status = ROUTE_ERR_INVALID_REQUEST;

    route_workspace_t *own = NULL;
    if (!ws) {
        own = ws = route_workspace_create(g->node_count);
        if (!ws) return out->status = ROUTE_ERR_NOMEM;
    }
    ws_begin(ws);

    route_status_t st = ROUTE_ERR_NO_PATH;
    const uint32_t gen = ws->gen;

    /* Steps 1-3: (lazily) initialise costs, source cost 0, push source. */
    ws->gcost[req->source] = 0.0;
    ws->pred[req->source]  = IRA_INVALID_IDX;
    ws->stamp[req->source] = gen;
    heap_push_or_decrease(ws->heap, req->source,
                          heuristic ? heuristic(g, req, req->source) : 0.0);

    uint32_t u;
    while (heap_pop(ws->heap, &u, NULL)) {          /* step 4: smallest cost */
        ws->settled[u] = gen;
        out->nodes_settled++;
        if (u == req->target) { st = ROUTE_OK; break; }   /* step 7: early stop */
        if (req->max_settled && out->nodes_settled >= req->max_settled) {
            st = ROUTE_ERR_LIMIT;
            break;
        }

        const double gu = ws->gcost[u];
        /* Step 5: relax outgoing edges. */
        for (edge_idx_t ei = graph_out_begin(g, u); ei < graph_out_end(g, u); ei++) {
            const graph_edge_t *e = &g->edges[ei];
            if (!route_edge_usable(e, req)) continue;
            node_idx_t v = e->to;
            /* With a consistent heuristic (zero, or haversine-based), a settled
             * node's cost is final, so it never needs reopening. */
            if (ws->settled[v] == gen) continue;
            double c = route_edge_cost(e, req->mode);
            if (req->penalty) c *= (double)route_penalty_get(req->penalty, ei);
            double ng = gu + c;
            if (ng < ws_cost(ws, v)) {
                ws->gcost[v] = ng;
                ws->pred[v]  = ei;               /* step 6: predecessor edge */
                ws->stamp[v] = gen;
                double f = ng + (heuristic ? heuristic(g, req, v) : 0.0);
                heap_push_or_decrease(ws->heap, v, f);
            }
        }
    }

    if (st == ROUTE_OK) st = reconstruct(g, req, ws, out);   /* steps 8-9 */

    route_workspace_free(own);
    if (st != ROUTE_OK) {
        size_t settled_count = out->nodes_settled;
        route_result_free(out);
        out->nodes_settled = settled_count;
    }
    out->status = st;
    return st;
}
