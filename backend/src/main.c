/*
 * main.c — IndiaRoute AI HTTP server entry point.
 *
 *   ira_server            start with configuration from environment variables
 *   ira_server --check    load data, print a summary, exit (no HTTP)
 *   ira_server --help
 */
#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "config.h"
#include "http_server.h"
#include "log.h"
#include "traffic_feed.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(void)
{
    puts("IndiaRoute AI server " IRA_VERSION "\n"
         "\n"
         "Usage: ira_server [--check] [--help]\n"
         "\n"
         "Configuration (environment variables):\n"
         "  IRA_GRAPH_PATH        graph.bin produced by ira_import (else sample CSV network)\n"
         "  IRA_DB_PATH           SQLite DB with places index / incidents (from ira_import --db)\n"
         "  IRA_SAMPLE_DIR        sample network directory (default ../data/sample)\n"
         "  IRA_FRONTEND_DIR      static frontend directory (default ../frontend)\n"
         "  IRA_PORT / IRA_BIND   listen port (8080) / IPv4 address (127.0.0.1)\n"
         "  IRA_THREADS           HTTP worker threads (4)\n"
         "  IRA_ROUTE_WORKERS     concurrent route searches (2)\n"
         "  IRA_MAX_SETTLED       per-search node limit, 0 = unlimited\n"
         "  IRA_MAX_SNAP_M        max distance to snap points to a road (5000)\n"
         "  IRA_ADMIN_TOKEN       bearer token for /api/v1/admin/* (>= 16 chars)\n"
         "  IRA_CORS_ORIGIN       allowed cross-origin (empty = same-origin only)\n"
         "  IRA_TRAFFIC_FEED_URL  normalised live-traffic JSON feed (see docs/api.md)\n"
         "  IRA_TRAFFIC_API_KEY   bearer key sent to the feed\n"
         "  IRA_TILE_URL          map tile URL template passed to the frontend\n"
         "  IRA_LOG_LEVEL         0 debug, 1 info, 2 warn, 3 error\n"
         "See .env.example for the full list.");
}

int main(int argc, char **argv)
{
    bool check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        if (strcmp(argv[i], "--check") == 0) check = true;
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
    }

    ira_config_t cfg;
    char err[512];
    config_defaults(&cfg);
    if (!config_load_env(&cfg, err, sizeof err)) {
        fprintf(stderr, "configuration error: %s\n", err);
        return 2;
    }
    log_set_level(cfg.log_level);
    if (!feed_http_global_init()) { LOGE("libcurl initialisation failed"); return 1; }

    app_t app;
    if (!app_init(&app, &cfg, err, sizeof err)) {
        LOGE("start-up failed: %s", err);
        app_free(&app);
        feed_http_global_cleanup();
        return 1;
    }
    if (check) {
        printf("OK: %zu nodes, %zu edges, dataset '%s'\n", app.g->node_count, app.g->edge_count,
               app.g->dataset);
        app_free(&app);
        feed_http_global_cleanup();
        return 0;
    }

#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    http_server_t *srv = http_server_start(&app, err, sizeof err);
    if (!srv) {
        LOGE("%s", err);
        app_free(&app);
        feed_http_global_cleanup();
        return 1;
    }
    app_start_provider(&app);
    LOGI("IndiaRoute AI listening on http://%s:%d/", cfg.bind_addr, cfg.port);

#ifndef _WIN32
    while (!g_stop) pause();
#else
    while (!g_stop) Sleep(100);
#endif

    LOGI("Shutting down");
    http_server_stop(srv);
    app_free(&app);
    feed_http_global_cleanup();
    return 0;
}
