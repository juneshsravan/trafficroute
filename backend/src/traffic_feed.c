#define _POSIX_C_SOURCE 200809L
/*
 * traffic_feed.c — normalised feed ingestion and libcurl fetching.
 */
#include "traffic_feed.h"
#include "geospatial.h"
#include "cJSON.h"

#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double num_or(const cJSON *o, const char *k, double def, bool *present)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (present) *present = cJSON_IsNumber(v);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool traffic_feed_ingest(traffic_state_t *t, const spatial_index_t *si,
                         const char *json, size_t len, int64_t now,
                         double match_m, int64_t max_age_s, feed_report_t *rep)
{
    memset(rep, 0, sizeof *rep);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsObject(root)) {
        snprintf(rep->error, sizeof rep->error, "feed is not a JSON object");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *obs = cJSON_GetObjectItemCaseSensitive(root, "observations");
    if (!cJSON_IsArray(obs)) {
        snprintf(rep->error, sizeof rep->error, "feed has no 'observations' array");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *prov = cJSON_GetObjectItemCaseSensitive(root, "provider");
    const cJSON *attr = cJSON_GetObjectItemCaseSensitive(root, "attribution");
    snprintf(rep->provider, sizeof rep->provider, "%s",
             cJSON_IsString(prov) ? prov->valuestring : "external-feed");
    snprintf(rep->attribution, sizeof rep->attribution, "%s",
             cJSON_IsString(attr) ? attr->valuestring : "");

    /* A fresh feed replaces the previous live snapshot. */
    size_t w = 0;
    for (size_t i = 0; i < t->obs_count; i++)
        if (t->obs[i].source != TSRC_LIVE) t->obs[w++] = t->obs[i];
    t->obs_count = w;

    const cJSON *o;
    cJSON_ArrayForEach(o, obs) {
        rep->received++;
        if (!cJSON_IsObject(o)) { rep->rejected_invalid++; continue; }
        bool has_lat, has_lon, has_speed, has_tt, has_idx;
        double lat = num_or(o, "latitude", NAN, &has_lat);
        double lon = num_or(o, "longitude", NAN, &has_lon);
        double speed = num_or(o, "speed_kmh", 0, &has_speed);
        double tt = num_or(o, "travel_time_seconds", 0, &has_tt);
        double idx = num_or(o, "edge_index", -1, &has_idx);
        double conf = num_or(o, "confidence", 0.7, NULL);
        double observed = num_or(o, "observed_at", NAN, NULL);
        double expires = num_or(o, "expires_at", observed + 900, NULL);

        if (!isfinite(observed) || observed > (double)now + 60 ||
            (double)now - observed > (double)max_age_s) { rep->rejected_stale++; continue; }

        edge_idx_t e = IRA_INVALID_IDX;
        if (has_idx) {
            if (idx >= 0 && idx < (double)t->g->edge_count && idx == floor(idx)) e = (edge_idx_t)idx;
        } else if (has_lat && has_lon && geo_valid_coord(lat, lon)) {
            double d;
            if (!spatial_nearest_edge(si, t->g, lat, lon, match_m, &e, &d)) {
                rep->rejected_unmatched++;
                continue;
            }
        }
        if (e == IRA_INVALID_IDX) { rep->rejected_invalid++; continue; }

        double time_s;
        if (has_tt) time_s = tt;
        else if (has_speed && speed > 0.5 && speed < 200) time_s = (double)t->g->edges[e].distance_m / (speed / 3.6);
        else { rep->rejected_invalid++; continue; }

        int rc = traffic_add_observation_noapply(t, e, time_s, conf, TSRC_LIVE,
                                                 (int64_t)observed, (int64_t)expires,
                                                 rep->provider, now, NULL);
        if (rc == 0) rep->accepted++;
        else if (rc == -2) rep->rejected_stale++;
        else rep->rejected_invalid++;
    }
    snprintf(t->live_provider, sizeof t->live_provider, "%s", rep->provider);
    snprintf(t->live_attribution, sizeof t->live_attribution, "%s", rep->attribution);
    traffic_reapply_all(t);
    cJSON_Delete(root);
    return true;
}

/* ---------------- libcurl ---------------- */

typedef struct {
    char  *buf;
    size_t len, cap, max;
    bool   overflow;
} membuf_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    membuf_t *m = ud;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->max) { m->overflow = true; return 0; }   /* abort transfer */
    if (m->len + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap : 65536;
        while (nc < m->len + n + 1) nc *= 2;
        if (nc > m->max) nc = m->max;
        char *p = realloc(m->buf, nc);
        if (!p) return 0;
        m->buf = p;
        m->cap = nc;
    }
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;
    m->buf[m->len] = '\0';
    return n;
}

bool feed_http_global_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }
void feed_http_global_cleanup(void) { curl_global_cleanup(); }

static void sleep_s(int s)
{
    struct timespec ts = { .tv_sec = s, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
}

long feed_http_fetch(const feed_http_config_t *cfg, char **body, size_t *len,
                     int *retry_after_s, char *err, size_t err_len)
{
    *body = NULL;
    *len = 0;
    if (retry_after_s) *retry_after_s = 0;
    int retries = cfg->max_retries > 0 ? cfg->max_retries : 3;
    if (retries > 5) retries = 5;
    long status = -1;

    for (int attempt = 0; attempt <= retries; attempt++) {
        CURL *c = curl_easy_init();
        if (!c) { snprintf(err, err_len, "curl init failed"); return -1; }
        membuf_t m = { .max = cfg->max_bytes ? cfg->max_bytes : (16u << 20) };
        struct curl_slist *hdrs = NULL;
        char auth[600];
        if (cfg->api_key[0]) {
            snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg->api_key);
            hdrs = curl_slist_append(hdrs, auth);
        }
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
        curl_easy_setopt(c, CURLOPT_URL, cfg->url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)(cfg->timeout_s > 0 ? cfg->timeout_s : 10));
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "IndiaRouteAI/1.0");
        /* Only allow http(s) and file (file:// is used by tests / local drops). */
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https,file");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");  /* never redirect to file:// */
#endif

        CURLcode rc = curl_easy_perform(c);
        status = -1;
        if (rc == CURLE_OK) {
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
            if (status == 0) status = 200;   /* file:// has no HTTP status */
        } else {
            snprintf(err, err_len, "%s%s", curl_easy_strerror(rc),
                     m.overflow ? " (response too large)" : "");
        }
        curl_off_t ra = 0;
        if (status == 429) curl_easy_getinfo(c, CURLINFO_RETRY_AFTER, &ra);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);

        if (status == 200) { *body = m.buf; *len = m.len; return status; }
        free(m.buf);
        if (status == 429) {
            if (retry_after_s) *retry_after_s = ra > 0 ? (int)ra : 60;
            snprintf(err, err_len, "rate limited by provider (HTTP 429)");
            return status;          /* do not hammer: caller waits Retry-After */
        }
        if (status >= 400 && status < 500) {
            snprintf(err, err_len, "provider returned HTTP %ld", status);
            return status;          /* client errors are not retried */
        }
        if (status >= 500) snprintf(err, err_len, "provider returned HTTP %ld", status);
        if (m.overflow) return -1;
        if (attempt < retries) sleep_s(1 << attempt);   /* 1, 2, 4 s back-off */
    }
    return status;
}
