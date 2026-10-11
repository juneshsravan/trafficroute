/*
 * api.c — REST endpoint implementations. See docs/api.md for the contract.
 *
 * Conventions:
 *   success: {"status":"success", ...}
 *   error:   {"status":"error","error":{"code":"NO_ROUTE","message":"...","field":"..."}}
 *   Request bodies are strictly validated: unknown fields, wrong types and
 *   out-of-range values are rejected with 400 rather than silently ignored.
 */
#define _POSIX_C_SOURCE 200809L
#include "api.h"
#include "cJSON.h"
#include "geospatial.h"
#include "location_search.h"
#include "log.h"
#include "route_alternatives.h"
#include "route_geometry.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Response helpers
 * ====================================================================== */

static void respond(api_response_t *r, int status, cJSON *root)
{
    r->status = status;
    r->body = cJSON_PrintUnformatted(root);
    if (!r->body) {
        r->status = 500;
        r->body = strdup("{\"status\":\"error\",\"error\":{\"code\":\"INTERNAL\",\"message\":\"serialisation failed\"}}");
    }
    cJSON_Delete(root);
}

static void error_resp(api_response_t *r, int status, const char *code,
                       const char *field, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON *e = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    if (field) cJSON_AddStringToObject(e, "field", field);
    respond(r, status, root);
}

static cJSON *success_obj(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    return root;
}

static double round6(double x) { return round(x * 1e6) / 1e6; }
static double round1(double x) { return round(x * 10.0) / 10.0; }

static cJSON *latlon_array(double lat, double lon)
{
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(round6(lat)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(round6(lon)));
    return a;
}

void api_response_free(api_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
}

bool api_token_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    /* Always walk the full expected token so timing does not leak length. */
    for (size_t i = 0; i < lb; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        diff |= (unsigned char)(ca ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

/* ======================================================================
 * Request parsing helpers
 * ====================================================================== */

static const char *arg(const api_request_t *req, const char *k)
{
    return req->get_arg ? req->get_arg(req->arg_ctx, k) : NULL;
}

static bool parse_double_str(const char *s, double *out)
{
    if (!s || !*s) return false;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_int_arg(const api_request_t *req, const char *k, long lo, long hi,
                          long def, long *out)
{
    const char *s = arg(req, k);
    if (!s || !*s) { *out = def; return true; }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Parses the JSON body into an object or writes an error response. */
static cJSON *parse_body(const api_request_t *req, api_response_t *resp)
{
    if (!req->body || req->body_len == 0) {
        error_resp(resp, 400, "EMPTY_BODY", NULL, "A JSON request body is required");
        return NULL;
    }
    if (req->body_len > API_MAX_BODY) {
        error_resp(resp, 413, "BODY_TOO_LARGE", NULL, "Request body exceeds %d bytes", API_MAX_BODY);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        error_resp(resp, 400, "INVALID_JSON", NULL, "Request body is not valid JSON");
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        error_resp(resp, 400, "INVALID_JSON", NULL, "Request body must be a JSON object");
        return NULL;
    }
    return root;
}

/* Rejects keys not in `allowed` (NULL-terminated). */
static bool only_keys(const cJSON *obj, const char *const *allowed, const char *where,
                      api_response_t *resp)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        bool ok = false;
        for (size_t i = 0; allowed[i]; i++)
            if (it->string && strcmp(it->string, allowed[i]) == 0) { ok = true; break; }
        if (!ok) {
            char field[160];
            snprintf(field, sizeof field, "%s%s%.100s", where ? where : "", where ? "." : "",
                     it->string ? it->string : "?");
            error_resp(resp, 400, "UNKNOWN_FIELD", field, "Unknown field '%s'", field);
            return false;
        }
    }
    return true;
}

static bool get_number(const cJSON *obj, const char *k, double lo, double hi, bool required,
                       double def, double *out, api_response_t *resp)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, k);
    if (!v) {
        if (required) { error_resp(resp, 400, "MISSING_FIELD", k, "Field '%s' is required", k); return false; }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo || v->valuedouble > hi) {
        error_resp(resp, 400, "INVALID_FIELD", k, "Field '%s' must be a number between %g and %g", k, lo, hi);
        return false;
    }
    *out = v->valuedouble;
    return true;
}

static bool get_int(const cJSON *obj, const char *k, long lo, long hi, long def, long *out,
                    api_response_t *resp)
{
    double d;
    if (!get_number(obj, k, (double)lo, (double)hi, false, (double)def, &d, resp)) return false;
    if (d != floor(d)) { error_resp(resp, 400, "INVALID_FIELD", k, "Field '%s' must be an integer", k); return false; }
    *out = (long)d;
    return true;
}

static bool get_bool(const cJSON *obj, const char *k, bool def, bool *out, api_response_t *resp)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, k);
    if (!v) { *out = def; return true; }
    if (!cJSON_IsBool(v)) { error_resp(resp, 400, "INVALID_FIELD", k, "Field '%s' must be true or false", k); return false; }
    *out = cJSON_IsTrue(v);
    return true;
}

static bool get_string(const cJSON *obj, const char *k, const char *def, const char **out,
                       api_response_t *resp)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, k);
    if (!v) { *out = def; return true; }
    if (!cJSON_IsString(v)) { error_resp(resp, 400, "INVALID_FIELD", k, "Field '%s' must be a string", k); return false; }
    *out = v->valuestring;
    return true;
}

/* {"latitude": .., "longitude": ..} with range validation. */
static bool get_point(const cJSON *obj, const char *k, double *lat, double *lon,
                      api_response_t *resp)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(obj, k);
    if (!p) { error_resp(resp, 400, "MISSING_FIELD", k, "Field '%s' is required", k); return false; }
    if (!cJSON_IsObject(p)) {
        error_resp(resp, 400, "INVALID_FIELD", k, "'%s' must be an object with latitude and longitude", k);
        return false;
    }
    static const char *const keys[] = {"latitude", "longitude", NULL};
    if (!only_keys(p, keys, k, resp)) return false;
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(p, "latitude");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(p, "longitude");
    if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo) ||
        !geo_valid_coord(la->valuedouble, lo->valuedouble)) {
        error_resp(resp, 400, "INVALID_COORDINATES", k,
                   "'%s' needs numeric latitude in [-90, 90] and longitude in [-180, 180]", k);
        return false;
    }
    *lat = la->valuedouble;
    *lon = lo->valuedouble;
    return true;
}

/* Rejects points far outside the loaded dataset's bounding box. */
static bool in_coverage(const app_t *app, double lat, double lon)
{
    double m = app->cfg.max_snap_m / 111195.0 * 2.0;
    const graph_t *g = app->g;
    return lat >= g->min_lat - m && lat <= g->max_lat + m &&
           lon >= g->min_lon - m * 2 && lon <= g->max_lon + m * 2;
}

static void sanitize_text(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

/* ======================================================================
 * Shared JSON builders
 * ====================================================================== */

static cJSON *traffic_data_json(app_t *app, int64_t now)
{
    traffic_summary_t s;
    traffic_summary(&app->traffic, now, &s);
    cJSON *t = cJSON_CreateObject();
    bool any = s.live_available || s.simulated_active || s.admin_active || s.incident_model_active;
    cJSON_AddBoolToObject(t, "available", any);
    cJSON_AddBoolToObject(t, "live", s.live_available);
    cJSON_AddBoolToObject(t, "simulated", s.simulated_active);
    cJSON_AddBoolToObject(t, "admin_overrides", s.admin_active);
    cJSON_AddBoolToObject(t, "incident_delay_model", s.incident_model_active);
    cJSON_AddNumberToObject(t, "closed_segments", (double)s.closed_edges);

    char msg[512];
    size_t k = 0;
    msg[0] = '\0';
    if (s.live_available)
        k += (size_t)snprintf(msg + k, sizeof msg - k, "Live traffic from %s. ",
                              app->traffic.live_provider[0] ? app->traffic.live_provider : "configured provider");
    if (s.simulated_active)
        k += (size_t)snprintf(msg + k, sizeof msg - k,
                              "SIMULATED traffic for demonstration only - not real road conditions. ");
    if (s.admin_active && k < sizeof msg)
        k += (size_t)snprintf(msg + k, sizeof msg - k, "Includes operator-entered traffic updates. ");
    if (s.incident_model_active && k < sizeof msg)
        k += (size_t)snprintf(msg + k, sizeof msg - k,
                              "Verified incidents applied with an estimated delay model. ");
    if (!any && k < sizeof msg)
        snprintf(msg + k, sizeof msg - k,
                 "No live traffic data - using baseline travel-time estimates from road type and speed limits.");
    size_t ml = strlen(msg);
    while (ml && msg[ml - 1] == ' ') msg[--ml] = '\0';
    cJSON_AddStringToObject(t, "message", msg);
    if (s.live_available && app->traffic.live_attribution[0])
        cJSON_AddStringToObject(t, "attribution", app->traffic.live_attribution);
    return t;
}

static cJSON *route_json(app_t *app, const route_result_t *r, const route_geometry_t *geo,
                         bool include_polyline)
{
    const graph_t *g = app->g;
    cJSON *o = cJSON_CreateObject();
    double baseline = 0;
    size_t affected = 0;
    for (size_t i = 0; i < r->edge_count; i++) {
        const graph_edge_t *e = &g->edges[r->edges[i]];
        baseline += (double)e->base_time_s;
        if (e->traffic_time_s > 0) affected++;
    }
    cJSON_AddNumberToObject(o, "distance_meters", round1(r->distance_m));
    cJSON_AddNumberToObject(o, "estimated_duration_seconds", round1(r->duration_s));
    cJSON_AddNumberToObject(o, "baseline_duration_seconds", round1(baseline));
    cJSON_AddNumberToObject(o, "traffic_affected_segments", (double)affected);
    cJSON_AddNumberToObject(o, "segment_count", (double)r->edge_count);
    cJSON_AddNumberToObject(o, "nodes_settled", (double)r->nodes_settled);

    cJSON *geom = cJSON_AddArrayToObject(o, "geometry");
    for (size_t i = 0; i < geo->point_count; i++)
        cJSON_AddItemToArray(geom, latlon_array(geo->points[i].lat, geo->points[i].lon));
    if (include_polyline) {
        char *pl = polyline_encode(geo->points, geo->point_count);
        if (pl) { cJSON_AddStringToObject(o, "polyline", pl); free(pl); }
    }
    cJSON *ins = cJSON_AddArrayToObject(o, "instructions");
    for (size_t i = 0; i < geo->instruction_count; i++) {
        const route_instruction_t *in = &geo->instructions[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "type", instruction_type_str(in->type));
        cJSON_AddStringToObject(j, "text", in->text);
        if (in->road[0]) cJSON_AddStringToObject(j, "road", in->road);
        cJSON_AddNumberToObject(j, "distance_meters", round1(in->distance_m));
        cJSON_AddNumberToObject(j, "duration_seconds", round1(in->duration_s));
        cJSON_AddItemToObject(j, "location", latlon_array(in->lat, in->lon));
        cJSON_AddNumberToObject(j, "geometry_index", (double)in->geometry_index);
        cJSON_AddItemToArray(ins, j);
    }
    return o;
}

static cJSON *incident_json(app_t *app, const incident_t *in)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", (double)in->id);
    cJSON_AddStringToObject(j, "type", incident_type_str(in->type));
    cJSON_AddStringToObject(j, "status", incident_status_str(in->status));
    cJSON_AddNumberToObject(j, "latitude", round6(in->lat));
    cJSON_AddNumberToObject(j, "longitude", round6(in->lon));
    cJSON_AddNumberToObject(j, "edge_index", (double)in->edge);
    cJSON_AddBoolToObject(j, "both_directions", in->reverse_edge != IRA_INVALID_IDX);
    if (in->edge < app->g->edge_count) {
        const char *name = graph_edge_name(app->g, &app->g->edges[in->edge]);
        if (name[0]) cJSON_AddStringToObject(j, "road", name);
    }
    cJSON_AddNumberToObject(j, "reported_at", (double)in->reported_at);
    cJSON_AddNumberToObject(j, "expires_at", (double)in->expires_at);
    cJSON_AddBoolToObject(j, "simulated", in->simulated);
    if (in->description[0]) cJSON_AddStringToObject(j, "description", in->description);
    char effect[96];
    if (in->status != INC_VERIFIED) snprintf(effect, sizeof effect, "none (not verified)");
    else if (in->type == INC_CLOSURE) snprintf(effect, sizeof effect, "road closed to routing");
    else if (in->type == INC_ACCIDENT)
        snprintf(effect, sizeof effect, "estimated delay x%.1f (model)", app->traffic.p.accident_delay_factor);
    else if (in->type == INC_ROADWORK)
        snprintf(effect, sizeof effect, "estimated delay x%.1f (model)", app->traffic.p.roadwork_delay_factor);
    else snprintf(effect, sizeof effect, "informational only");
    cJSON_AddStringToObject(j, "routing_effect", effect);
    return j;
}

/* ======================================================================
 * Endpoints
 * ====================================================================== */

static void h_health(app_t *app, api_response_t *resp)
{
    int64_t now = app_now();
    cJSON *root = cJSON_CreateObject();
    pthread_rwlock_rdlock(&app->lock);
    bool provider_ok = !app->cfg.traffic_feed_url[0] ||
                       (app->provider_last_success && now - app->provider_last_success < 3 * app->cfg.traffic_poll_s);
    cJSON_AddStringToObject(root, "status", provider_ok ? "ok" : "degraded");
    cJSON_AddStringToObject(root, "version", IRA_VERSION);
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(now - app->started_at));
    cJSON *gr = cJSON_AddObjectToObject(root, "graph");
    cJSON_AddBoolToObject(gr, "loaded", app->g != NULL);
    cJSON_AddStringToObject(gr, "dataset", app->g->dataset);
    cJSON_AddBoolToObject(gr, "synthetic_sample", app->sample_mode);
    cJSON_AddNumberToObject(gr, "nodes", (double)app->g->node_count);
    cJSON_AddNumberToObject(gr, "edges", (double)app->g->edge_count);
    cJSON *db = cJSON_AddObjectToObject(root, "database");
    cJSON_AddBoolToObject(db, "connected", app->db != NULL);
    cJSON_AddNumberToObject(db, "places", (double)db_place_count(app->db));
    cJSON *tp = cJSON_AddObjectToObject(root, "traffic_provider");
    cJSON_AddBoolToObject(tp, "configured", app->cfg.traffic_feed_url[0] != '\0');
    if (app->cfg.traffic_feed_url[0]) {
        cJSON_AddNumberToObject(tp, "last_attempt", (double)app->provider_last_attempt);
        cJSON_AddNumberToObject(tp, "last_success", (double)app->provider_last_success);
        if (app->provider_last_error[0]) cJSON_AddStringToObject(tp, "last_error", app->provider_last_error);
    }
    cJSON_AddItemToObject(root, "traffic_data", traffic_data_json(app, now));
    cJSON_AddBoolToObject(root, "admin_api_enabled", config_admin_enabled(&app->cfg));
    pthread_rwlock_unlock(&app->lock);
    cJSON *rq = cJSON_AddObjectToObject(root, "requests");
    long ok = atomic_load(&app->routes_ok);
    cJSON_AddNumberToObject(rq, "total", (double)atomic_load(&app->requests));
    cJSON_AddNumberToObject(rq, "routes_ok", (double)ok);
    cJSON_AddNumberToObject(rq, "routes_failed", (double)atomic_load(&app->routes_failed));
    cJSON_AddNumberToObject(rq, "mean_route_ms",
                            ok ? round1((double)atomic_load(&app->route_us_total) / (double)ok / 1000.0) : 0);
    respond(resp, 200, root);
}

static void h_config(app_t *app, api_response_t *resp)
{
    cJSON *root = success_obj();
    cJSON_AddStringToObject(root, "tile_url", app->cfg.tile_url);
    cJSON_AddStringToObject(root, "tile_attribution", app->cfg.tile_attribution);
    cJSON_AddStringToObject(root, "data_attribution",
                            app->osm_data ? "Road data &copy; OpenStreetMap contributors (ODbL)"
                                          : "Synthetic sample network - not real roads");
    cJSON_AddBoolToObject(root, "synthetic_sample", app->sample_mode);
    cJSON_AddStringToObject(root, "dataset", app->g->dataset);
    cJSON *bb = cJSON_AddArrayToObject(root, "bbox");   /* [minLat, minLon, maxLat, maxLon] */
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(round6(app->g->min_lat)));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(round6(app->g->min_lon)));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(round6(app->g->max_lat)));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(round6(app->g->max_lon)));
    cJSON_AddNumberToObject(root, "max_snap_meters", app->cfg.max_snap_m);
    cJSON_AddNumberToObject(root, "max_alternatives", MAX_ALTERNATIVES);
    respond(resp, 200, root);
}

static void h_network_stats(app_t *app, api_response_t *resp)
{
    cJSON *root = success_obj();
    const graph_t *g = app->g;
    cJSON_AddStringToObject(root, "dataset", g->dataset);
    cJSON_AddNumberToObject(root, "dataset_timestamp", (double)g->dataset_time);
    cJSON_AddBoolToObject(root, "synthetic_sample", app->sample_mode);
    cJSON_AddNumberToObject(root, "nodes", (double)g->node_count);
    cJSON_AddNumberToObject(root, "directed_edges", (double)g->edge_count);
    cJSON_AddNumberToObject(root, "shape_points", (double)g->shape_count);
    cJSON_AddNumberToObject(root, "main_component_node_share", round(app->main_component_share * 10000) / 10000);
    cJSON_AddNumberToObject(root, "max_speed_kmh", round1(g->max_speed_mps * 3.6));
    cJSON_AddNumberToObject(root, "graph_memory_mb", round1((double)graph_memory_bytes(g) / 1048576.0));
    cJSON_AddNumberToObject(root, "spatial_index_memory_mb",
                            round1((double)spatial_index_memory_bytes(app->si) / 1048576.0));
    cJSON_AddNumberToObject(root, "search_workspace_memory_mb",
                            round1((double)app->ws_count * (double)g->node_count * 37.0 / 1048576.0));
    cJSON *bb = cJSON_AddObjectToObject(root, "bbox");
    cJSON_AddNumberToObject(bb, "min_latitude", round6(g->min_lat));
    cJSON_AddNumberToObject(bb, "min_longitude", round6(g->min_lon));
    cJSON_AddNumberToObject(bb, "max_latitude", round6(g->max_lat));
    cJSON_AddNumberToObject(bb, "max_longitude", round6(g->max_lon));
    size_t by_class[ROAD_CLASS_COUNT] = {0}, tolls = 0, restricted = 0;
    double km[ROAD_CLASS_COUNT] = {0};
    for (size_t i = 0; i < g->edge_count; i++) {
        const graph_edge_t *e = &g->edges[i];
        by_class[e->road_class]++;
        km[e->road_class] += (double)e->distance_m / 1000.0;
        if (e->flags & EDGE_FLAG_TOLL) tolls++;
        if (e->flags & EDGE_FLAG_RESTRICTED) restricted++;
    }
    cJSON *cls = cJSON_AddObjectToObject(root, "edges_by_road_class");
    cJSON *ckm = cJSON_AddObjectToObject(root, "directed_km_by_road_class");
    for (int c = 0; c < ROAD_CLASS_COUNT; c++) {
        cJSON_AddNumberToObject(cls, road_class_name((road_class_t)c), (double)by_class[c]);
        cJSON_AddNumberToObject(ckm, road_class_name((road_class_t)c), round1(km[c]));
    }
    cJSON_AddNumberToObject(root, "toll_edges", (double)tolls);
    cJSON_AddNumberToObject(root, "restricted_edges", (double)restricted);
    cJSON_AddNumberToObject(root, "places_indexed", (double)db_place_count(app->db));
    db_import_meta_t m;
    if (db_latest_import_meta(app->db, &m)) {
        cJSON *im = cJSON_AddObjectToObject(root, "import");
        cJSON_AddStringToObject(im, "dataset_version", m.dataset_version);
        cJSON_AddStringToObject(im, "source", m.source);
        cJSON_AddNumberToObject(im, "import_time", (double)m.import_time);
        cJSON_AddStringToObject(im, "status", m.import_status);
        cJSON *rep = cJSON_Parse(m.report_json);
        if (rep) cJSON_AddItemToObject(im, "report", rep);
    }
    cJSON_AddStringToObject(root, "coverage_note",
        app->sample_mode
            ? "Synthetic test network only. No real-world coverage."
            : "Coverage is limited to roads present and routable in the imported OpenStreetMap extract; "
              "minor village roads may be missing or unconnected.");
    respond(resp, 200, root);
}

static void h_locations_search(app_t *app, const api_request_t *req, api_response_t *resp)
{
    const char *q = arg(req, "q");
    if (!q || !*q) { error_resp(resp, 400, "MISSING_FIELD", "q", "Query parameter 'q' is required"); return; }
    if (strlen(q) > 100) { error_resp(resp, 400, "INVALID_FIELD", "q", "'q' must be at most 100 characters"); return; }
    long limit, offset;
    if (!parse_int_arg(req, "limit", 1, LOCATION_MAX_RESULTS, 10, &limit)) {
        error_resp(resp, 400, "INVALID_FIELD", "limit", "'limit' must be 1-%d", LOCATION_MAX_RESULTS); return;
    }
    if (!parse_int_arg(req, "offset", 0, 1000, 0, &offset)) {
        error_resp(resp, 400, "INVALID_FIELD", "offset", "'offset' must be 0-1000"); return;
    }
    place_result_t res[LOCATION_MAX_RESULTS];
    int n = location_search(app->db, q, (int)limit, (int)offset, res, LOCATION_MAX_RESULTS);
    if (n < 0) { error_resp(resp, 500, "SEARCH_FAILED", NULL, "Location search failed"); return; }

    cJSON *root = success_obj();
    cJSON_AddStringToObject(root, "query", q);
    cJSON_AddNumberToObject(root, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(root, "results");
    for (int i = 0; i < n; i++) {
        place_result_t *p = &res[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", (double)p->id);
        cJSON_AddStringToObject(j, "name", p->name);
        if (p->name_en[0] && strcmp(p->name_en, p->name) != 0) cJSON_AddStringToObject(j, "name_en", p->name_en);
        cJSON_AddStringToObject(j, "type", p->type);
        if (p->district[0]) cJSON_AddStringToObject(j, "district", p->district);
        if (p->state[0]) cJSON_AddStringToObject(j, "state", p->state);
        if (p->near[0]) cJSON_AddStringToObject(j, "near", p->near);
        char label[600];
        snprintf(label, sizeof label, "%s%s%s%s%s%s%s",
                 p->name_en[0] ? p->name_en : p->name,
                 p->district[0] ? ", " : "", p->district,
                 p->state[0] ? ", " : "", p->state,
                 (!p->district[0] && !p->state[0] && p->near[0]) ? ", near " : "",
                 (!p->district[0] && !p->state[0] && p->near[0]) ? p->near : "");
        cJSON_AddStringToObject(j, "label", label);
        cJSON_AddNumberToObject(j, "latitude", round6(p->lat));
        cJSON_AddNumberToObject(j, "longitude", round6(p->lon));
        if (p->population > 0) cJSON_AddNumberToObject(j, "population", (double)p->population);
        /* Only claim a place is routable once it snaps to the road network. */
        node_idx_t node;
        double d;
        bool ok = spatial_snap_node(app->si, app->g, p->lat, p->lon, app->cfg.max_snap_m, &node, &d);
        cJSON_AddBoolToObject(j, "routable", ok);
        if (ok) cJSON_AddNumberToObject(j, "snap_distance_meters", round1(d));
        cJSON_AddItemToArray(arr, j);
    }
    if (db_place_count(app->db) <= 0)
        cJSON_AddStringToObject(root, "note", "Place index is empty; import data with --db or click the map.");
    respond(resp, 200, root);
}

static void h_locations_snap(app_t *app, const api_request_t *req, api_response_t *resp)
{
    double lat, lon;
    if (!parse_double_str(arg(req, "lat"), &lat) || !parse_double_str(arg(req, "lon"), &lon) ||
        !geo_valid_coord(lat, lon)) {
        error_resp(resp, 400, "INVALID_COORDINATES", NULL, "'lat' and 'lon' query parameters must be valid coordinates");
        return;
    }
    node_idx_t n;
    double d;
    cJSON *root = success_obj();
    bool ok = spatial_snap_node(app->si, app->g, lat, lon, app->cfg.max_snap_m, &n, &d);
    cJSON_AddBoolToObject(root, "routable", ok);
    if (ok) {
        cJSON_AddNumberToObject(root, "latitude", round6(app->g->nodes[n].lat));
        cJSON_AddNumberToObject(root, "longitude", round6(app->g->nodes[n].lon));
        cJSON_AddNumberToObject(root, "snap_distance_meters", round1(d));
        cJSON_AddNumberToObject(root, "node_index", n);
    } else {
        cJSON_AddStringToObject(root, "message", "No road within the maximum snapping distance");
    }
    respond(resp, 200, root);
}

/* ---------------- routes ---------------- */

typedef enum { RK_DEFAULT, RK_SHORTEST, RK_ALTERNATIVES } route_kind_t;

static void h_route(app_t *app, const api_request_t *req, api_response_t *resp, route_kind_t kind)
{
    static const char *const keys[] = {"origin", "destination", "mode", "algorithm",
                                       "alternatives", "avoid", "include_polyline", NULL};
    cJSON *body = parse_body(req, resp);
    if (!body) return;
    route_request_t rq = {0};
    double olat, olon, dlat, dlon;
    long alts = 0;
    const char *mode = "fastest", *algo = "dijkstra";
    bool polyline = false;

    if (!only_keys(body, keys, NULL, resp) ||
        !get_point(body, "origin", &olat, &olon, resp) ||
        !get_point(body, "destination", &dlat, &dlon, resp) ||
        !get_string(body, "mode", kind == RK_SHORTEST ? "shortest" : "fastest", &mode, resp) ||
        !get_string(body, "algorithm", "dijkstra", &algo, resp) ||
        !get_int(body, "alternatives", 0, MAX_ALTERNATIVES, kind == RK_ALTERNATIVES ? 2 : 0, &alts, resp) ||
        !get_bool(body, "include_polyline", false, &polyline, resp))
        goto done;

    if (strcmp(mode, "fastest") == 0) rq.mode = COST_TIME;
    else if (strcmp(mode, "shortest") == 0) rq.mode = COST_DISTANCE;
    else { error_resp(resp, 400, "INVALID_MODE", "mode", "'mode' must be 'fastest' or 'shortest'"); goto done; }
    if (kind == RK_SHORTEST && rq.mode != COST_DISTANCE) {
        error_resp(resp, 400, "INVALID_MODE", "mode", "/routes/shortest only supports mode 'shortest'"); goto done;
    }
    if (kind == RK_ALTERNATIVES && alts < 1) {
        error_resp(resp, 400, "INVALID_FIELD", "alternatives", "/routes/alternatives needs alternatives >= 1"); goto done;
    }
    if (strcmp(algo, "dijkstra") == 0) rq.algorithm = ALGO_DIJKSTRA;
    else if (strcmp(algo, "astar") == 0) rq.algorithm = ALGO_ASTAR;
    else { error_resp(resp, 400, "INVALID_FIELD", "algorithm", "'algorithm' must be 'dijkstra' or 'astar'"); goto done; }

    const cJSON *avoid = cJSON_GetObjectItemCaseSensitive(body, "avoid");
    if (avoid) {
        static const char *const akeys[] = {"tolls", "restricted", NULL};
        if (!cJSON_IsObject(avoid)) { error_resp(resp, 400, "INVALID_FIELD", "avoid", "'avoid' must be an object"); goto done; }
        if (!only_keys(avoid, akeys, "avoid", resp) ||
            !get_bool(avoid, "tolls", false, &rq.avoid_tolls, resp) ||
            !get_bool(avoid, "restricted", false, &rq.avoid_restricted, resp))
            goto done;
    }

    if (!in_coverage(app, olat, olon) || !in_coverage(app, dlat, dlon)) {
        error_resp(resp, 422, "OUTSIDE_COVERAGE", in_coverage(app, olat, olon) ? "destination" : "origin",
                   "Point is outside the area covered by the loaded road dataset");
        goto done;
    }
    double osnap, dsnap;
    if (!spatial_snap_node(app->si, app->g, olat, olon, app->cfg.max_snap_m, &rq.source, &osnap)) {
        error_resp(resp, 422, "NOT_ON_NETWORK", "origin", "No road found within %.0f m of the origin", app->cfg.max_snap_m);
        goto done;
    }
    if (!spatial_snap_node(app->si, app->g, dlat, dlon, app->cfg.max_snap_m, &rq.target, &dsnap)) {
        error_resp(resp, 422, "NOT_ON_NETWORK", "destination", "No road found within %.0f m of the destination", app->cfg.max_snap_m);
        goto done;
    }
    rq.max_settled = app->cfg.max_settled;

    int64_t now = app_now();
    app_maybe_expire(app, now);
    route_workspace_t *ws = app_acquire_workspace(app, 5000);
    if (!ws) {
        resp->retry_after = 2;
        error_resp(resp, 503, "SERVER_BUSY", NULL, "All route workers are busy; retry shortly");
        goto done;
    }

    double t0 = app_monotonic_s();
    route_result_t best, alt[MAX_ALTERNATIVES];
    size_t nalt = 0;
    route_geometry_t gbest, galt[MAX_ALTERNATIVES];
    memset(&gbest, 0, sizeof gbest);
    memset(galt, 0, sizeof galt);

    pthread_rwlock_rdlock(&app->lock);
    route_status_t st = route_with_alternatives(app->g, &rq, ws, NULL, (size_t)alts, &best, alt, &nalt);
    bool geo_ok = true;
    if (st == ROUTE_OK) {
        geo_ok = route_geometry_build(app->g, &best, &gbest);
        for (size_t i = 0; i < nalt && geo_ok; i++) geo_ok = route_geometry_build(app->g, &alt[i], &galt[i]);
    }
    cJSON *root = NULL;
    if (st == ROUTE_OK && geo_ok) {
        double ms = (app_monotonic_s() - t0) * 1000.0;
        root = success_obj();
        cJSON_AddStringToObject(root, "routing_mode", rq.mode == COST_TIME ? "fastest" : "shortest");
        cJSON_AddStringToObject(root, "algorithm", rq.algorithm == ALGO_ASTAR ? "astar" : "dijkstra");
        cJSON_AddNumberToObject(root, "distance_meters", round1(best.distance_m));
        cJSON_AddNumberToObject(root, "estimated_duration_seconds", round1(best.duration_s));
        cJSON_AddItemToObject(root, "traffic_data", traffic_data_json(app, now));
        cJSON *sn = cJSON_AddObjectToObject(root, "snapped");
        cJSON *so = cJSON_AddObjectToObject(sn, "origin");
        cJSON_AddNumberToObject(so, "latitude", round6(app->g->nodes[rq.source].lat));
        cJSON_AddNumberToObject(so, "longitude", round6(app->g->nodes[rq.source].lon));
        cJSON_AddNumberToObject(so, "snap_distance_meters", round1(osnap));
        cJSON *sd = cJSON_AddObjectToObject(sn, "destination");
        cJSON_AddNumberToObject(sd, "latitude", round6(app->g->nodes[rq.target].lat));
        cJSON_AddNumberToObject(sd, "longitude", round6(app->g->nodes[rq.target].lon));
        cJSON_AddNumberToObject(sd, "snap_distance_meters", round1(dsnap));
        cJSON *rj = route_json(app, &best, &gbest, polyline);
        cJSON *wp = cJSON_AddArrayToObject(rj, "waypoints");
        cJSON_AddItemToArray(wp, latlon_array(app->g->nodes[rq.source].lat, app->g->nodes[rq.source].lon));
        cJSON_AddItemToArray(wp, latlon_array(app->g->nodes[rq.target].lat, app->g->nodes[rq.target].lon));
        cJSON_AddItemToObject(root, "route", rj);
        cJSON *aa = cJSON_AddArrayToObject(root, "alternatives");
        for (size_t i = 0; i < nalt; i++) cJSON_AddItemToArray(aa, route_json(app, &alt[i], &galt[i], polyline));
        if ((size_t)alts > nalt)
            cJSON_AddStringToObject(root, "alternatives_note",
                nalt ? "Fewer alternatives than requested: no other sufficiently different route was found"
                     : "No sufficiently different alternative route was found");
        cJSON_AddStringToObject(root, "instructions_note",
            "Basic instructions derived from road names and bearings; turn restrictions and lanes are not modelled.");
        cJSON_AddNumberToObject(root, "compute_ms", round1(ms));
        cJSON_AddStringToObject(root, "dataset", app->g->dataset);
        cJSON_AddStringToObject(root, "attribution",
                                app->osm_data ? "Road data (c) OpenStreetMap contributors, ODbL"
                                              : "Synthetic sample network - not real roads");
        atomic_fetch_add(&app->routes_ok, 1);
        atomic_fetch_add(&app->route_us_total, (long)(ms * 1000.0));
    }
    pthread_rwlock_unlock(&app->lock);
    app_release_workspace(app, ws);

    if (root) respond(resp, 200, root);
    else {
        atomic_fetch_add(&app->routes_failed, 1);
        if (st == ROUTE_ERR_NO_PATH)
            error_resp(resp, 404, "NO_ROUTE", NULL,
                       "No route found with the selected options: the points may be on disconnected roads, "
                       "or every connection is closed or excluded by the avoid options");
        else if (st == ROUTE_ERR_LIMIT)
            error_resp(resp, 422, "ROUTE_TOO_COMPLEX", NULL,
                       "Route search exceeded the configured effort limit (IRA_MAX_SETTLED)");
        else
            error_resp(resp, 500, "INTERNAL", NULL, "Route computation failed: %s",
                       st == ROUTE_OK ? "geometry error" : route_status_str(st));
    }
    route_result_free(&best);
    route_geometry_free(&gbest);
    for (size_t i = 0; i < nalt; i++) { route_result_free(&alt[i]); route_geometry_free(&galt[i]); }
done:
    cJSON_Delete(body);
}

/* ---------------- traffic segments ---------------- */

static int cmp_edge_idx(const void *a, const void *b)
{
    edge_idx_t x = *(const edge_idx_t *)a, y = *(const edge_idx_t *)b;
    return (x > y) - (x < y);
}

static bool parse_bbox(const char *s, double *min_lon, double *min_lat, double *max_lon, double *max_lat)
{
    if (!s) return false;
    char buf[200];
    if (strlen(s) >= sizeof buf) return false;
    snprintf(buf, sizeof buf, "%s", s);
    double v[4];
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        char *c = strchr(p, ',');
        if (i < 3 && !c) return false;
        if (c) *c = '\0';
        if (!parse_double_str(p, &v[i])) return false;
        p = c ? c + 1 : p;
    }
    *min_lon = v[0]; *min_lat = v[1]; *max_lon = v[2]; *max_lat = v[3];
    return geo_valid_coord(v[1], v[0]) && geo_valid_coord(v[3], v[2]) && v[0] < v[2] && v[1] < v[3];
}

static void edge_geometry_json(const graph_t *g, edge_idx_t ei, cJSON *arr)
{
    const graph_edge_t *e = &g->edges[ei];
    cJSON_AddItemToArray(arr, latlon_array(g->nodes[e->from].lat, g->nodes[e->from].lon));
    for (uint32_t s = 0; s < e->geom_count; s++)
        cJSON_AddItemToArray(arr, latlon_array(g->shape[e->geom_offset + s].lat, g->shape[e->geom_offset + s].lon));
    cJSON_AddItemToArray(arr, latlon_array(g->nodes[e->to].lat, g->nodes[e->to].lon));
}

static void h_traffic_segments(app_t *app, const api_request_t *req, api_response_t *resp)
{
    double mnlo, mnla, mxlo, mxla;
    if (!parse_bbox(arg(req, "bbox"), &mnlo, &mnla, &mxlo, &mxla)) {
        error_resp(resp, 400, "INVALID_FIELD", "bbox", "'bbox' must be minLon,minLat,maxLon,maxLat");
        return;
    }
    if ((mxlo - mnlo) * (mxla - mnla) > 25.0) {
        error_resp(resp, 400, "BBOX_TOO_LARGE", "bbox", "bbox area must be at most 25 square degrees");
        return;
    }
    int64_t now = app_now();
    app_maybe_expire(app, now);
    cJSON *root = success_obj();
    pthread_rwlock_rdlock(&app->lock);
    cJSON_AddItemToObject(root, "traffic_data", traffic_data_json(app, now));
    const traffic_state_t *t = &app->traffic;
    size_t n = t->modified_count;
    edge_idx_t *edges = malloc((n ? n : 1) * sizeof *edges);
    cJSON *arr = cJSON_AddArrayToObject(root, "segments");
    size_t out = 0;
    if (edges) {
        if (n) {
            memcpy(edges, t->modified, n * sizeof *edges);
            qsort(edges, n, sizeof *edges, cmp_edge_idx);
        }
        for (size_t i = 0; i < n && out < 2000; i++) {
            if (i && edges[i] == edges[i - 1]) continue;
            edge_idx_t ei = edges[i];
            const graph_edge_t *e = &app->g->edges[ei];
            const graph_node_t *a = &app->g->nodes[e->from];
            if (a->lat < mnla || a->lat > mxla || a->lon < mnlo || a->lon > mxlo) continue;
            /* newest observation for this edge, if any (obs sorted by time) */
            const traffic_obs_t *o = NULL;
            for (size_t k = t->obs_count; k-- > 0;)
                if (t->obs[k].edge == ei) { o = &t->obs[k]; break; }
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "edge_index", ei);
            const char *nm = graph_edge_name(app->g, e);
            if (nm[0]) cJSON_AddStringToObject(j, "road", nm);
            cJSON_AddBoolToObject(j, "closed", (e->flags & EDGE_FLAG_CLOSED) != 0);
            cJSON_AddNumberToObject(j, "baseline_seconds", round1((double)e->base_time_s));
            cJSON_AddNumberToObject(j, "current_seconds", round1(graph_edge_time(e)));
            cJSON_AddNumberToObject(j, "delay_ratio", round(graph_edge_time(e) / (double)e->base_time_s * 100) / 100);
            cJSON_AddStringToObject(j, "source", o ? traffic_source_str(o->source) : "incident");
            cJSON_AddBoolToObject(j, "simulated", o ? o->source == TSRC_SIMULATED : false);
            if (o) {
                cJSON_AddNumberToObject(j, "confidence", round(o->confidence * 100) / 100);
                cJSON_AddNumberToObject(j, "observed_at", (double)o->observed_at);
                cJSON_AddNumberToObject(j, "expires_at", (double)o->expires_at);
            }
            edge_geometry_json(app->g, ei, cJSON_AddArrayToObject(j, "geometry"));
            cJSON_AddItemToArray(arr, j);
            out++;
        }
        free(edges);
    }
    pthread_rwlock_unlock(&app->lock);
    cJSON_AddNumberToObject(root, "count", (double)out);
    respond(resp, 200, root);
}

/* ---------------- incidents ---------------- */

static void list_incidents(app_t *app, api_response_t *resp, bool admin, const char *status_filter)
{
    int64_t now = app_now();
    app_maybe_expire(app, now);
    cJSON *root = success_obj();
    cJSON *arr = cJSON_AddArrayToObject(root, "incidents");
    pthread_rwlock_rdlock(&app->lock);
    for (size_t i = 0; i < app->traffic.inc_count; i++) {
        const incident_t *in = &app->traffic.inc[i];
        if (!admin && in->status != INC_VERIFIED) continue;
        if (admin && status_filter && strcmp(status_filter, "all") != 0 &&
            strcmp(status_filter, incident_status_str(in->status)) != 0) continue;
        cJSON_AddItemToArray(arr, incident_json(app, in));
    }
    pthread_rwlock_unlock(&app->lock);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
    if (!admin) cJSON_AddStringToObject(root, "note", "Only verified incidents are shown and used for routing.");
    respond(resp, 200, root);
}

static void create_incident(app_t *app, const api_request_t *req, api_response_t *resp, bool admin)
{
    static const char *const keys[] = {"type", "latitude", "longitude", "description",
                                       "expires_in_minutes", "both_directions", "verified",
                                       "simulated", NULL};
    cJSON *body = parse_body(req, resp);
    if (!body) return;
    const char *type_s = NULL, *desc = "";
    double lat, lon;
    long minutes;
    bool both, verified = false, simulated = false;
    incident_type_t type;
    if (!only_keys(body, keys, NULL, resp) ||
        !get_string(body, "type", NULL, &type_s, resp) ||
        !get_number(body, "latitude", -90, 90, true, 0, &lat, resp) ||
        !get_number(body, "longitude", -180, 180, true, 0, &lon, resp) ||
        !get_string(body, "description", "", &desc, resp) ||
        !get_int(body, "expires_in_minutes", 5, 1440, 120, &minutes, resp) ||
        !get_bool(body, "both_directions", true, &both, resp) ||
        !get_bool(body, "verified", false, &verified, resp) ||
        !get_bool(body, "simulated", false, &simulated, resp))
        goto done;
    if (!type_s || !incident_type_from_str(type_s, &type)) {
        error_resp(resp, 400, "INVALID_FIELD", "type",
                   "'type' must be one of accident, closure, congestion, roadwork, hazard");
        goto done;
    }
    if (strlen(desc) > 200) { error_resp(resp, 400, "INVALID_FIELD", "description", "'description' max 200 characters"); goto done; }
    if (!admin && (verified || simulated)) {
        error_resp(resp, 403, "FORBIDDEN_FIELD", verified ? "verified" : "simulated",
                   "Only administrators can create verified or simulated incidents");
        goto done;
    }
    edge_idx_t e;
    double d;
    if (!spatial_nearest_edge(app->si, app->g, lat, lon, 150.0, &e, &d)) {
        error_resp(resp, 422, "NOT_ON_NETWORK", NULL, "No road within 150 m of the reported location");
        goto done;
    }
    char clean[256];
    snprintf(clean, sizeof clean, "%s", desc);
    sanitize_text(clean);

    int64_t now = app_now();
    uint64_t id = 0;
    pthread_rwlock_wrlock(&app->lock);
    int rc = traffic_add_incident(&app->traffic, type, verified ? INC_VERIFIED : INC_PENDING, lat, lon, e,
                                  both, now, now + minutes * 60, simulated, clean, &id);
    cJSON *root = NULL;
    if (rc == 0) {
        const incident_t *in = traffic_find_incident(&app->traffic, id);
        if (!db_save_incident(app->db, in, app->g->dataset)) LOGW("Could not persist incident %llu", (unsigned long long)id);
        root = success_obj();
        cJSON_AddItemToObject(root, "incident", incident_json(app, in));
        cJSON_AddNumberToObject(root, "snap_distance_meters", round1(d));
        cJSON_AddStringToObject(root, "message",
            verified ? "Incident created and applied to routing"
                     : "Incident received and pending moderation; it will not affect routes until verified");
    }
    pthread_rwlock_unlock(&app->lock);
    if (root) respond(resp, verified ? 201 : 202, root);
    else if (rc == -3) error_resp(resp, 503, "CAPACITY", NULL, "Incident store is full");
    else error_resp(resp, 400, "INVALID_INCIDENT", NULL, "Incident could not be created");
done:
    cJSON_Delete(body);
}

static void moderate_incident(app_t *app, api_response_t *resp, uint64_t id, incident_status_t st)
{
    pthread_rwlock_wrlock(&app->lock);
    int rc = traffic_set_incident_status(&app->traffic, id, st);
    cJSON *root = NULL;
    if (rc == 0) {
        const incident_t *in = traffic_find_incident(&app->traffic, id);
        db_update_incident_status(app->db, id, st);
        root = success_obj();
        cJSON_AddItemToObject(root, "incident", incident_json(app, in));
    }
    pthread_rwlock_unlock(&app->lock);
    if (root) respond(resp, 200, root);
    else if (rc == -2) error_resp(resp, 409, "INCIDENT_EXPIRED", NULL, "Incident has expired");
    else error_resp(resp, 404, "NOT_FOUND", NULL, "No incident with id %llu", (unsigned long long)id);
}

/* ---------------- admin ---------------- */

static bool require_admin(app_t *app, const api_request_t *req, api_response_t *resp)
{
    if (!config_admin_enabled(&app->cfg)) {
        error_resp(resp, 403, "ADMIN_DISABLED", NULL, "Admin API is disabled (IRA_ADMIN_TOKEN not configured)");
        return false;
    }
    const char *h = req->authorization;
    if (!h || strncmp(h, "Bearer ", 7) != 0 || !api_token_equal(h + 7, app->cfg.admin_token)) {
        error_resp(resp, 401, "UNAUTHORIZED", NULL, "Valid 'Authorization: Bearer <token>' header required");
        return false;
    }
    return true;
}

static void h_admin_traffic_updates(app_t *app, const api_request_t *req, api_response_t *resp)
{
    static const char *const keys[] = {"updates", NULL};
    static const char *const ukeys[] = {"edge_index", "latitude", "longitude", "travel_time_seconds",
                                        "speed_kmh", "confidence", "expires_in_seconds", NULL};
    cJSON *body = parse_body(req, resp);
    if (!body) return;
    if (!only_keys(body, keys, NULL, resp)) goto done;
    const cJSON *ups = cJSON_GetObjectItemCaseSensitive(body, "updates");
    if (!cJSON_IsArray(ups) || cJSON_GetArraySize(ups) < 1 || cJSON_GetArraySize(ups) > 1000) {
        error_resp(resp, 400, "INVALID_FIELD", "updates", "'updates' must be an array of 1-1000 items");
        goto done;
    }
    int64_t now = app_now();
    cJSON *root = success_obj();
    cJSON *results = cJSON_AddArrayToObject(root, "results");
    size_t accepted = 0;
    pthread_rwlock_wrlock(&app->lock);
    const cJSON *u;
    int idx = 0;
    cJSON_ArrayForEach(u, ups) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "index", idx++);
        api_response_t tmp = {0};
        double ei_d, lat, lon, tt, sp, conf, exp_s;
        edge_idx_t e = IRA_INVALID_IDX;
        const char *err = NULL;
        if (!cJSON_IsObject(u) || !only_keys(u, ukeys, "updates[]", &tmp)) err = "invalid or unknown field";
        else if (!get_number(u, "edge_index", 0, 4294967294.0, false, -1, &ei_d, &tmp) ||
                 !get_number(u, "latitude", -90, 90, false, NAN, &lat, &tmp) ||
                 !get_number(u, "longitude", -180, 180, false, NAN, &lon, &tmp) ||
                 !get_number(u, "travel_time_seconds", 0.1, 1e6, false, -1, &tt, &tmp) ||
                 !get_number(u, "speed_kmh", 0.5, 200, false, -1, &sp, &tmp) ||
                 !get_number(u, "confidence", 0, 1, false, 1.0, &conf, &tmp) ||
                 !get_number(u, "expires_in_seconds", 60, 86400, false, 900, &exp_s, &tmp))
            err = "field out of range";
        else {
            double d;
            if (ei_d >= 0) {
                if (ei_d == floor(ei_d) && ei_d < (double)app->g->edge_count) e = (edge_idx_t)ei_d;
                else err = "edge_index out of range";
            } else if (isfinite(lat) && isfinite(lon)) {
                if (!spatial_nearest_edge(app->si, app->g, lat, lon, 50.0, &e, &d)) err = "no road within 50 m";
            } else err = "edge_index or latitude/longitude required";
            if (!err && tt < 0 && sp < 0) err = "travel_time_seconds or speed_kmh required";
        }
        if (!err) {
            double time_s = tt > 0 ? tt : (double)app->g->edges[e].distance_m / (sp / 3.6);
            uint64_t oid;
            int rc = traffic_add_observation_noapply(&app->traffic, e, time_s, conf, TSRC_ADMIN, now,
                                                     now + (int64_t)exp_s, "admin", now, &oid);
            if (rc == 0) {
                accepted++;
                cJSON_AddBoolToObject(r, "accepted", true);
                cJSON_AddNumberToObject(r, "edge_index", e);
                cJSON_AddNumberToObject(r, "travel_time_seconds", round1(time_s));
                const traffic_obs_t *o = &app->traffic.obs[app->traffic.obs_count - 1];
                db_log_observation(app->db, o, app->g->nodes[app->g->edges[e].from].lat,
                                   app->g->nodes[app->g->edges[e].from].lon);
            } else err = rc == -3 ? "observation store full" : "rejected";
        }
        if (err) {
            cJSON_AddBoolToObject(r, "accepted", false);
            cJSON_AddStringToObject(r, "error", err);
        }
        api_response_free(&tmp);
        cJSON_AddItemToArray(results, r);
    }
    if (accepted) traffic_reapply_all(&app->traffic);
    pthread_rwlock_unlock(&app->lock);
    cJSON_AddNumberToObject(root, "accepted", (double)accepted);
    cJSON_AddNumberToObject(root, "rejected", (double)(cJSON_GetArraySize(ups) - (int)accepted));
    respond(resp, 200, root);
done:
    cJSON_Delete(body);
}

static void h_admin_simulate(app_t *app, const api_request_t *req, api_response_t *resp)
{
    static const char *const keys[] = {"latitude", "longitude", "radius_m", "count",
                                       "duration_seconds", "seed", NULL};
    cJSON *body = parse_body(req, resp);
    if (!body) return;
    double lat, lon, radius;
    long count, dur, seed;
    if (!only_keys(body, keys, NULL, resp) ||
        !get_number(body, "latitude", -90, 90, true, 0, &lat, resp) ||
        !get_number(body, "longitude", -180, 180, true, 0, &lon, resp) ||
        !get_number(body, "radius_m", 50, 50000, false, 3000, &radius, resp) ||
        !get_int(body, "count", 1, 500, 25, &count, resp) ||
        !get_int(body, "duration_seconds", 60, 86400, 3600, &dur, resp) ||
        !get_int(body, "seed", 0, 2147483647L, 1, &seed, resp))
        goto done;
    int64_t now = app_now();
    pthread_rwlock_wrlock(&app->lock);
    size_t made = traffic_simulate(&app->traffic, app->si, lat, lon, radius, (size_t)count,
                                   (uint32_t)seed, now, dur);
    pthread_rwlock_unlock(&app->lock);
    cJSON *root = success_obj();
    cJSON_AddNumberToObject(root, "simulated_segments", (double)made);
    cJSON_AddNumberToObject(root, "expires_at", (double)(now + dur));
    cJSON_AddStringToObject(root, "message",
        "Simulated congestion added. API responses label it as simulated; it is not real traffic.");
    respond(resp, 200, root);
done:
    cJSON_Delete(body);
}

static void h_admin_clear_traffic(app_t *app, const api_request_t *req, api_response_t *resp)
{
    const char *src = arg(req, "source");
    traffic_source_t s;
    if (!src || strcmp(src, "simulated") == 0) s = TSRC_SIMULATED;
    else if (strcmp(src, "admin") == 0) s = TSRC_ADMIN;
    else if (strcmp(src, "live") == 0) s = TSRC_LIVE;
    else { error_resp(resp, 400, "INVALID_FIELD", "source", "'source' must be simulated, admin or live"); return; }
    pthread_rwlock_wrlock(&app->lock);
    size_t n = traffic_clear_source(&app->traffic, s);
    pthread_rwlock_unlock(&app->lock);
    cJSON *root = success_obj();
    cJSON_AddStringToObject(root, "source", traffic_source_str(s));
    cJSON_AddNumberToObject(root, "removed", (double)n);
    respond(resp, 200, root);
}

/* ======================================================================
 * Dispatcher
 * ====================================================================== */

static bool parse_incident_action(const char *path, uint64_t *id, const char **action)
{
    const char *prefix = "/api/v1/admin/incidents/";
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl) != 0) return false;
    const char *p = path + pl;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 10);
    if (errno || end == p || *end != '/' || v == 0) return false;
    *id = v;
    *action = end + 1;
    return true;
}

void api_handle(app_t *app, const api_request_t *req, api_response_t *resp)
{
    memset(resp, 0, sizeof *resp);
    atomic_fetch_add(&app->requests, 1);
    const char *m = req->method ? req->method : "";
    const char *p = req->path ? req->path : "";
    const char *ip = req->client_ip ? req->client_ip : "unknown";
    double mono = app_monotonic_s();

    rl_class_t cls = RL_GENERAL;
    if (strcmp(m, "POST") == 0 && strncmp(p, "/api/v1/routes", 14) == 0) cls = RL_ROUTE;
    else if (strcmp(m, "POST") == 0 && strcmp(p, "/api/v1/incidents") == 0) cls = RL_INCIDENT;
    int retry = 0;
    if (!rl_allow(app->rl, ip, cls, mono, &retry) ||
        (cls != RL_GENERAL && !rl_allow(app->rl, ip, RL_GENERAL, mono, &retry))) {
        resp->retry_after = retry;
        error_resp(resp, 429, "RATE_LIMITED", NULL, "Too many requests; retry after %d s", retry);
        return;
    }

    bool get = strcmp(m, "GET") == 0, post = strcmp(m, "POST") == 0, del = strcmp(m, "DELETE") == 0;
    uint64_t inc_id;
    const char *action;

    if (strcmp(p, "/api/v1/health") == 0) { if (get) { h_health(app, resp); return; } }
    else if (strcmp(p, "/api/v1/config") == 0) { if (get) { h_config(app, resp); return; } }
    else if (strcmp(p, "/api/v1/network/stats") == 0) { if (get) { h_network_stats(app, resp); return; } }
    else if (strcmp(p, "/api/v1/locations/search") == 0) { if (get) { h_locations_search(app, req, resp); return; } }
    else if (strcmp(p, "/api/v1/locations/snap") == 0) { if (get) { h_locations_snap(app, req, resp); return; } }
    else if (strcmp(p, "/api/v1/routes") == 0) { if (post) { h_route(app, req, resp, RK_DEFAULT); return; } }
    else if (strcmp(p, "/api/v1/routes/shortest") == 0) { if (post) { h_route(app, req, resp, RK_SHORTEST); return; } }
    else if (strcmp(p, "/api/v1/routes/alternatives") == 0) { if (post) { h_route(app, req, resp, RK_ALTERNATIVES); return; } }
    else if (strcmp(p, "/api/v1/traffic/segments") == 0) { if (get) { h_traffic_segments(app, req, resp); return; } }
    else if (strcmp(p, "/api/v1/incidents") == 0) {
        if (get) { list_incidents(app, resp, false, NULL); return; }
        if (post) { create_incident(app, req, resp, false); return; }
    }
    else if (strcmp(p, "/api/v1/admin/traffic/updates") == 0) {
        if (post) { if (require_admin(app, req, resp)) h_admin_traffic_updates(app, req, resp); return; }
    }
    else if (strcmp(p, "/api/v1/admin/traffic/simulate") == 0) {
        if (post) { if (require_admin(app, req, resp)) h_admin_simulate(app, req, resp); return; }
    }
    else if (strcmp(p, "/api/v1/admin/traffic") == 0) {
        if (del) { if (require_admin(app, req, resp)) h_admin_clear_traffic(app, req, resp); return; }
    }
    else if (strcmp(p, "/api/v1/admin/incidents") == 0) {
        if (get) { if (require_admin(app, req, resp)) list_incidents(app, resp, true, arg(req, "status")); return; }
        if (post) { if (require_admin(app, req, resp)) create_incident(app, req, resp, true); return; }
    }
    else if (parse_incident_action(p, &inc_id, &action)) {
        if (post && (strcmp(action, "verify") == 0 || strcmp(action, "reject") == 0)) {
            if (require_admin(app, req, resp))
                moderate_incident(app, resp, inc_id, action[0] == 'v' ? INC_VERIFIED : INC_REJECTED);
            return;
        }
        error_resp(resp, 404, "NOT_FOUND", NULL, "Unknown incident action");
        return;
    }
    else {
        error_resp(resp, 404, "NOT_FOUND", NULL, "Unknown endpoint");
        return;
    }
    error_resp(resp, 405, "METHOD_NOT_ALLOWED", NULL, "Method %s not allowed for %s", m, p);
}
