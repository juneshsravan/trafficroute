/*
 * importer/main.c — `ira_import`: offline administrative tool that turns an
 * OpenStreetMap extract into graph.bin + a SQLite place index.
 *
 *   ira_import --input india-latest.osm.pbf --output india.graph.bin --db india.sqlite
 *
 * The server never runs this during a request.
 */
#define _POSIX_C_SOURCE 200809L
#include "cJSON.h"
#include "database.h"
#include "graph_bin.h"
#include "graph_builder.h"
#include "import_validation.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void usage(void)
{
    puts("ira_import - OpenStreetMap to IndiaRoute AI graph importer\n"
         "\n"
         "Usage: ira_import --input FILE.osm.pbf --output FILE.graph.bin [options]\n"
         "\n"
         "Options:\n"
         "  --db FILE.sqlite        write place search index + import metadata\n"
         "  --report FILE.json      write the import report (default: OUTPUT.report.json)\n"
         "  --max-edge-length M     split road segments longer than M metres (default 1000, 0=off)\n"
         "  --no-tracks             exclude highway=track\n"
         "  --no-early-stop         read the whole file in pass 2 (unsorted input files)\n"
         "  --export-edges          also write road_nodes/road_edges tables (large!)\n"
         "  --print-schema          print the SQLite schema and exit\n"
         "  --verbose               progress output\n");
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *dbpath = NULL, *report_path = NULL;
    bool export_edges = false;
    import_options_t opt;
    import_default_options(&opt);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--print-schema")) { fputs(db_schema_sql(), stdout); return 0; }
        else if (!strcmp(a, "--input") && v) { input = v; i++; }
        else if (!strcmp(a, "--output") && v) { output = v; i++; }
        else if (!strcmp(a, "--db") && v) { dbpath = v; i++; }
        else if (!strcmp(a, "--report") && v) { report_path = v; i++; }
        else if (!strcmp(a, "--max-edge-length") && v) {
            char *end;
            opt.max_edge_m = strtod(v, &end);
            if (*end || opt.max_edge_m < 0 || opt.max_edge_m > 100000) {
                fprintf(stderr, "invalid --max-edge-length\n"); return 2;
            }
            i++;
        }
        else if (!strcmp(a, "--no-tracks")) opt.rules.include_tracks = false;
        else if (!strcmp(a, "--no-early-stop")) opt.early_stop = false;
        else if (!strcmp(a, "--export-edges")) export_edges = true;
        else if (!strcmp(a, "--verbose")) opt.verbose = true;
        else { fprintf(stderr, "unknown or incomplete option: %s\n", a); usage(); return 2; }
    }
    if (!input || !output) { usage(); return 2; }

    struct stat sb;
    if (stat(input, &sb) != 0) { fprintf(stderr, "cannot stat %s: %s\n", input, strerror(errno)); return 1; }

    import_result_t res;
    import_stats_t st;
    time_t started = time(NULL);
    if (!import_osm(input, &opt, &res, &st)) {
        fprintf(stderr, "import failed: %s\n", st.error);
        return 1;
    }
    graph_t *g = res.graph;

    scc_report_t scc;
    if (!graph_mark_main_component(g, &scc)) { fprintf(stderr, "connectivity analysis failed\n"); import_result_free(&res); return 1; }

    char day[16];
    struct tm tmv;
    gmtime_r(&sb.st_mtime, &tmv);
    strftime(day, sizeof day, "%Y-%m-%d", &tmv);
    snprintf(g->dataset, sizeof g->dataset, "osm:%.90s (file date %s)", base_name(input), day);
    g->dataset_time = (int64_t)sb.st_mtime;

    size_t tolls = 0, restricted = 0;
    for (size_t i = 0; i < g->edge_count; i++) {
        if (g->edges[i].flags & EDGE_FLAG_TOLL) tolls++;
        if (g->edges[i].flags & EDGE_FLAG_RESTRICTED) restricted++;
    }
    double main_share = g->node_count ? (double)scc.largest_size / (double)g->node_count : 0;

    /* ---- report ---- */
    cJSON *rep = cJSON_CreateObject();
    cJSON_AddStringToObject(rep, "input", base_name(input));
    cJSON_AddNumberToObject(rep, "input_bytes", (double)sb.st_size);
    cJSON_AddStringToObject(rep, "dataset", g->dataset);
    cJSON *p1 = cJSON_AddObjectToObject(rep, "pass1");
    cJSON_AddNumberToObject(p1, "nodes_seen", (double)st.nodes_seen);
    cJSON_AddNumberToObject(p1, "ways_seen", (double)st.ways_seen);
    cJSON_AddNumberToObject(p1, "routable_ways", (double)st.ways_routable);
    cJSON *dr = cJSON_AddObjectToObject(p1, "ways_dropped");
    for (int r = 1; r < WAY_REASON_COUNT; r++)
        cJSON_AddNumberToObject(dr, way_reason_str((way_reason_t)r), (double)st.ways_dropped[r]);
    cJSON_AddNumberToObject(p1, "referenced_nodes", (double)st.referenced_nodes);
    cJSON_AddNumberToObject(p1, "places", (double)res.place_count);
    cJSON *p2 = cJSON_AddObjectToObject(rep, "pass2");
    cJSON_AddNumberToObject(p2, "coordinates_found", (double)st.coords_found);
    cJSON_AddNumberToObject(p2, "missing_node_refs", (double)st.refs_missing);
    cJSON *gr = cJSON_AddObjectToObject(rep, "graph");
    cJSON_AddNumberToObject(gr, "nodes", (double)g->node_count);
    cJSON_AddNumberToObject(gr, "directed_edges", (double)g->edge_count);
    cJSON_AddNumberToObject(gr, "road_segments", (double)st.segments);
    cJSON_AddNumberToObject(gr, "oneway_segments", (double)st.oneway_segments);
    cJSON_AddNumberToObject(gr, "shape_points", (double)g->shape_count);
    cJSON_AddNumberToObject(gr, "road_km", (double)((long long)(st.road_km * 10)) / 10.0);
    cJSON_AddNumberToObject(gr, "toll_edges", (double)tolls);
    cJSON_AddNumberToObject(gr, "restricted_edges", (double)restricted);
    cJSON_AddNumberToObject(gr, "self_loops_skipped", (double)st.self_loops_skipped);
    cJSON_AddNumberToObject(gr, "zero_length_skipped", (double)st.zero_length_skipped);
    cJSON_AddNumberToObject(gr, "memory_bytes", (double)graph_memory_bytes(g));
    cJSON *cn = cJSON_AddObjectToObject(rep, "connectivity");
    cJSON_AddNumberToObject(cn, "strongly_connected_components", (double)scc.component_count);
    cJSON_AddNumberToObject(cn, "largest_component_nodes", (double)scc.largest_size);
    cJSON_AddNumberToObject(cn, "largest_component_share", (double)((long long)(main_share * 10000)) / 10000.0);
    cJSON_AddNumberToObject(cn, "singleton_components", (double)scc.singleton_components);
    cJSON_AddNumberToObject(cn, "dead_end_nodes", (double)scc.zero_out_degree);
    cJSON_AddNumberToObject(cn, "source_only_nodes", (double)scc.zero_in_degree);
    cJSON *tm = cJSON_AddObjectToObject(rep, "seconds");
    cJSON_AddNumberToObject(tm, "pass1", st.seconds_pass1);
    cJSON_AddNumberToObject(tm, "pass2", st.seconds_pass2);
    cJSON_AddNumberToObject(tm, "build", st.seconds_build);
    cJSON *warn = cJSON_AddArrayToObject(rep, "warnings");
    if (st.refs_missing)
        cJSON_AddItemToArray(warn, cJSON_CreateString(
            "Some ways reference nodes outside the extract; those ways were cut at the boundary."));
    if (main_share < 0.9)
        cJSON_AddItemToArray(warn, cJSON_CreateString(
            "Largest connected component holds < 90% of nodes: many routes may be impossible."));
    if (res.place_count == 0)
        cJSON_AddItemToArray(warn, cJSON_CreateString("No place nodes found: location search will be empty."));
    cJSON_AddItemToArray(warn, cJSON_CreateString("Turn restrictions (OSM relations) are not yet applied."));

    /* ---- write graph ---- */
    double t0 = (double)time(NULL);
    graph_status_t gs = graph_bin_write(g, output);
    if (gs != GRAPH_OK) {
        fprintf(stderr, "writing %s failed: %s\n", output, graph_status_str(gs));
        cJSON_Delete(rep);
        import_result_free(&res);
        return 1;
    }
    cJSON_AddNumberToObject(tm, "write", (double)time(NULL) - t0);

    /* ---- database ---- */
    int status = 0;
    if (dbpath) {
        char err[512];
        ira_db_t *db = db_open(dbpath, true, err, sizeof err);
        if (!db || !db_init_schema(db, err, sizeof err)) {
            fprintf(stderr, "database error: %s\n", err);
            status = 1;
        } else {
            /* One transaction: a fresh import replaces the previous place index
             * (and edge export) atomically. */
            bool ok = db_begin(db) && db_clear_import_tables(db);
            for (size_t i = 0; ok && i < res.place_count; i++) {
                import_place_t *p = &res.places[i];
                db_place_t dp = { .osm_id = p->osm_id, .name = p->name, .name_en = p->name_en,
                                  .type = p->type, .type_rank = p->rank, .state = p->state,
                                  .district = p->district, .near = p->near, .lat = p->lat,
                                  .lon = p->lon, .population = p->population };
                ok = db_insert_place(db, &dp);
            }
            if (ok && export_edges) {
                for (size_t i = 0; ok && i < g->node_count; i++)
                    ok = db_insert_road_node(db, (int64_t)i, g->nodes[i].lat, g->nodes[i].lon, g->nodes[i].id);
                for (size_t i = 0; ok && i < g->edge_count; i++) {
                    const graph_edge_t *e = &g->edges[i];
                    ok = db_insert_road_edge(db, (int64_t)i, e->from, e->to, e->distance_m, e->base_time_s,
                                             road_class_name((road_class_t)e->road_class), e->speed_limit_kmh,
                                             e->flags, graph_find_reverse(g, (edge_idx_t)i) == IRA_INVALID_IDX,
                                             e->geom_offset);
                }
            }
            if (ok) ok = db_commit(db) && db_rebuild_place_index(db);
            else db_rollback(db);

            db_import_meta_t m;
            memset(&m, 0, sizeof m);
            snprintf(m.dataset_version, sizeof m.dataset_version, "%.60s", day);
            snprintf(m.source, sizeof m.source, "%s", g->dataset);
            m.import_time = (int64_t)started;
            m.node_count = (int64_t)g->node_count;
            m.edge_count = (int64_t)g->edge_count;
            snprintf(m.import_status, sizeof m.import_status, "%s",
                     !ok ? "failed" : cJSON_GetArraySize(warn) > 1 ? "warnings" : "ok");
            char *rj = cJSON_PrintUnformatted(rep);
            if (rj) { snprintf(m.report_json, sizeof m.report_json, "%s", rj); free(rj); }
            if (strlen(m.report_json) >= sizeof m.report_json - 1) m.report_json[0] = '\0';
            db_insert_import_meta(db, &m);
            if (!ok) { fprintf(stderr, "database write failed\n"); status = 1; }
            else printf("Place index: %zu places written to %s\n", res.place_count, dbpath);
        }
        db_close(db);
    }

    char rp[4096];
    if (!report_path) { snprintf(rp, sizeof rp, "%s.report.json", output); report_path = rp; }
    char *pretty = cJSON_Print(rep);
    FILE *rf = fopen(report_path, "w");
    if (rf && pretty) { fputs(pretty, rf); fputc('\n', rf); }
    if (rf) fclose(rf);
    free(pretty);

    printf("Imported %s\n", input);
    printf("  graph: %zu nodes, %zu directed edges, %.1f road-km, %zu shape points\n",
           g->node_count, g->edge_count, st.road_km, g->shape_count);
    printf("  largest strongly connected component: %.1f%% of nodes (%zu components)\n",
           main_share * 100.0, scc.component_count);
    printf("  time: pass1 %.1fs, pass2 %.1fs, build %.1fs\n", st.seconds_pass1, st.seconds_pass2, st.seconds_build);
    printf("  wrote %s and %s\n", output, report_path);

    cJSON_Delete(rep);
    import_result_free(&res);
    return status;
}
