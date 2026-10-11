/*
 * config.h — server configuration from environment variables.
 * Secrets (admin token, traffic API key) come ONLY from the environment and
 * are never logged or returned by the API.
 */
#ifndef IRA_CONFIG_H
#define IRA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    /* network */
    int    port;                    /* IRA_PORT (8080) */
    char   bind_addr[64];           /* IRA_BIND (127.0.0.1) */
    int    threads;                 /* IRA_THREADS (4) */
    char   cors_origin[256];        /* IRA_CORS_ORIGIN ("" = same-origin only) */
    bool   trust_proxy;             /* IRA_TRUST_PROXY (0): honour X-Forwarded-For */

    /* data */
    char   graph_path[1024];        /* IRA_GRAPH_PATH  (graph.bin from importer) */
    char   db_path[1024];           /* IRA_DB_PATH     (places/incidents SQLite) */
    char   sample_dir[1024];        /* IRA_SAMPLE_DIR  (CSV test network; used if no graph) */
    char   frontend_dir[1024];      /* IRA_FRONTEND_DIR (../frontend) */

    /* routing */
    int    route_workers;           /* IRA_ROUTE_WORKERS (2): concurrent searches */
    size_t max_settled;             /* IRA_MAX_SETTLED (0 = unlimited) */
    double max_snap_m;              /* IRA_MAX_SNAP_M (5000) */

    /* security */
    char   admin_token[256];        /* IRA_ADMIN_TOKEN (admin API disabled if empty/short) */
    int    rate_route_per_min;      /* IRA_RATE_ROUTE_PER_MIN (60) */
    int    rate_general_per_min;    /* IRA_RATE_GENERAL_PER_MIN (300) */
    int    rate_incident_per_hour;  /* IRA_RATE_INCIDENT_PER_HOUR (10) */

    /* traffic */
    char   traffic_feed_url[1024];  /* IRA_TRAFFIC_FEED_URL ("" = no live provider) */
    char   traffic_api_key[512];    /* IRA_TRAFFIC_API_KEY */
    char   traffic_provider[64];    /* IRA_TRAFFIC_PROVIDER_NAME */
    char   traffic_attribution[160];/* IRA_TRAFFIC_ATTRIBUTION */
    int    traffic_poll_s;          /* IRA_TRAFFIC_POLL_SECONDS (300, min 30) */
    double accident_delay_factor;   /* IRA_ACCIDENT_DELAY_FACTOR (2.0) */
    double roadwork_delay_factor;   /* IRA_ROADWORK_DELAY_FACTOR (1.5) */

    /* map tiles (passed to the frontend) */
    char   tile_url[512];           /* IRA_TILE_URL */
    char   tile_attribution[256];   /* IRA_TILE_ATTRIBUTION */
    char   tile_cache_dir[512];     /* IRA_TILE_CACHE_DIR */

    int    log_level;               /* IRA_LOG_LEVEL 0=debug 1=info 2=warn 3=error */
} ira_config_t;

void config_defaults(ira_config_t *c);
/* Reads environment variables over the defaults. Returns false (with err)
 * on invalid values. */
bool config_load_env(ira_config_t *c, char *err, size_t err_len);
bool config_admin_enabled(const ira_config_t *c);

#endif
