/*
 * graph_bin.h — preprocessed binary graph file ("graph.bin").
 *
 * Written once by the importer, loaded by the server at start-up, so the
 * national graph is never rebuilt during a request.
 *
 * Layout (little-endian, native struct layout; header records struct sizes
 * and the loader refuses files built with a different ABI):
 *   header (graph_bin_header_t)
 *   nodes[node_count]            graph_node_t
 *   node_flags[node_count]       uint8_t
 *   first_out[node_count + 1]    edge_idx_t
 *   edges[edge_count]            graph_edge_t
 *   shape[shape_count]           graph_point_t
 *   names[names_size]            char
 *   checksum                     uint64_t FNV-1a over everything above
 *
 * Writes go to "<path>.tmp" and are renamed into place, so a running server
 * or a crash never sees a half-written graph (atomic graph replacement).
 */
#ifndef IRA_GRAPH_BIN_H
#define IRA_GRAPH_BIN_H

#include "graph.h"

#define GRAPH_BIN_MAGIC   "IRAGRPH\x01"
#define GRAPH_BIN_VERSION 2u

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t endian_check;      /* 0x01020304 */
    uint32_t sizeof_node;
    uint32_t sizeof_edge;
    uint32_t sizeof_point;
    uint32_t reserved0;
    uint64_t node_count;
    uint64_t edge_count;
    uint64_t shape_count;
    uint64_t names_size;
    int64_t  dataset_time;
    double   max_speed_mps;
    char     dataset[128];
} graph_bin_header_t;

graph_status_t graph_bin_write(const graph_t *g, const char *path);

/* Loads and fully validates a graph file. err (optional) receives a message. */
graph_t *graph_bin_read(const char *path, char *err, size_t err_len);

#endif
