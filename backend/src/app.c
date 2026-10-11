/*
 * app.c — server state construction, workspace pool, traffic provider loop.
 */
#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "graph_bin.h"
#include "graph_io.h"
#include "log.h"
#include "tile_cache.h"
#include "traffic_feed.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t app_now(void) { return (int64_t)time(NULL); }

double app_monotonic_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

typedef struct {
    app_t   *app;
    size_t   restored, dropped;
} restore_ctx_t;

/* Re-attach persisted incidents to the (possibly re-imported) graph by
 * coordinate, since edge indices change between graph versions. */
static void restore_incident(void *vctx, const incident_t *in)
{
    restore_ctx_t *c = vctx;
    app_t *app = c->app;
    edge_idx_t e;
    double d;
    if (!spatial_nearest_edge(app->si, app->g, in->lat, in->lon, 100.0, &e, &d)) {
        c->dropped++;
        return;
    }
    app->traffic.next_inc_id = in->id;      /* keep the persisted id */
    if (traffic_add_incident(&app->traffic, in->type, in->status, in->lat, in->lon, e,
                             in->reverse_edge != IRA_INVALID_IDX, in->reported_at,
                             in->expires_at, in->simulated, in->description, NULL) == 0)
        c->restored++;
    else
        c->dropped++;
}

bool app_init(app_t *app, const ira_config_t *cfg, char *err, size_t err_len)
{
    memset(app, 0, sizeof *app);
    app->cfg = *cfg;
    log_set_level(cfg->log_level);
    app->started_at = app_now();
    pthread_rwlock_init(&app->lock, NULL);
    pthread_mutex_init(&app->ws_mu, NULL);
    pthread_cond_init(&app->ws_cv, NULL);

    /* ---- graph ---- */
    double t0 = app_monotonic_s();
    if (cfg->graph_path[0]) {
        char gerr[256];
        app->g = graph_bin_read(cfg->graph_path, gerr, sizeof gerr);
        if (!app->g) { set_err(err, err_len, "graph load failed: %s", gerr); return false; }
        app->osm_data = strncmp(app->g->dataset, "osm:", 4) == 0;
    } else {
        char np[1100], ep[1100];
        snprintf(np, sizeof np, "%s/nodes.csv", cfg->sample_dir);
        snprintf(ep, sizeof ep, "%s/edges.csv", cfg->sample_dir);
        graph_load_report_t rep;
        app->g = graph_load_csv(np, ep, &rep);
        if (!app->g) { set_err(err, err_len, "sample load failed: %s", rep.error); return false; }
        app->sample_mode = true;
        LOGW("No IRA_GRAPH_PATH set: serving the SYNTHETIC sample network from %s "
             "(not real roads).", cfg->sample_dir);
    }
    size_t main_nodes = 0;
    for (size_t i = 0; i < app->g->node_count; i++)
        main_nodes += (app->g->node_flags[i] & NODE_FLAG_MAIN_COMPONENT) != 0;
    app->main_component_share = app->g->node_count ? (double)main_nodes / (double)app->g->node_count : 0;
    LOGI("Graph loaded: %zu nodes, %zu edges, %zu shape points in %.2f s (%s)",
         app->g->node_count, app->g->edge_count, app->g->shape_count,
         app_monotonic_s() - t0, app->g->dataset);

    /* ---- spatial index ---- */
    t0 = app_monotonic_s();
    app->si = spatial_index_build(app->g);
    if (!app->si) { set_err(err, err_len, "spatial index build failed (out of memory?)"); return false; }
    LOGI("Spatial index built in %.2f s (%.1f MB)", app_monotonic_s() - t0,
         (double)spatial_index_memory_bytes(app->si) / 1048576.0);

    /* ---- database ---- */
    char derr[512];
    const char *dbp = cfg->db_path[0] ? cfg->db_path : ":memory:";
    app->db = db_open(dbp, true, derr, sizeof derr);
    if (!app->db || !db_init_schema(app->db, derr, sizeof derr)) {
        set_err(err, err_len, "%s", derr);
        return false;
    }
    if (app->sample_mode && db_place_count(app->db) == 0) {
        char pp[1100];
        snprintf(pp, sizeof pp, "%s/places.csv", cfg->sample_dir);
        if (file_exists(pp) && !db_load_places_csv(app->db, pp, derr, sizeof derr))
            LOGW("places.csv not loaded: %s", derr);
    }
    int64_t places = db_place_count(app->db);
    if (places <= 0)
        LOGW("Place search index is empty — location search will return no results. "
             "Run the importer with --db to build it.");
    else
        LOGI("Place search index: %lld places", (long long)places);

    /* ---- traffic ---- */
    traffic_params_t tp;
    traffic_default_params(&tp);
    tp.accident_delay_factor = cfg->accident_delay_factor;
    tp.roadwork_delay_factor = cfg->roadwork_delay_factor;
    if (!traffic_init(&app->traffic, app->g, &tp)) { set_err(err, err_len, "traffic init failed"); return false; }
    restore_ctx_t rc = { .app = app };
    uint64_t max_id = db_load_incidents(app->db, app_now(), restore_incident, &rc);
    app->traffic.next_inc_id = max_id + 1;
    if (rc.restored || rc.dropped)
        LOGI("Incidents restored: %zu (dropped %zu that no longer match a road)", rc.restored, rc.dropped);

    /* ---- workspaces ---- */
    app->ws_count = cfg->route_workers;
    app->ws = calloc((size_t)app->ws_count, sizeof *app->ws);
    app->ws_busy = calloc((size_t)app->ws_count, sizeof *app->ws_busy);
    if (!app->ws || !app->ws_busy) { set_err(err, err_len, "out of memory"); return false; }
    for (int i = 0; i < app->ws_count; i++) {
        app->ws[i] = route_workspace_create(app->g->node_count);
        if (!app->ws[i]) { set_err(err, err_len, "out of memory creating search workspaces"); return false; }
    }

    /* ---- rate limiting ---- */
    app->rl = rl_create(8192);
    if (!app->rl) { set_err(err, err_len, "out of memory"); return false; }
    double gpm = cfg->rate_general_per_min, rpm = cfg->rate_route_per_min, iph = cfg->rate_incident_per_hour;
    rl_configure(app->rl, RL_GENERAL, gpm, 60, gpm / 2 > 10 ? gpm / 2 : 10);
    rl_configure(app->rl, RL_ROUTE, rpm, 60, rpm / 4 > 5 ? rpm / 4 : 5);
    rl_configure(app->rl, RL_INCIDENT, iph, 3600, iph / 3 > 2 ? iph / 3 : 2);

    /* ---- map tile cache ---- */
    if (cfg->tile_cache_dir[0]) {
        app->tile_cache = tile_cache_create(cfg->tile_cache_dir);
        LOGI("Map tiles: proxied and cached in %s", cfg->tile_cache_dir);
    }

    if (!config_admin_enabled(cfg))
        LOGW("Admin API disabled: set IRA_ADMIN_TOKEN (>= 16 characters) to enable it.");
    return true;
}

void app_free(app_t *app)
{
    app_stop_provider(app);
    for (int i = 0; app->ws && i < app->ws_count; i++) route_workspace_free(app->ws[i]);
    free(app->ws);
    free(app->ws_busy);
    traffic_free(&app->traffic);
    db_close(app->db);
    spatial_index_free(app->si);
    graph_free(app->g);
    rl_free(app->rl);
    tile_cache_free(app->tile_cache);
    pthread_rwlock_destroy(&app->lock);
    pthread_mutex_destroy(&app->ws_mu);
    pthread_cond_destroy(&app->ws_cv);
    memset(app, 0, sizeof *app);
}

route_workspace_t *app_acquire_workspace(app_t *app, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&app->ws_mu);
    route_workspace_t *got = NULL;
    for (;;) {
        for (int i = 0; i < app->ws_count; i++)
            if (!app->ws_busy[i]) { app->ws_busy[i] = true; got = app->ws[i]; break; }
        if (got) break;
        if (pthread_cond_timedwait(&app->ws_cv, &app->ws_mu, &deadline) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&app->ws_mu);
    return got;
}

void app_release_workspace(app_t *app, route_workspace_t *ws)
{
    pthread_mutex_lock(&app->ws_mu);
    for (int i = 0; i < app->ws_count; i++)
        if (app->ws[i] == ws) app->ws_busy[i] = false;
    pthread_cond_signal(&app->ws_cv);
    pthread_mutex_unlock(&app->ws_mu);
}

void app_maybe_expire(app_t *app, int64_t now)
{
    long long last = atomic_load(&app->last_expire_s);
    if (now <= last) return;
    if (!atomic_compare_exchange_strong(&app->last_expire_s, &last, (long long)now)) return;
    pthread_rwlock_wrlock(&app->lock);
    size_t n = traffic_expire(&app->traffic, now);
    pthread_rwlock_unlock(&app->lock);
    if (n) LOGI("Expired %zu traffic observations/incidents", n);
}

/* ---------------- live provider ---------------- */

bool app_poll_provider_once(app_t *app, int *retry_after_s)
{
    feed_http_config_t fc;
    memset(&fc, 0, sizeof fc);
    snprintf(fc.url, sizeof fc.url, "%s", app->cfg.traffic_feed_url);
    snprintf(fc.api_key, sizeof fc.api_key, "%s", app->cfg.traffic_api_key);
    fc.timeout_s = 10;
    fc.max_retries = 3;

    char *body = NULL, err[256] = "";
    size_t len = 0;
    long status = feed_http_fetch(&fc, &body, &len, retry_after_s, err, sizeof err);
    int64_t now = app_now();

    pthread_rwlock_wrlock(&app->lock);
    app->provider_last_attempt = now;
    bool ok = false;
    if (status == 200 && body) {
        feed_report_t rep;
        ok = traffic_feed_ingest(&app->traffic, app->si, body, len, now, 40.0, 1800, &rep);
        if (ok) {
            app->provider_last_success = now;
            app->provider_last_error[0] = '\0';
            if (!app->traffic.live_provider[0] || !rep.provider[0])
                snprintf(app->traffic.live_provider, sizeof app->traffic.live_provider, "%s",
                         app->cfg.traffic_provider);
            if (!rep.attribution[0] && app->cfg.traffic_attribution[0])
                snprintf(app->traffic.live_attribution, sizeof app->traffic.live_attribution, "%s",
                         app->cfg.traffic_attribution);
            LOGI("Traffic feed: %zu received, %zu accepted, %zu stale, %zu unmatched, %zu invalid",
                 rep.received, rep.accepted, rep.rejected_stale, rep.rejected_unmatched,
                 rep.rejected_invalid);
        } else {
            snprintf(app->provider_last_error, sizeof app->provider_last_error, "%s", rep.error);
        }
    } else {
        snprintf(app->provider_last_error, sizeof app->provider_last_error, "%s", err);
    }
    pthread_rwlock_unlock(&app->lock);
    if (!ok) LOGW("Traffic feed poll failed: %s", app->provider_last_error);
    free(body);
    return ok;
}

static void *provider_main(void *arg)
{
    app_t *app = arg;
    while (!atomic_load(&app->provider_stop)) {
        int retry_after = 0;
        app_poll_provider_once(app, &retry_after);
        int wait = retry_after > app->cfg.traffic_poll_s ? retry_after : app->cfg.traffic_poll_s;
        for (int i = 0; i < wait && !atomic_load(&app->provider_stop); i++) {
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

bool app_start_provider(app_t *app)
{
    if (!app->cfg.traffic_feed_url[0]) {
        LOGI("No IRA_TRAFFIC_FEED_URL configured: routes use baseline travel-time estimates.");
        return true;
    }
    atomic_store(&app->provider_stop, false);
    if (pthread_create(&app->provider_thread, NULL, provider_main, app) != 0) return false;
    app->provider_running = true;
    LOGI("Live traffic polling enabled every %d s", app->cfg.traffic_poll_s);
    return true;
}

void app_stop_provider(app_t *app)
{
    if (!app->provider_running) return;
    atomic_store(&app->provider_stop, true);
    pthread_join(app->provider_thread, NULL);
    app->provider_running = false;
}
