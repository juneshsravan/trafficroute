#include "import_validation.h"

#include <stdlib.h>
#include <string.h>

#define UNVISITED UINT32_MAX

bool graph_mark_main_component(graph_t *g, scc_report_t *rep)
{
    memset(rep, 0, sizeof *rep);
    if (!g || !g->finalized) return false;
    size_t V = g->node_count;
    if (V == 0) return true;

    uint32_t *index = malloc(V * sizeof *index);
    uint32_t *low = malloc(V * sizeof *low);
    uint32_t *comp = malloc(V * sizeof *comp);
    uint32_t *stack = malloc(V * sizeof *stack);       /* Tarjan node stack */
    uint32_t *cs_node = malloc(V * sizeof *cs_node);   /* explicit call stack */
    edge_idx_t *cs_edge = malloc(V * sizeof *cs_edge);
    uint8_t *on_stack = calloc(V, 1);
    uint32_t *in_deg = calloc(V, sizeof *in_deg);
    if (!index || !low || !comp || !stack || !cs_node || !cs_edge || !on_stack || !in_deg) {
        free(index); free(low); free(comp); free(stack); free(cs_node); free(cs_edge);
        free(on_stack); free(in_deg);
        return false;
    }
    for (size_t i = 0; i < V; i++) index[i] = UNVISITED;
    for (size_t i = 0; i < g->edge_count; i++) in_deg[g->edges[i].to]++;

    uint32_t next_index = 0, sp = 0, ncomp = 0;
    uint32_t best_comp = 0, best_size = 0;

    for (size_t root = 0; root < V; root++) {
        if (index[root] != UNVISITED) continue;
        uint32_t csp = 0;
        cs_node[csp] = (uint32_t)root;
        cs_edge[csp] = graph_out_begin(g, (node_idx_t)root);
        csp++;
        index[root] = low[root] = next_index++;
        stack[sp++] = (uint32_t)root;
        on_stack[root] = 1;

        while (csp > 0) {
            uint32_t v = cs_node[csp - 1];
            edge_idx_t ei = cs_edge[csp - 1];
            if (ei < graph_out_end(g, v)) {
                cs_edge[csp - 1]++;
                uint32_t w = g->edges[ei].to;
                if (index[w] == UNVISITED) {             /* "recurse" into w */
                    index[w] = low[w] = next_index++;
                    stack[sp++] = w;
                    on_stack[w] = 1;
                    cs_node[csp] = w;
                    cs_edge[csp] = graph_out_begin(g, w);
                    csp++;
                } else if (on_stack[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
                continue;
            }
            /* all edges of v done: pop frame */
            if (low[v] == index[v]) {                    /* v is an SCC root */
                uint32_t size = 0, w;
                do {
                    w = stack[--sp];
                    on_stack[w] = 0;
                    comp[w] = ncomp;
                    size++;
                } while (w != v);
                if (size > best_size) { best_size = size; best_comp = ncomp; }
                if (size == 1) rep->singleton_components++;
                ncomp++;
            }
            csp--;
            if (csp > 0) {
                uint32_t parent = cs_node[csp - 1];
                if (low[v] < low[parent]) low[parent] = low[v];
            }
        }
    }

    for (size_t i = 0; i < V; i++) {
        if (comp[i] == best_comp) g->node_flags[i] |= NODE_FLAG_MAIN_COMPONENT;
        else g->node_flags[i] &= (uint8_t)~NODE_FLAG_MAIN_COMPONENT;
        size_t outd = graph_out_end(g, (node_idx_t)i) - graph_out_begin(g, (node_idx_t)i);
        if (outd == 0 && in_deg[i] == 0) rep->nodes_without_edges++;
        else if (outd == 0) rep->zero_out_degree++;
        else if (in_deg[i] == 0) rep->zero_in_degree++;
    }
    rep->component_count = ncomp;
    rep->largest_size = best_size;

    free(index); free(low); free(comp); free(stack); free(cs_node); free(cs_edge);
    free(on_stack); free(in_deg);
    return true;
}
