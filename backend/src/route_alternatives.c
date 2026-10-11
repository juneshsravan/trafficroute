/*
 * route_alternatives.c — penalty-method alternatives (see header).
 */
#include "route_alternatives.h"

#include <stdlib.h>
#include <string.h>

void alternatives_default_params(alternatives_params_t *p)
{
    p->penalty_factor = 1.4f;
    p->max_overlap = 0.6;
    p->max_stretch = 1.5;
}

static int cmp_edge(const void *a, const void *b)
{
    edge_idx_t x = *(const edge_idx_t *)a, y = *(const edge_idx_t *)b;
    return (x > y) - (x < y);
}

double route_overlap_fraction(const graph_t *g, const route_result_t *a,
                              const route_result_t *b)
{
    if (a->distance_m <= 0 || b->edge_count == 0) return 0.0;
    edge_idx_t *sorted = malloc(b->edge_count * sizeof *sorted);
    if (!sorted) return 1.0;   /* be conservative: treat as identical */
    memcpy(sorted, b->edges, b->edge_count * sizeof *sorted);
    qsort(sorted, b->edge_count, sizeof *sorted, cmp_edge);

    double shared = 0.0;
    for (size_t i = 0; i < a->edge_count; i++) {
        edge_idx_t e = a->edges[i];
        if (bsearch(&e, sorted, b->edge_count, sizeof *sorted, cmp_edge))
            shared += (double)g->edges[e].distance_m;
    }
    free(sorted);
    return shared / a->distance_m;
}

static void penalise(route_penalty_t *p, const route_result_t *r, float factor)
{
    for (size_t i = 0; i < r->edge_count; i++) route_penalty_multiply(p, r->edges[i], factor);
}

route_status_t route_with_alternatives(const graph_t *g, const route_request_t *req,
                                       route_workspace_t *ws,
                                       const alternatives_params_t *params,
                                       size_t max_alts, route_result_t *best,
                                       route_result_t *alts, size_t *alt_count)
{
    alternatives_params_t defaults;
    if (!params) { alternatives_default_params(&defaults); params = &defaults; }
    if (max_alts > MAX_ALTERNATIVES) max_alts = MAX_ALTERNATIVES;
    *alt_count = 0;

    route_request_t base = *req;
    base.penalty = NULL;
    route_status_t st = route_compute_ws(g, &base, ws, best);
    if (st != ROUTE_OK || max_alts == 0 || best->edge_count == 0) return st;

    route_penalty_t pen;
    if (!route_penalty_init(&pen, best->edge_count * 4)) return st;  /* best route still valid */
    penalise(&pen, best, params->penalty_factor);

    route_request_t alt_req = base;
    alt_req.penalty = &pen;
    size_t attempts = max_alts * 3;

    for (size_t a = 0; a < attempts && *alt_count < max_alts; a++) {
        route_result_t cand;
        if (route_compute_ws(g, &alt_req, ws, &cand) != ROUTE_OK) {
            route_result_free(&cand);
            break;
        }
        bool accept = cand.total_cost <= best->total_cost * params->max_stretch;
        if (accept && route_overlap_fraction(g, &cand, best) > params->max_overlap) accept = false;
        for (size_t j = 0; accept && j < *alt_count; j++)
            if (route_overlap_fraction(g, &cand, &alts[j]) > params->max_overlap) accept = false;

        penalise(&pen, &cand, params->penalty_factor);
        if (accept) alts[(*alt_count)++] = cand;
        else route_result_free(&cand);
    }
    route_penalty_free(&pen);
    return st;
}
