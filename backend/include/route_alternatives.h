/*
 * route_alternatives.h — alternative routes by the PENALTY METHOD.
 *
 * After the best route is found, every edge it uses gets its cost multiplied
 * (default x1.4) and the search is repeated, which pushes the next search
 * onto different roads. A candidate is accepted only if:
 *   - it shares at most `max_overlap` (default 60 %) of its length with every
 *     route already accepted (so it is meaningfully different), and
 *   - its REAL cost is at most `max_stretch` (default 1.5x) the best cost.
 * Each candidate (accepted or not) is penalised too, so the loop makes
 * progress. All candidates come from the same search with the same filters
 * (closures, tolls, restrictions), so they are valid routes by construction.
 *
 * Trade-off: each alternative costs one full search (k alternatives ~ up to
 * 3k searches), and penalties can produce slightly odd detours. More
 * sophisticated methods (plateau / via-node methods) give better quality but
 * need two-direction search trees; documented as a later improvement.
 */
#ifndef IRA_ROUTE_ALTERNATIVES_H
#define IRA_ROUTE_ALTERNATIVES_H

#include "routing.h"

#define MAX_ALTERNATIVES 3

typedef struct {
    float  penalty_factor;   /* default 1.4 */
    double max_overlap;      /* default 0.6 */
    double max_stretch;      /* default 1.5 */
} alternatives_params_t;

void alternatives_default_params(alternatives_params_t *p);

/* Computes best route into *best and up to max_alts alternatives into alts[].
 * Returns the status of the best-route search. Caller frees every result. */
route_status_t route_with_alternatives(const graph_t *g, const route_request_t *req,
                                       route_workspace_t *ws,
                                       const alternatives_params_t *params,
                                       size_t max_alts, route_result_t *best,
                                       route_result_t *alts, size_t *alt_count);

/* Fraction of `a`'s length that also lies on `b` (by edge, 0..1). */
double route_overlap_fraction(const graph_t *g, const route_result_t *a,
                              const route_result_t *b);

#endif
