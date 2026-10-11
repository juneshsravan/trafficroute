/* graph.bin round trip and corruption detection. */
#include "graph_bin.h"
#include "graph_io.h"
#include "test_util.h"

#include <string.h>

#define SAMPLE "../data/sample"

static const char *TMP = "build/test_graph.bin";

static graph_t *load_sample(void)
{
    graph_load_report_t rep;
    graph_t *g = graph_load_csv(SAMPLE "/nodes.csv", SAMPLE "/edges.csv", &rep);
    if (!g) { fprintf(stderr, "%s\n", rep.error); exit(1); }
    return g;
}

static void test_round_trip(void)
{
    graph_t *g = load_sample();
    /* runtime state must not be persisted */
    graph_set_traffic_time(g, 0, 9999);
    graph_set_closed(g, 1, true);
    CHECK(graph_bin_write(g, TMP) == GRAPH_OK);
    char err[256];
    graph_t *h = graph_bin_read(TMP, err, sizeof err);
    CHECK(h != NULL);
    if (!h) { fprintf(stderr, "  %s\n", err); graph_free(g); return; }
    CHECK(h->node_count == g->node_count && h->edge_count == g->edge_count);
    CHECK(h->names_size == g->names_size);
    CHECK(strcmp(h->dataset, g->dataset) == 0);
    for (size_t i = 0; i < g->node_count; i++) {
        CHECK(h->nodes[i].id == g->nodes[i].id);
        CHECK(h->node_flags[i] == g->node_flags[i]);
    }
    for (size_t i = 0; i < g->edge_count; i++) {
        CHECK(h->edges[i].id == g->edges[i].id);
        CHECK(h->edges[i].distance_m == g->edges[i].distance_m);
        CHECK(strcmp(graph_edge_name(h, &h->edges[i]), graph_edge_name(g, &g->edges[i])) == 0);
    }
    CHECK(h->edges[0].traffic_time_s == 0.0f);
    CHECK(!(h->edges[1].flags & EDGE_FLAG_CLOSED));
    graph_free(h);
    graph_free(g);
}

static void corrupt_byte(long offset)
{
    FILE *f = fopen(TMP, "r+b");
    fseek(f, offset, SEEK_SET);
    int c = fgetc(f);
    fseek(f, offset, SEEK_SET);
    fputc(c ^ 0x55, f);
    fclose(f);
}

static void test_corruption_detected(void)
{
    graph_t *g = load_sample();
    char err[256];
    CHECK(graph_bin_write(g, TMP) == GRAPH_OK);
    corrupt_byte((long)sizeof(graph_bin_header_t) + 40);
    CHECK(graph_bin_read(TMP, err, sizeof err) == NULL);
    CHECK(strstr(err, "checksum") != NULL);

    CHECK(graph_bin_write(g, TMP) == GRAPH_OK);
    corrupt_byte(0);
    CHECK(graph_bin_read(TMP, err, sizeof err) == NULL);
    CHECK(strstr(err, "not a graph.bin") != NULL);

    CHECK(graph_bin_write(g, TMP) == GRAPH_OK);
    /* truncate: rewrite all but the last 100 bytes */
    FILE *f = fopen(TMP, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size);
    CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    f = fopen(TMP, "wb");
    fwrite(buf, 1, (size_t)(size - 100), f);
    fclose(f);
    free(buf);
    CHECK(graph_bin_read(TMP, err, sizeof err) == NULL);

    CHECK(graph_bin_read("build/does-not-exist.bin", err, sizeof err) == NULL);
    remove(TMP);
    graph_free(g);
}

int main(void)
{
    printf("graph.bin tests\n");
    RUN(test_round_trip);
    RUN(test_corruption_detected);
    TEST_SUMMARY();
}
