/*
 * tile_cache.c — caching proxy for OpenStreetMap tiles.
 *
 * Downloads tiles from OpenStreetMap with a proper User-Agent
 * and caches them on disk for 7 days (604800 s).
 */
#define _POSIX_C_SOURCE 200809L
#include "tile_cache.h"
#include "log.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif

#define TILE_TTL_SEC (7 * 86400)
#define MAX_TILE_BYTES (1u << 20) /* 1 MB */

struct tile_cache {
    char cache_dir[512];
};

typedef struct {
    char  *buf;
    size_t len;
    size_t max;
} tile_buf_t;

static size_t tile_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    tile_buf_t *b = userdata;
    size_t bytes = size * nmemb;
    if (b->len + bytes > b->max) return 0;
    char *nb = realloc(b->buf, b->len + bytes);
    if (!nb) return 0;
    b->buf = nb;
    memcpy(b->buf + b->len, ptr, bytes);
    b->len += bytes;
    return bytes;
}

static void ensure_parent_dirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            mkdir_one(tmp);
            *p = sep;
        }
    }
}

tile_cache_t *tile_cache_create(const char *cache_dir)
{
    if (!cache_dir || !cache_dir[0]) return NULL;
    tile_cache_t *tc = calloc(1, sizeof *tc);
    if (!tc) return NULL;
    snprintf(tc->cache_dir, sizeof tc->cache_dir, "%s", cache_dir);
    ensure_parent_dirs(tc->cache_dir);
    mkdir_one(tc->cache_dir);
    return tc;
}

void tile_cache_free(tile_cache_t *tc)
{
    free(tc);
}

static enum MHD_Result send_response(struct MHD_Connection *c, unsigned status,
                                     const char *ctype, const void *data, size_t len,
                                     bool head)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(head ? 0 : len,
                                                             (void *)(head ? "" : data),
                                                             MHD_RESPMEM_MUST_COPY);
    if (!r) return MHD_NO;
    MHD_add_response_header(r, "Content-Type", ctype);
    MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(r, "Cache-Control", "public, max-age=604800");
    enum MHD_Result ret = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return ret;
}

enum MHD_Result tile_cache_serve(tile_cache_t *tc, struct MHD_Connection *c,
                                 const char *url, bool head)
{
    if (!tc) return MHD_NO;
    unsigned int z = 0, x = 0, y = 0;
    if (sscanf(url, "/tiles/%u/%u/%u.png", &z, &x, &y) != 3) {
        return send_response(c, 404, "text/plain", "Not Found\n", 10, head);
    }
    if (z > 19) {
        return send_response(c, 400, "text/plain", "Bad Request\n", 12, head);
    }

    char disk_path[600];
    snprintf(disk_path, sizeof disk_path, "%s/%u/%u/%u.png", tc->cache_dir, z, x, y);

    struct stat st;
    time_t now = time(NULL);
    bool cache_hit = false;
    if (stat(disk_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        if ((now - st.st_mtime) < TILE_TTL_SEC) {
            cache_hit = true;
        }
    }

    if (cache_hit) {
        FILE *f = fopen(disk_path, "rb");
        if (f) {
            char *buf = malloc((size_t)st.st_size);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)st.st_size, f);
                fclose(f);
                enum MHD_Result res = send_response(c, 200, "image/png", buf, rd, head);
                free(buf);
                return res;
            }
            fclose(f);
        }
    }

    /* Download tile from OpenStreetMap */
    char osm_url[256];
    snprintf(osm_url, sizeof osm_url, "https://tile.openstreetmap.org/%u/%u/%u.png", z, x, y);

    CURL *curl = curl_easy_init();
    if (!curl) return send_response(c, 502, "text/plain", "Gateway Error\n", 14, head);

    tile_buf_t tb = { .buf = NULL, .len = 0, .max = MAX_TILE_BYTES };
    curl_easy_setopt(curl, CURLOPT_URL, osm_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, tile_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &tb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "IndiaRouteAI/1.0 (college project; traffic-optimization)");

    CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OK && http_status == 200 && tb.buf && tb.len > 0) {
        ensure_parent_dirs(disk_path);
        FILE *wf = fopen(disk_path, "wb");
        if (wf) {
            fwrite(tb.buf, 1, tb.len, wf);
            fclose(wf);
        }
        enum MHD_Result res = send_response(c, 200, "image/png", tb.buf, tb.len, head);
        free(tb.buf);
        return res;
    }

    /* If download failed but an older cached tile exists, fallback to it */
    if (stat(disk_path, &st) == 0 && st.st_size > 0) {
        FILE *f = fopen(disk_path, "rb");
        if (f) {
            char *buf = malloc((size_t)st.st_size);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)st.st_size, f);
                fclose(f);
                free(tb.buf);
                enum MHD_Result res = send_response(c, 200, "image/png", buf, rd, head);
                free(buf);
                return res;
            }
            fclose(f);
        }
    }

    free(tb.buf);
    return send_response(c, (unsigned)(http_status >= 400 ? http_status : 502),
                         "text/plain", "Tile Unavailable\n", 17, head);
}
