/*
 * api.h — REST API request handling, independent of the HTTP library.
 *
 * http_server.c turns a libmicrohttpd request into an api_request_t and
 * sends back the api_response_t. Tests call api_handle() directly, so every
 * endpoint, validation rule and error code is testable without sockets.
 */
#ifndef IRA_API_H
#define IRA_API_H

#include "app.h"

#define API_MAX_BODY (64 * 1024)

typedef const char *(*api_arg_fn)(void *ctx, const char *key);

typedef struct {
    const char *method;          /* "GET", "POST", ... */
    const char *path;            /* "/api/v1/routes" (no query string) */
    api_arg_fn  get_arg;         /* query-string lookup, may be NULL */
    void       *arg_ctx;
    const char *body;            /* request body (may be NULL) */
    size_t      body_len;
    const char *client_ip;       /* for rate limiting */
    const char *authorization;   /* raw Authorization header or NULL */
} api_request_t;

typedef struct {
    int   status;                /* HTTP status */
    char *body;                  /* malloc'd JSON text */
    int   retry_after;           /* seconds, 0 = none */
} api_response_t;

void api_handle(app_t *app, const api_request_t *req, api_response_t *resp);
void api_response_free(api_response_t *resp);

/* Constant-time comparison for the admin bearer token. */
bool api_token_equal(const char *a, const char *b);

#endif
