/*
 * http_server.c — libmicrohttpd glue (see http_server.h).
 */
#define _POSIX_C_SOURCE 200809L
#include "http_server.h"
#include "api.h"
#include "log.h"
#include "tile_cache.h"

#include <microhttpd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_STATIC_BYTES (8u << 20)

struct http_server {
    struct MHD_Daemon *d;
    app_t             *app;
};

typedef struct {
    char  *buf;
    size_t len;
    bool   too_large;
    double started;
} conn_state_t;

const char *http_static_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    static const struct { const char *ext, *type; } types[] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".png", "image/png"},
        {".svg", "image/svg+xml"}, {".ico", "image/x-icon"}, {".json", "application/json"},
        {".woff2", "font/woff2"},
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcmp(dot, types[i].ext) == 0) return types[i].type;
    return NULL;
}

bool http_static_path_safe(const char *url)
{
    if (!url || url[0] != '/' || strlen(url) > 200) return false;
    if (strstr(url, "..") || strstr(url, "//") || strchr(url, '\\') || strchr(url, '%')) return false;
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(c == '/' || c == '.' || c == '-' || c == '_' || (c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    }
    /* no hidden files */
    if (strstr(url, "/.")) return false;
    return true;
}

static void add_common_headers(struct MHD_Response *r, const app_t *app, bool api)
{
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "X-Frame-Options", "DENY");
    if (api) {
        MHD_add_response_header(r, "Cache-Control", "no-store");
    } else {
        /* Frontend: scripts/styles only from this server; tiles from https. */
        MHD_add_response_header(r, "Content-Security-Policy",
            "default-src 'self'; img-src 'self' data: https:; script-src 'self'; "
            "style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
        MHD_add_response_header(r, "Cache-Control", "no-cache");
    }
    if (app->cfg.cors_origin[0] && api) {
        MHD_add_response_header(r, "Access-Control-Allow-Origin", app->cfg.cors_origin);
        MHD_add_response_header(r, "Vary", "Origin");
    }
}

static enum MHD_Result send_text(struct MHD_Connection *c, const app_t *app, unsigned status,
                                 const char *ctype, const char *body, bool api, int retry_after)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(strlen(body), (void *)body,
                                                             MHD_RESPMEM_MUST_COPY);
    if (!r) return MHD_NO;
    MHD_add_response_header(r, "Content-Type", ctype);
    add_common_headers(r, app, api);
    if (retry_after > 0) {
        char ra[16];
        snprintf(ra, sizeof ra, "%d", retry_after);
        MHD_add_response_header(r, "Retry-After", ra);
    }
    enum MHD_Result ret = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return ret;
}

static enum MHD_Result serve_static(struct MHD_Connection *c, const app_t *app, const char *url,
                                    bool head)
{
    const char *path = strcmp(url, "/") == 0 ? "/index.html" : url;
    const char *ctype = http_static_content_type(path);
    if (!http_static_path_safe(path) || !ctype)
        return send_text(c, app, 404, "text/plain; charset=utf-8", "Not found\n", false, 0);
    char full[1400];
    if (snprintf(full, sizeof full, "%s%s", app->cfg.frontend_dir, path) >= (int)sizeof full)
        return send_text(c, app, 404, "text/plain; charset=utf-8", "Not found\n", false, 0);
    int fd = open(full, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (off_t)MAX_STATIC_BYTES) {
        if (fd >= 0) close(fd);
        return send_text(c, app, 404, "text/plain; charset=utf-8", "Not found\n", false, 0);
    }
    struct MHD_Response *r = head ? MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT)
                                  : MHD_create_response_from_fd((uint64_t)st.st_size, fd);
    if (head) close(fd);
    if (!r) { if (!head) close(fd); return MHD_NO; }
    MHD_add_response_header(r, "Content-Type", ctype);
    add_common_headers(r, app, false);
    enum MHD_Result ret = MHD_queue_response(c, MHD_HTTP_OK, r);
    MHD_destroy_response(r);
    return ret;
}

static void client_ip(struct MHD_Connection *c, const app_t *app, char *buf, size_t n)
{
    snprintf(buf, n, "unknown");
    if (app->cfg.trust_proxy) {
        const char *xff = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-Forwarded-For");
        if (xff && *xff) {
            size_t len = strcspn(xff, ", ");
            if (len >= n) len = n - 1;
            memcpy(buf, xff, len);
            buf[len] = '\0';
            return;
        }
    }
    const union MHD_ConnectionInfo *ci = MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!ci || !ci->client_addr) return;
    const struct sockaddr *sa = ci->client_addr;
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)(const void *)sa)->sin_addr, buf, (socklen_t)n);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr, buf, (socklen_t)n);
}

static const char *get_arg(void *ctx, const char *key)
{
    return MHD_lookup_connection_value((struct MHD_Connection *)ctx, MHD_GET_ARGUMENT_KIND, key);
}

static enum MHD_Result handler(void *cls, struct MHD_Connection *c, const char *url,
                               const char *method, const char *version,
                               const char *upload_data, size_t *upload_size, void **con_cls)
{
    (void)version;
    http_server_t *s = cls;
    app_t *app = s->app;

    conn_state_t *st = *con_cls;
    if (!st) {                                 /* first call: headers only */
        st = calloc(1, sizeof *st);
        if (!st) return MHD_NO;
        st->started = app_monotonic_s();
        *con_cls = st;
        return MHD_YES;
    }
    if (*upload_size) {                        /* body chunk */
        if (st->len + *upload_size > API_MAX_BODY) {
            st->too_large = true;
        } else if (!st->too_large) {
            char *nb = realloc(st->buf, st->len + *upload_size + 1);
            if (!nb) return MHD_NO;
            st->buf = nb;
            memcpy(st->buf + st->len, upload_data, *upload_size);
            st->len += *upload_size;
            st->buf[st->len] = '\0';
        }
        *upload_size = 0;
        return MHD_YES;
    }

    bool is_api = strncmp(url, "/api/", 5) == 0;
    char ip[64];
    client_ip(c, app, ip, sizeof ip);
    enum MHD_Result ret;
    unsigned status;

    if (strcmp(method, "OPTIONS") == 0) {
        struct MHD_Response *r = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
        add_common_headers(r, app, true);
        if (app->cfg.cors_origin[0]) {
            MHD_add_response_header(r, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            MHD_add_response_header(r, "Access-Control-Allow-Headers", "Content-Type, Authorization");
            MHD_add_response_header(r, "Access-Control-Max-Age", "600");
        }
        ret = MHD_queue_response(c, MHD_HTTP_NO_CONTENT, r);
        MHD_destroy_response(r);
        status = 204;
    } else if (!is_api) {
        bool head = strcmp(method, "HEAD") == 0;
        if (strcmp(method, "GET") != 0 && !head) {
            ret = send_text(c, app, 405, "text/plain; charset=utf-8", "Method not allowed\n", false, 0);
            status = 405;
        } else if (app->tile_cache && strncmp(url, "/tiles/", 7) == 0) {
            ret = tile_cache_serve(app->tile_cache, c, url, head);
            status = 200;
        } else {
            ret = serve_static(c, app, url, head);
            status = 200;
        }
    } else if (st->too_large) {
        ret = send_text(c, app, 413, "application/json",
                        "{\"status\":\"error\",\"error\":{\"code\":\"BODY_TOO_LARGE\","
                        "\"message\":\"Request body exceeds 65536 bytes\"}}", true, 0);
        status = 413;
    } else {
        if (strcmp(method, "POST") == 0) {
            const char *ct = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Type");
            if (!ct || strncmp(ct, "application/json", 16) != 0) {
                ret = send_text(c, app, 415, "application/json",
                                "{\"status\":\"error\",\"error\":{\"code\":\"UNSUPPORTED_MEDIA_TYPE\","
                                "\"message\":\"Content-Type must be application/json\"}}", true, 0);
                LOGI("%s %s 415 %s", method, url, ip);
                return ret;
            }
        }
        api_request_t req = {
            .method = method, .path = url, .get_arg = get_arg, .arg_ctx = c,
            .body = st->buf, .body_len = st->len, .client_ip = ip,
            .authorization = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization"),
        };
        api_response_t resp;
        api_handle(app, &req, &resp);
        status = (unsigned)resp.status;
        ret = send_text(c, app, status, "application/json", resp.body ? resp.body : "{}", true,
                        resp.retry_after);
        api_response_free(&resp);
    }
    /* Access log: never includes headers, bodies or tokens. */
    if (is_api || status >= 400)
        LOGI("%s %s %u %.1fms %s", method, url, status,
             (app_monotonic_s() - st->started) * 1000.0, ip);
    return ret;
}

static void completed(void *cls, struct MHD_Connection *c, void **con_cls,
                      enum MHD_RequestTerminationCode toe)
{
    (void)cls; (void)c; (void)toe;
    conn_state_t *st = *con_cls;
    if (st) { free(st->buf); free(st); }
    *con_cls = NULL;
}

http_server_t *http_server_start(app_t *app, char *err, size_t err_len)
{
    http_server_t *s = calloc(1, sizeof *s);
    if (!s) { snprintf(err, err_len, "out of memory"); return NULL; }
    s->app = app;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)app->cfg.port);
    if (inet_pton(AF_INET, app->cfg.bind_addr, &addr.sin_addr) != 1) {
        snprintf(err, err_len, "IRA_BIND must be an IPv4 address (got '%s')", app->cfg.bind_addr);
        free(s);
        return NULL;
    }
    s->d = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
        (uint16_t)app->cfg.port, NULL, NULL, handler, s,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&addr,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)app->cfg.threads,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)15,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned int)512,
        MHD_OPTION_PER_IP_CONNECTION_LIMIT, (unsigned int)64,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)(128 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, completed, NULL,
        MHD_OPTION_END);
    if (!s->d) {
        snprintf(err, err_len, "could not start HTTP server on %s:%d (port in use?)",
                 app->cfg.bind_addr, app->cfg.port);
        free(s);
        return NULL;
    }
    return s;
}

void http_server_stop(http_server_t *s)
{
    if (!s) return;
    MHD_stop_daemon(s->d);
    free(s);
}
