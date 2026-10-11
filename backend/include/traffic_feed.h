/*
 * traffic_feed.h — live traffic provider interface.
 *
 * The server polls IRA_TRAFFIC_FEED_URL with libcurl (timeouts, bounded
 * retries with back-off, HTTP 429 Retry-After honoured, response size cap,
 * optional "Authorization: Bearer $IRA_TRAFFIC_API_KEY").
 *
 * The feed must be in this project's NORMALISED JSON format:
 *   {
 *     "provider": "name", "attribution": "text shown to users",
 *     "generated_at": 1760000000,
 *     "observations": [
 *       { "latitude": 12.97, "longitude": 77.59,
 *         "speed_kmh": 18.5            (or "travel_time_seconds": 42 with edge_index),
 *         "confidence": 0.8,           (0..1, default 0.7)
 *         "observed_at": 1760000000,   (unix seconds)
 *         "expires_at": 1760000900 }   (optional, default observed_at + 900)
 *     ]
 *   }
 * Commercial APIs (TomTom, HERE, ...) each have their own formats, keys and
 * licence terms; connecting one means writing a small adapter that emits
 * this format — they are NOT built in, and nothing here claims coverage that
 * a configured provider does not actually deliver.
 */
#ifndef IRA_TRAFFIC_FEED_H
#define IRA_TRAFFIC_FEED_H

#include "traffic.h"
#include "spatial_index.h"

typedef struct {
    size_t received;
    size_t accepted;
    size_t rejected_invalid;
    size_t rejected_stale;
    size_t rejected_unmatched;
    char   provider[64];
    char   attribution[160];
    char   error[256];
} feed_report_t;

/* Parses a normalised feed and adds TSRC_LIVE observations (previous live
 * observations are replaced). match_m = max distance to snap to a road. */
bool traffic_feed_ingest(traffic_state_t *t, const spatial_index_t *si,
                         const char *json, size_t len, int64_t now,
                         double match_m, int64_t max_age_s, feed_report_t *rep);

/* ---- polling provider (libcurl) ---- */
typedef struct {
    char url[1024];
    char api_key[512];
    int  timeout_s;      /* per request, default 10 */
    int  max_retries;    /* default 3 */
    size_t max_bytes;    /* default 16 MB */
} feed_http_config_t;

/* Fetches the URL into a malloc'd NUL-terminated buffer. Returns HTTP status
 * (200 on success), or <0 on transport error; err receives a message. */
long feed_http_fetch(const feed_http_config_t *cfg, char **body, size_t *len,
                     int *retry_after_s, char *err, size_t err_len);

bool feed_http_global_init(void);
void feed_http_global_cleanup(void);

#endif
