/*
 * import_validation.h — connectivity analysis of an imported graph.
 *
 * Finds strongly connected components (iterative Tarjan, O(V+E), no
 * recursion so it cannot overflow the stack on national-scale graphs) and
 * flags nodes of the largest one with NODE_FLAG_MAIN_COMPONENT. Snapping
 * prefers those nodes, so a click next to an isolated parking-lot fragment
 * does not produce "no route".
 */
#ifndef IRA_IMPORT_VALIDATION_H
#define IRA_IMPORT_VALIDATION_H

#include "graph.h"

typedef struct {
    size_t component_count;
    size_t largest_size;
    size_t singleton_components;
    size_t nodes_without_edges;
    size_t zero_out_degree;      /* dead ends (can enter, cannot leave) */
    size_t zero_in_degree;       /* can leave, cannot enter */
} scc_report_t;

bool graph_mark_main_component(graph_t *g, scc_report_t *rep);

#endif
