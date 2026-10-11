/*
 * REST API tests: api_handle() called directly (no sockets) on the sample
 * network. Covers success schemas, validation errors, admin auth, traffic
 * and incident flows, rate limiting.
 */
#include "api.h"
#include "cJSON.h"
#include "test_util.h"

#include <string.h>

#define ADMIN "Bearer test-admin-token-0123456789"

static app_t APP;

/* --- tiny query-string helper: ctx is "a=1&b=2" (no URL decoding needed here) --- */
static const char *qs_get(void *ctx, const char *key)
{
    static char bufs[4][256];
    static int which = 0;
    const char *qs = ctx;
    size_t kl = strlen(key);
    for (const char *p = qs; p && *p;) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            char *out = bufs[which++ % 4];
            size_t n = strcspn(p + kl + 1, "&");
            if (n > 255) n = 255;
            memcpy(out, p + kl + 1, n);
            out[n] = '\0';
            return out;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

static cJSON *call_ip(app_t *app, const char *method, const char *path, const char *query,
                      const char *body, const char *auth, const char *ip, int *status)
{
    api_request_t req = { .method = method, .path = path, .get_arg = query ? qs_get : NULL,
                          .arg_ctx = (void *)query, .body = body, .body_len = body ? strlen(body) : 0,
                          .client_ip = ip, .authorization = auth };
    api_response_t resp;
    api_handle(app, &req, &resp);
    *status = resp.status;
    cJSON *j = cJSON_Parse(resp.body);
    api_response_free(&resp);
    return j;
}

static cJSON *call(const char *method, const char *path, const char *query, const char *body,
                   const char *auth, int *status)
{
    return call_ip(&APP, method, path, query, body, auth, "127.0.0.1", status);
}

static const char *err_code(const cJSON *j)
{
    const cJSON *e = cJSON_GetObjectItem(j, "error");
    const cJSON *c = cJSON_GetObjectItem(e, "code");
    return cJSON_IsString(c) ? c->valuestring : "";
}

static double num(const cJSON *j, const char *k) { return cJSON_GetNumberValue(cJSON_GetObjectItem(j, k)); }

#define ROUTE_1_TO_4 "{\"origin\":{\"latitude\":17.0,\"longitude\":79.0},\"destination\":{\"latitude\":17.0,\"longitude\":79.06}"

static void expect_error(const char *path, const char *body, int want_status, const char *want_code)
{
    int st;
    cJSON *j = call("POST", path, NULL, body, NULL, &st);
    CHECK(st == want_status);
    if (st != want_status || strcmp(err_code(j), want_code) != 0)
        fprintf(stderr, "    body=%s -> status %d code %s (want %d %s)\n", body ? body : "(null)", st,
                err_code(j), want_status, want_code);
    CHECK(strcmp(err_code(j), want_code) == 0);
    cJSON_Delete(j);
}

static void test_health_and_config(void)
{
    int st;
    cJSON *j = call("GET", "/api/v1/health", NULL, NULL, NULL, &st);
    CHECK(st == 200);
    CHECK(strcmp(cJSON_GetObjectItem(j, "status")->valuestring, "ok") == 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "graph"), "synthetic_sample")));
    cJSON_Delete(j);
    j = call("GET", "/api/v1/config", NULL, NULL, NULL, &st);
    CHECK(st == 200 && cJSON_GetArraySize(cJSON_GetObjectItem(j, "bbox")) == 4);
    cJSON_Delete(j);
    j = call("GET", "/api/v1/network/stats", NULL, NULL, NULL, &st);
    CHECK(st == 200 && num(j, "nodes") == 10 && num(j, "directed_edges") == 22);
    cJSON_Delete(j);
}

static void test_route_success_schema(void)
{
    int st;
    cJSON *j = call("POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 ",\"mode\":\"fastest\",\"include_polyline\":true}", NULL, &st);
    CHECK(st == 200);
    if (st != 200) { cJSON_Delete(j); return; }
    CHECK(strcmp(cJSON_GetObjectItem(j, "routing_mode")->valuestring, "fastest") == 0);
    CHECK_NEAR(num(j, "distance_meters"), 10800, 1.0);
    CHECK_NEAR(num(j, "estimated_duration_seconds"), 555.4, 0.5);
    const cJSON *td = cJSON_GetObjectItem(j, "traffic_data");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(td, "available")));
    CHECK(strstr(cJSON_GetObjectItem(td, "message")->valuestring, "No live traffic") != NULL);
    const cJSON *r = cJSON_GetObjectItem(j, "route");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r, "geometry")) == 4);
    CHECK(cJSON_IsString(cJSON_GetObjectItem(r, "polyline")));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r, "instructions")) == 4);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r, "waypoints")) == 2);
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(j, "alternatives")));
    cJSON_Delete(j);

    j = call("POST", "/api/v1/routes/shortest", NULL, ROUTE_1_TO_4 "}", NULL, &st);
    CHECK(st == 200 && fabs(num(j, "distance_meters") - 6380.2) < 1.0);
    cJSON_Delete(j);

    j = call("POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 ",\"algorithm\":\"astar\",\"avoid\":{\"tolls\":true}}", NULL, &st);
    CHECK(st == 200 && fabs(num(j, "distance_meters") - 7043.5) < 1.0);
    CHECK(strcmp(cJSON_GetObjectItem(j, "algorithm")->valuestring, "astar") == 0);
    cJSON_Delete(j);

    j = call("POST", "/api/v1/routes/alternatives", NULL, ROUTE_1_TO_4 "}", NULL, &st);
    CHECK(st == 200 && cJSON_GetArraySize(cJSON_GetObjectItem(j, "alternatives")) >= 1);
    cJSON_Delete(j);
}

static void test_route_validation(void)
{
    expect_error("/api/v1/routes", NULL, 400, "EMPTY_BODY");
    expect_error("/api/v1/routes", "{bad json", 400, "INVALID_JSON");
    expect_error("/api/v1/routes", "[1,2]", 400, "INVALID_JSON");
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":17,\"longitude\":79}}", 400, "MISSING_FIELD");
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":91,\"longitude\":79},\"destination\":{\"latitude\":17,\"longitude\":79}}", 400, "INVALID_COORDINATES");
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":\"17\",\"longitude\":79},\"destination\":{\"latitude\":17,\"longitude\":79}}", 400, "INVALID_COORDINATES");
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":17,\"longitude\":79,\"x\":1},\"destination\":{\"latitude\":17,\"longitude\":79}}", 400, "UNKNOWN_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"mode\":\"fast\"}", 400, "INVALID_MODE");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"alternatives\":9}", 400, "INVALID_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"alternatives\":1.5}", 400, "INVALID_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"algorithm\":\"bfs\"}", 400, "INVALID_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"avoid\":{\"tolls\":1}}", 400, "INVALID_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"avoid\":{\"ferries\":true}}", 400, "UNKNOWN_FIELD");
    expect_error("/api/v1/routes", ROUTE_1_TO_4 ",\"foo\":1}", 400, "UNKNOWN_FIELD");
    expect_error("/api/v1/routes/shortest", ROUTE_1_TO_4 ",\"mode\":\"fastest\"}", 400, "INVALID_MODE");
    expect_error("/api/v1/routes/alternatives", ROUTE_1_TO_4 ",\"alternatives\":0}", 400, "INVALID_FIELD");
    /* Chennai is far outside the synthetic network */
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":13.08,\"longitude\":80.27},\"destination\":{\"latitude\":17,\"longitude\":79}}", 422, "OUTSIDE_COVERAGE");
    /* inside the bbox but > 5 km from any road */
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":16.95,\"longitude\":79.12},\"destination\":{\"latitude\":17,\"longitude\":79}}", 422, "NOT_ON_NETWORK");
    /* disconnected island I-J */
    expect_error("/api/v1/routes", "{\"origin\":{\"latitude\":17.0,\"longitude\":79.0},\"destination\":{\"latitude\":16.9,\"longitude\":79.2}}", 404, "NO_ROUTE");
}

static void test_endpoints_misc(void)
{
    int st;
    cJSON *j = call("GET", "/api/v1/nope", NULL, NULL, NULL, &st);
    CHECK(st == 404); cJSON_Delete(j);
    j = call("DELETE", "/api/v1/routes", NULL, NULL, NULL, &st);
    CHECK(st == 405); cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/search", "q=junction&limit=5", NULL, NULL, &st);
    CHECK(st == 200 && num(j, "count") == 5);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "results"), 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(first, "routable")));
    CHECK(strstr(cJSON_GetObjectItem(first, "label")->valuestring, "Synthetic") != NULL);
    cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/search", NULL, NULL, NULL, &st);
    CHECK(st == 400); cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/search", "q=a&limit=999", NULL, NULL, &st);
    CHECK(st == 400); cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/search", "q=\"*)OR(", NULL, NULL, &st);   /* FTS injection attempt */
    CHECK(st == 200); cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/snap", "lat=17.0005&lon=79.0", NULL, NULL, &st);
    CHECK(st == 200 && cJSON_IsTrue(cJSON_GetObjectItem(j, "routable")));
    cJSON_Delete(j);
    j = call("GET", "/api/v1/locations/snap", "lat=abc&lon=79", NULL, NULL, &st);
    CHECK(st == 400); cJSON_Delete(j);
    j = call("GET", "/api/v1/traffic/segments", "bbox=78.9,16.8,79.3,17.1", NULL, NULL, &st);
    CHECK(st == 200); cJSON_Delete(j);
    j = call("GET", "/api/v1/traffic/segments", "bbox=1,2,3", NULL, NULL, &st);
    CHECK(st == 400); cJSON_Delete(j);
    j = call("GET", "/api/v1/traffic/segments", "bbox=70,10,80,20", NULL, NULL, &st);
    CHECK(st == 400 && strcmp(err_code(j), "BBOX_TOO_LARGE") == 0); cJSON_Delete(j);
}

static void test_admin_auth(void)
{
    int st;
    const char *body = "{\"latitude\":17.0,\"longitude\":79.03}";
    cJSON *j = call("POST", "/api/v1/admin/traffic/simulate", NULL, body, NULL, &st);
    CHECK(st == 401); cJSON_Delete(j);
    j = call("POST", "/api/v1/admin/traffic/simulate", NULL, body, "Bearer wrong-token-0123456789x", &st);
    CHECK(st == 401); cJSON_Delete(j);
    j = call("POST", "/api/v1/admin/traffic/simulate", NULL, body, "Basic abc", &st);
    CHECK(st == 401); cJSON_Delete(j);
    CHECK(api_token_equal("abc", "abc"));
    CHECK(!api_token_equal("abc", "abd"));
    CHECK(!api_token_equal("ab", "abc"));
    CHECK(!api_token_equal("abcd", "abc"));
}

static double fastest_distance(void)
{
    int st;
    cJSON *j = call("POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 "}", NULL, &st);
    double d = st == 200 ? num(j, "distance_meters") : -1;
    cJSON_Delete(j);
    return d;
}

static void test_admin_traffic_changes_route(void)
{
    int st;
    CHECK_NEAR(fastest_distance(), 10800, 1);
    /* edge index of E->F (external id 5) */
    edge_idx_t e5 = graph_find_edge(APP.g, 5);
    char body[256];
    snprintf(body, sizeof body, "{\"updates\":[{\"edge_index\":%u,\"travel_time_seconds\":1200},"
                                "{\"latitude\":10,\"longitude\":10,\"speed_kmh\":5}]}", e5);
    cJSON *j = call("POST", "/api/v1/admin/traffic/updates", NULL, body, ADMIN, &st);
    CHECK(st == 200 && num(j, "accepted") == 1 && num(j, "rejected") == 1);
    cJSON_Delete(j);
    CHECK_NEAR(fastest_distance(), 7043.5, 1);         /* detour now faster */
    j = call("POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 "}", NULL, &st);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "traffic_data"), "admin_overrides")));
    cJSON_Delete(j);
    j = call("DELETE", "/api/v1/admin/traffic", "source=admin", NULL, ADMIN, &st);
    CHECK(st == 200 && num(j, "removed") == 1);
    cJSON_Delete(j);
    CHECK_NEAR(fastest_distance(), 10800, 1);
}

static void test_incident_flow(void)
{
    int st;
    const char *closure = "{\"type\":\"closure\",\"latitude\":17.005,\"longitude\":79.025,\"description\":\"Bridge\\nwork\"}";
    cJSON *j = call("POST", "/api/v1/incidents", NULL, closure, NULL, &st);
    CHECK(st == 202);
    const cJSON *inc = cJSON_GetObjectItem(j, "incident");
    int id = (int)num(inc, "id");
    CHECK(strcmp(cJSON_GetObjectItem(inc, "status")->valuestring, "pending") == 0);
    CHECK(strchr(cJSON_GetObjectItem(inc, "description")->valuestring, '\n') == NULL);   /* sanitised */
    cJSON_Delete(j);

    j = call("GET", "/api/v1/incidents", NULL, NULL, NULL, &st);
    CHECK(st == 200 && num(j, "count") == 0);              /* pending: not public, no effect */
    cJSON_Delete(j);

    const char *avoid = ROUTE_1_TO_4 ",\"avoid\":{\"tolls\":true}}";
    j = call("POST", "/api/v1/routes", NULL, avoid, NULL, &st);
    CHECK(fabs(num(j, "distance_meters") - 7043.5) < 1);  /* still via B->H */
    cJSON_Delete(j);

    char path[96];
    snprintf(path, sizeof path, "/api/v1/admin/incidents/%d/verify", id);
    j = call("POST", path, NULL, NULL, ADMIN, &st);
    CHECK(st == 200);
    cJSON_Delete(j);
    j = call("GET", "/api/v1/incidents", NULL, NULL, NULL, &st);
    CHECK(num(j, "count") == 1);
    cJSON_Delete(j);
    j = call("POST", "/api/v1/routes", NULL, avoid, NULL, &st);
    CHECK(st == 200 && fabs(num(j, "distance_meters") - 6380.2) < 1);   /* closure avoided */
    CHECK(num(cJSON_GetObjectItem(j, "traffic_data"), "closed_segments") == 1);
    cJSON_Delete(j);

    snprintf(path, sizeof path, "/api/v1/admin/incidents/%d/reject", id);
    j = call("POST", path, NULL, NULL, ADMIN, &st);
    CHECK(st == 200); cJSON_Delete(j);
    j = call("POST", "/api/v1/admin/incidents/99999/verify", NULL, NULL, ADMIN, &st);
    CHECK(st == 404); cJSON_Delete(j);

    /* public users cannot self-verify; bad type; off-road */
    j = call("POST", "/api/v1/incidents", NULL, "{\"type\":\"closure\",\"latitude\":17.005,\"longitude\":79.025,\"verified\":true}", NULL, &st);
    CHECK(st == 403); cJSON_Delete(j);
    j = call("POST", "/api/v1/incidents", NULL, "{\"type\":\"meteor\",\"latitude\":17.005,\"longitude\":79.025}", NULL, &st);
    CHECK(st == 400); cJSON_Delete(j);
    j = call("POST", "/api/v1/incidents", NULL, "{\"type\":\"accident\",\"latitude\":16.95,\"longitude\":79.12}", NULL, &st);
    CHECK(st == 422); cJSON_Delete(j);
    j = call("GET", "/api/v1/admin/incidents", "status=all", NULL, ADMIN, &st);
    CHECK(st == 200 && num(j, "count") >= 1); cJSON_Delete(j);
}

static void test_simulation_labelled(void)
{
    int st;
    cJSON *j = call("POST", "/api/v1/admin/traffic/simulate", NULL,
                    "{\"latitude\":17.0,\"longitude\":79.03,\"radius_m\":5000,\"count\":5,\"seed\":3}", ADMIN, &st);
    CHECK(st == 200 && num(j, "simulated_segments") == 5);
    cJSON_Delete(j);
    j = call("POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 "}", NULL, &st);
    const cJSON *td = cJSON_GetObjectItem(j, "traffic_data");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(td, "simulated")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(td, "live")));
    CHECK(strstr(cJSON_GetObjectItem(td, "message")->valuestring, "SIMULATED") != NULL);
    cJSON_Delete(j);
    j = call("GET", "/api/v1/traffic/segments", "bbox=78.9,16.8,79.3,17.1", NULL, NULL, &st);
    CHECK(num(j, "count") == 5);
    const cJSON *seg = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "segments"), 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(seg, "simulated")));
    cJSON_Delete(j);
    j = call("DELETE", "/api/v1/admin/traffic", "source=simulated", NULL, ADMIN, &st);
    CHECK(st == 200 && num(j, "removed") == 5);
    cJSON_Delete(j);
}

static void test_rate_limiting(void)
{
    ira_config_t cfg;
    config_defaults(&cfg);
    cfg.rate_route_per_min = 4;        /* burst = 5 (minimum) */
    cfg.log_level = 3;
    app_t app;
    char err[256];
    CHECK(app_init(&app, &cfg, err, sizeof err));
    int st, ok = 0, limited = 0;
    for (int i = 0; i < 8; i++) {
        cJSON *j = call_ip(&app, "POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 "}", NULL, "10.0.0.9", &st);
        if (st == 200) ok++;
        if (st == 429) limited++;
        cJSON_Delete(j);
    }
    CHECK(ok == 5 && limited == 3);
    /* another client is unaffected */
    cJSON *j = call_ip(&app, "POST", "/api/v1/routes", NULL, ROUTE_1_TO_4 "}", NULL, "10.0.0.10", &st);
    CHECK(st == 200);
    cJSON_Delete(j);
    app_free(&app);
}

static void test_admin_disabled_without_token(void)
{
    ira_config_t cfg;
    config_defaults(&cfg);
    cfg.log_level = 3;
    snprintf(cfg.admin_token, sizeof cfg.admin_token, "short");   /* < 16 chars: disabled */
    app_t app;
    char err[256];
    CHECK(app_init(&app, &cfg, err, sizeof err));
    int st;
    cJSON *j = call_ip(&app, "DELETE", "/api/v1/admin/traffic", NULL, NULL, "Bearer short", "1.2.3.4", &st);
    CHECK(st == 403 && strcmp(err_code(j), "ADMIN_DISABLED") == 0);
    cJSON_Delete(j);
    app_free(&app);
}

int main(void)
{
    ira_config_t cfg;
    config_defaults(&cfg);
    cfg.log_level = 3;
    cfg.rate_route_per_min = 100000;
    cfg.rate_general_per_min = 100000;
    cfg.rate_incident_per_hour = 10000;
    snprintf(cfg.admin_token, sizeof cfg.admin_token, "test-admin-token-0123456789");
    char err[256];
    if (!app_init(&APP, &cfg, err, sizeof err)) { fprintf(stderr, "app_init: %s\n", err); return 1; }

    printf("api tests\n");
    RUN(test_health_and_config);
    RUN(test_route_success_schema);
    RUN(test_route_validation);
    RUN(test_endpoints_misc);
    RUN(test_admin_auth);
    RUN(test_admin_traffic_changes_route);
    RUN(test_incident_flow);
    RUN(test_simulation_labelled);
    RUN(test_rate_limiting);
    RUN(test_admin_disabled_without_token);
    app_free(&APP);
    TEST_SUMMARY();
}
