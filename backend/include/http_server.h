/*
 * http_server.h — libmicrohttpd front end: request body accumulation with a
 * size cap, API dispatch, static frontend files, CORS and security headers.
 */
#ifndef IRA_HTTP_SERVER_H
#define IRA_HTTP_SERVER_H

#include "app.h"

typedef struct http_server http_server_t;

http_server_t *http_server_start(app_t *app, char *err, size_t err_len);
void           http_server_stop(http_server_t *s);

/* Exposed for tests: content type for a file extension, NULL if not served. */
const char *http_static_content_type(const char *path);
/* Exposed for tests: true if a URL path is safe to map into the frontend dir. */
bool http_static_path_safe(const char *url);

#endif
