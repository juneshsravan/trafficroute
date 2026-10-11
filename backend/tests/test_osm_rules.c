/*
 * OSM tag rules (pure functions) + an end-to-end import of the hand-written
 * data/test_osm/rules_test.osm with exact expected edges.
 */
#include "graph_builder.h"
#include "import_validation.h"
#include "osm_parser.h"
#include "test_util.h"

#include <string.h>

#define T(k, v) { (k), (v) }

static void test_classify(void)
{
    osm_rules_t rules = { .include_tracks = true };
    way_attrs_t a;
    readosm_tag t1[] = { T("highway", "primary"), T("name", "MG Road"), T("ref", "SH 1") };
    CHECK(osm_classify_way(t1, 3, &rules, &a) == WAY_OK);
    CHECK(a.cls == ROAD_PRIMARY && a.oneway == 0 && !a.toll);
    CHECK(strcmp(a.name, "MG Road (SH 1)") == 0);
    CHECK_NEAR(a.speed_kmh, 55, 1e-9);

    readosm_tag t2[] = { T("highway", "footway") };
    CHECK(osm_classify_way(t2, 1, &rules, &a) == WAY_NOT_ROUTABLE_TYPE);
    readosm_tag t3[] = { T("building", "yes") };
    CHECK(osm_classify_way(t3, 1, &rules, &a) == WAY_NOT_HIGHWAY);
    readosm_tag t4[] = { T("highway", "service"), T("access", "private") };
    CHECK(osm_classify_way(t4, 2, &rules, &a) == WAY_NO_ACCESS);
    /* more specific tag wins: motorcar=yes overrides access=no */
    readosm_tag t5[] = { T("highway", "service"), T("access", "no"), T("motorcar", "yes") };
    CHECK(osm_classify_way(t5, 3, &rules, &a) == WAY_OK);
    readosm_tag t6[] = { T("highway", "residential"), T("access", "destination") };
    CHECK(osm_classify_way(t6, 2, &rules, &a) == WAY_OK && a.restricted);
    readosm_tag t7[] = { T("highway", "motorway") };
    CHECK(osm_classify_way(t7, 1, &rules, &a) == WAY_OK && a.oneway == 1);   /* implied */
    readosm_tag t8[] = { T("highway", "secondary"), T("junction", "roundabout") };
    CHECK(osm_classify_way(t8, 2, &rules, &a) == WAY_OK && a.oneway == 1);
    readosm_tag t9[] = { T("highway", "tertiary"), T("oneway", "-1") };
    CHECK(osm_classify_way(t9, 2, &rules, &a) == WAY_OK && a.oneway == -1);
    readosm_tag t10[] = { T("highway", "motorway"), T("oneway", "no") };
    CHECK(osm_classify_way(t10, 2, &rules, &a) == WAY_OK && a.oneway == 0);
    readosm_tag t11[] = { T("highway", "trunk"), T("toll", "yes"), T("maxspeed", "100") };
    CHECK(osm_classify_way(t11, 3, &rules, &a) == WAY_OK && a.toll);
    CHECK_NEAR(a.speed_kmh, 70 * 1.25, 1e-9);          /* capped at default +25 % */
    readosm_tag t12[] = { T("highway", "primary"), T("maxspeed", "30") };
    CHECK(osm_classify_way(t12, 2, &rules, &a) == WAY_OK);
    CHECK_NEAR(a.speed_kmh, 30, 1e-9);                 /* legal limit below default */
    readosm_tag t13[] = { T("highway", "track") };
    osm_rules_t notracks = { .include_tracks = false };
    CHECK(osm_classify_way(t13, 1, &notracks, &a) == WAY_TRACK_EXCLUDED);
    CHECK(osm_classify_way(t13, 1, &rules, &a) == WAY_OK && a.cls == ROAD_TRACK);
    readosm_tag t14[] = { T("highway", "primary"), T("area", "yes") };
    CHECK(osm_classify_way(t14, 2, &rules, &a) == WAY_AREA);
    readosm_tag t15[] = { T("highway", "primary_link") };
    CHECK(osm_classify_way(t15, 1, &rules, &a) == WAY_OK && a.is_link);
    CHECK_NEAR(a.speed_kmh, 55 * 0.75, 1e-9);
}

static void test_maxspeed(void)
{
    CHECK_NEAR(osm_parse_maxspeed("50"), 50, 1e-9);
    CHECK_NEAR(osm_parse_maxspeed("60 km/h"), 60, 1e-9);
    CHECK_NEAR(osm_parse_maxspeed("30 mph"), 30 * 1.609344, 1e-9);
    CHECK(osm_parse_maxspeed("IN:urban") == 0);
    CHECK(osm_parse_maxspeed("none") == 0);
    CHECK(osm_parse_maxspeed("999") == 0);
    CHECK(osm_parse_maxspeed(NULL) == 0);
    CHECK(osm_place_rank("city") < osm_place_rank("village"));
    CHECK(osm_place_rank("shop") == -1);
}

#ifndef OSM_TEST_DIR
#define OSM_TEST_DIR "../data/test_osm"
#endif

/* Finds edge from OSM node a to OSM node b. */
static const graph_edge_t *edge_between(const graph_t *g, int64_t a, int64_t b)
{
    for (size_t i = 0; i < g->edge_count; i++) {
        const graph_edge_t *e = &g->edges[i];
        if (g->nodes[e->from].id == a && g->nodes[e->to].id == b) return e;
    }
    return NULL;
}

static void test_import_rules_file(void)
{
    import_options_t opt;
    import_default_options(&opt);
    opt.early_stop = false;
    import_result_t res;
    import_stats_t st;
    bool ok = import_osm(OSM_TEST_DIR "/rules_test.osm", &opt, &res, &st);
    CHECK(ok);
    if (!ok) { fprintf(stderr, "  import error: %s\n", st.error); return; }
    graph_t *g = res.graph;

    CHECK(g->node_count == 6);          /* 1,3,4,7,8,10 — node 2 is only a shape point */
    CHECK(g->edge_count == 8);
    CHECK(st.ways_dropped[WAY_NOT_ROUTABLE_TYPE] == 1);
    CHECK(st.ways_dropped[WAY_NO_ACCESS] == 1);
    CHECK(st.ways_dropped[WAY_NOT_HIGHWAY] == 1);

    const graph_edge_t *e13 = edge_between(g, 1, 3);
    CHECK(e13 && edge_between(g, 3, 1));
    if (e13) {
        CHECK(e13->geom_count == 1);    /* node 2 kept as geometry */
        CHECK(strcmp(graph_edge_name(g, e13), "Test Main Road (SH 99)") == 0);
        /* 1->2->3 is 2 x 0.001 deg of longitude at 12.97 N */
        CHECK_NEAR(e13->distance_m, 2 * 108.36, 1.0);
        CHECK(e13->speed_limit_kmh == 40);
    }
    CHECK(edge_between(g, 3, 4) && !edge_between(g, 4, 3));   /* oneway=yes */
    CHECK(edge_between(g, 8, 7) && !edge_between(g, 7, 8));   /* oneway=-1 */
    const graph_edge_t *e47 = edge_between(g, 4, 7);
    CHECK(e47 && (e47->flags & EDGE_FLAG_TOLL));
    const graph_edge_t *e810 = edge_between(g, 8, 10);
    CHECK(e810 && (e810->flags & EDGE_FLAG_RESTRICTED));
    CHECK(!edge_between(g, 4, 6));                           /* private dropped */

    CHECK(res.place_count == 1);
    if (res.place_count) {
        CHECK(strcmp(res.places[0].name, "Testpur") == 0);
        CHECK(strcmp(res.places[0].state, "Karnataka") == 0);
        CHECK(res.places[0].population == 1200);
    }

    scc_report_t scc;
    CHECK(graph_mark_main_component(g, &scc));
    /* 1<->3 form an SCC; so do 4<->7. 8 and 10 are... 8->7 only, 8<->10. */
    CHECK(scc.largest_size == 2);
    import_result_free(&res);
}

int main(void)
{
    printf("osm rule tests\n");
    RUN(test_classify);
    RUN(test_maxspeed);
    RUN(test_import_rules_file);
    TEST_SUMMARY();
}
