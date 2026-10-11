#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_ADMIN_TOKEN_LEN 16

void config_defaults(ira_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->port = 8080;
    snprintf(c->bind_addr, sizeof c->bind_addr, "127.0.0.1");
    c->threads = 4;
    snprintf(c->sample_dir, sizeof c->sample_dir, "../data/sample");
    snprintf(c->frontend_dir, sizeof c->frontend_dir, "../frontend");
    c->route_workers = 2;
    c->max_settled = 0;
    c->max_snap_m = 5000.0;
    c->rate_route_per_min = 60;
    c->rate_general_per_min = 300;
    c->rate_incident_per_hour = 10;
    snprintf(c->traffic_provider, sizeof c->traffic_provider, "external-feed");
    c->traffic_poll_s = 300;
    c->accident_delay_factor = 2.0;
    c->roadwork_delay_factor = 1.5;
    /* Default map tile proxy and cache */
    snprintf(c->tile_cache_dir, sizeof c->tile_cache_dir, "data/tile_cache");
    snprintf(c->tile_url, sizeof c->tile_url, "/tiles/{z}/{x}/{y}.png");
    snprintf(c->tile_attribution, sizeof c->tile_attribution,
             "&copy; OpenStreetMap contributors");
    c->log_level = 1;
}

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static bool env_str(const char *name, char *dst, size_t n, char *err, size_t en)
{
    const char *v = getenv(name);
    if (!v) return true;
    if (strlen(v) >= n) { set_err(err, en, "%s is too long", name); return false; }
    snprintf(dst, n, "%s", v);
    return true;
}

static bool env_long(const char *name, long *dst, long lo, long hi, char *err, size_t en)
{
    const char *v = getenv(name);
    if (!v || !*v) return true;
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (errno || *end || x < lo || x > hi) {
        set_err(err, en, "%s must be an integer in [%ld, %ld]", name, lo, hi);
        return false;
    }
    *dst = x;
    return true;
}

static bool env_double(const char *name, double *dst, double lo, double hi, char *err, size_t en)
{
    const char *v = getenv(name);
    if (!v || !*v) return true;
    char *end;
    errno = 0;
    double x = strtod(v, &end);
    if (errno || *end || !(x >= lo && x <= hi)) {
        set_err(err, en, "%s must be a number in [%g, %g]", name, lo, hi);
        return false;
    }
    *dst = x;
    return true;
}

bool config_load_env(ira_config_t *c, char *err, size_t en)
{
    long v;
    v = c->port;            if (!env_long("IRA_PORT", &v, 1, 65535, err, en)) return false; c->port = (int)v;
    v = c->threads;         if (!env_long("IRA_THREADS", &v, 1, 64, err, en)) return false; c->threads = (int)v;
    v = c->route_workers;   if (!env_long("IRA_ROUTE_WORKERS", &v, 1, 32, err, en)) return false; c->route_workers = (int)v;
    v = (long)c->max_settled; if (!env_long("IRA_MAX_SETTLED", &v, 0, 2000000000L, err, en)) return false; c->max_settled = (size_t)v;
    v = c->rate_route_per_min;     if (!env_long("IRA_RATE_ROUTE_PER_MIN", &v, 1, 100000, err, en)) return false; c->rate_route_per_min = (int)v;
    v = c->rate_general_per_min;   if (!env_long("IRA_RATE_GENERAL_PER_MIN", &v, 1, 1000000, err, en)) return false; c->rate_general_per_min = (int)v;
    v = c->rate_incident_per_hour; if (!env_long("IRA_RATE_INCIDENT_PER_HOUR", &v, 1, 10000, err, en)) return false; c->rate_incident_per_hour = (int)v;
    v = c->traffic_poll_s;  if (!env_long("IRA_TRAFFIC_POLL_SECONDS", &v, 30, 86400, err, en)) return false; c->traffic_poll_s = (int)v;
    v = c->log_level;       if (!env_long("IRA_LOG_LEVEL", &v, 0, 3, err, en)) return false; c->log_level = (int)v;
    v = c->trust_proxy;     if (!env_long("IRA_TRUST_PROXY", &v, 0, 1, err, en)) return false; c->trust_proxy = v != 0;

    if (!env_double("IRA_MAX_SNAP_M", &c->max_snap_m, 10, 50000, err, en)) return false;
    if (!env_double("IRA_ACCIDENT_DELAY_FACTOR", &c->accident_delay_factor, 1, 20, err, en)) return false;
    if (!env_double("IRA_ROADWORK_DELAY_FACTOR", &c->roadwork_delay_factor, 1, 20, err, en)) return false;

    if (!env_str("IRA_BIND", c->bind_addr, sizeof c->bind_addr, err, en)) return false;
    if (!env_str("IRA_CORS_ORIGIN", c->cors_origin, sizeof c->cors_origin, err, en)) return false;
    if (!env_str("IRA_GRAPH_PATH", c->graph_path, sizeof c->graph_path, err, en)) return false;
    if (!env_str("IRA_DB_PATH", c->db_path, sizeof c->db_path, err, en)) return false;
    if (!env_str("IRA_SAMPLE_DIR", c->sample_dir, sizeof c->sample_dir, err, en)) return false;
    if (!env_str("IRA_FRONTEND_DIR", c->frontend_dir, sizeof c->frontend_dir, err, en)) return false;
    if (!env_str("IRA_ADMIN_TOKEN", c->admin_token, sizeof c->admin_token, err, en)) return false;
    if (!env_str("IRA_TRAFFIC_FEED_URL", c->traffic_feed_url, sizeof c->traffic_feed_url, err, en)) return false;
    if (!env_str("IRA_TRAFFIC_API_KEY", c->traffic_api_key, sizeof c->traffic_api_key, err, en)) return false;
    if (!env_str("IRA_TRAFFIC_PROVIDER_NAME", c->traffic_provider, sizeof c->traffic_provider, err, en)) return false;
    if (!env_str("IRA_TRAFFIC_ATTRIBUTION", c->traffic_attribution, sizeof c->traffic_attribution, err, en)) return false;
    if (!env_str("IRA_TILE_CACHE_DIR", c->tile_cache_dir, sizeof c->tile_cache_dir, err, en)) return false;
    if (!env_str("IRA_TILE_URL", c->tile_url, sizeof c->tile_url, err, en)) return false;
    if (!env_str("IRA_TILE_ATTRIBUTION", c->tile_attribution, sizeof c->tile_attribution, err, en)) return false;
    if (c->tile_cache_dir[0] && !getenv("IRA_TILE_URL")) {
        snprintf(c->tile_url, sizeof c->tile_url, "/tiles/{z}/{x}/{y}.png");
    }
    char *tp = strstr(c->tile_url, "/tiles/{z}/{x}/{y}.png");
    if (tp) {
        memmove(c->tile_url, tp, strlen(tp) + 1);
    }

    if (strchr(c->cors_origin, '\r') || strchr(c->cors_origin, '\n')) {
        set_err(err, en, "IRA_CORS_ORIGIN contains invalid characters");
        return false;
    }
    return true;
}

bool config_admin_enabled(const ira_config_t *c)
{
    return strlen(c->admin_token) >= MIN_ADMIN_TOKEN_LEN;
}
