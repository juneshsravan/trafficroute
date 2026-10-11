/* Live-traffic feed ingestion + libcurl fetch (file:// URL, no network). */
#include "app.h"
#include "traffic_feed.h"
#include "graph_io.h"
#include "test_util.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE "../data/sample"

static void test_ingest(void)
{
    graph_load_report_t rep;
    graph_t *g = graph_load_csv(SAMPLE "/nodes.csv", SAMPLE "/edges.csv", &rep);
    spatial_index_t *si = spatial_index_build(g);
    traffic_state_t t;
    traffic_init(&t, g, NULL);
    int64_t now = 1760000000;
    char json[2048];
    snprintf(json, sizeof json,
        "{\"provider\":\"TestFeed\",\"attribution\":\"(c) Test\",\"observations\":["
        "{\"latitude\":17.01,\"longitude\":79.005,\"speed_kmh\":20,\"confidence\":0.9,\"observed_at\":%lld},"
        "{\"latitude\":17.01,\"longitude\":79.005,\"speed_kmh\":20,\"observed_at\":%lld},"
        "{\"latitude\":10.0,\"longitude\":70.0,\"speed_kmh\":20,\"observed_at\":%lld},"
        "{\"latitude\":17.01,\"longitude\":79.005,\"observed_at\":%lld},"
        "\"junk\"]}",
        (long long)now - 30, (long long)now - 7200, (long long)now - 30, (long long)now - 30);
    feed_report_t fr;
    CHECK(traffic_feed_ingest(&t, si, json, strlen(json), now, 40, 1800, &fr));
    CHECK(fr.received == 5);
    CHECK(fr.accepted == 1);
    CHECK(fr.rejected_stale == 1);
    CHECK(fr.rejected_unmatched == 1);
    CHECK(fr.rejected_invalid == 2);
    CHECK(strcmp(t.live_provider, "TestFeed") == 0);
    traffic_summary_t s;
    traffic_summary(&t, now, &s);
    CHECK(s.live_available);
    /* the matched edge is A-E (id 4 or its reverse), 3000 m at 20 km/h = 540 s */
    edge_idx_t e = t.obs[0].edge;
    CHECK(g->edges[e].id == 4 || g->edges[e].id == -4);
    CHECK_NEAR(graph_edge_time(&g->edges[e]), 3000.0 / (20 / 3.6), 0.1);

    /* malformed feeds */
    CHECK(!traffic_feed_ingest(&t, si, "[1]", 3, now, 40, 1800, &fr));
    CHECK(!traffic_feed_ingest(&t, si, "{\"x\":1}", 7, now, 40, 1800, &fr));
    traffic_free(&t);
    spatial_index_free(si);
    graph_free(g);
}

static void test_fetch_file_url_and_app_poll(void)
{
    char path[] = "build/test_feed.json";
    FILE *f = fopen(path, "w");
    long long now = (long long)time(NULL);
    fprintf(f, "{\"provider\":\"LocalDrop\",\"observations\":[{\"latitude\":17.01,\"longitude\":79.005,"
               "\"speed_kmh\":15,\"observed_at\":%lld}]}", now - 5);
    fclose(f);
    char raw_cwd[400];
    CHECK(getcwd(raw_cwd, sizeof raw_cwd) != NULL);
    char cwd[600];
    size_t ci = 0;
    for (char *p = raw_cwd; *p && ci + 4 < sizeof cwd; p++) {
        if (*p == '\\') cwd[ci++] = '/';
        else if (*p == ' ') { memcpy(&cwd[ci], "%20", 3); ci += 3; }
        else cwd[ci++] = *p;
    }
    cwd[ci] = '\0';
    char url[800];
    if (cwd[0] == '/') snprintf(url, sizeof url, "file://%s/%s", cwd, path);
    else snprintf(url, sizeof url, "file:///%s/%s", cwd, path);

    feed_http_config_t fc = {0};
    snprintf(fc.url, sizeof fc.url, "%s", url);
    char *body; size_t len; int ra; char err[256];
    CHECK(feed_http_fetch(&fc, &body, &len, &ra, err, sizeof err) == 200);
    CHECK(body && strstr(body, "LocalDrop"));
    free(body);
    if (cwd[0] == '/') snprintf(fc.url, sizeof fc.url, "file://%s/build/missing.json", cwd);
    else snprintf(fc.url, sizeof fc.url, "file:///%s/build/missing.json", cwd);
    fc.max_retries = 1;
    CHECK(feed_http_fetch(&fc, &body, &len, &ra, err, sizeof err) != 200);

    ira_config_t cfg;
    config_defaults(&cfg);
    cfg.log_level = 3;
    snprintf(cfg.traffic_feed_url, sizeof cfg.traffic_feed_url, "%s", url);
    app_t app;
    CHECK(app_init(&app, &cfg, err, sizeof err));
    CHECK(app_poll_provider_once(&app, &ra));
    traffic_summary_t s;
    traffic_summary(&app.traffic, app_now(), &s);
    CHECK(s.live_available && s.active_by_source[TSRC_LIVE] == 1);
    CHECK(app.provider_last_success > 0);
    app_free(&app);
    remove(path);
}

int main(void)
{
    feed_http_global_init();
    printf("traffic feed tests\n");
    RUN(test_ingest);
    RUN(test_fetch_file_url_and_app_poll);
    feed_http_global_cleanup();
    TEST_SUMMARY();
}
