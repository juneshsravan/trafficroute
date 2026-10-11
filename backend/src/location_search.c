/*
 * location_search.c — FTS5 place search with safe query construction.
 */
#include "location_search.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define MAX_TOKENS 8

/* Turns user text into  "tok1"* "tok2"*  using only alphanumerics and UTF-8
 * multibyte characters (Devanagari, Tamil, ... names). Quotes and FTS5
 * operators can therefore never reach the MATCH expression. */
bool location_build_match(const char *q, char *buf, size_t n)
{
    if (!q || !buf || n < 8) return false;
    size_t k = 0;
    int tokens = 0;
    const unsigned char *p = (const unsigned char *)q;
    buf[0] = '\0';
    while (*p && tokens < MAX_TOKENS) {
        while (*p && !(*p >= 0x80 || (*p >= '0' && *p <= '9') ||
                       ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')))
            p++;
        if (!*p) break;
        if (k + 4 >= n) break;
        if (tokens) buf[k++] = ' ';
        buf[k++] = '"';
        size_t start = k;
        while (*p && (*p >= 0x80 || (*p >= '0' && *p <= '9') ||
                      ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z'))) {
            if (k + 3 >= n || k - start >= 64) { p++; continue; }
            buf[k++] = (char)*p++;
        }
        buf[k++] = '"';
        buf[k++] = '*';
        buf[k] = '\0';
        tokens++;
    }
    return tokens > 0;
}

static void col(char *dst, size_t n, sqlite3_stmt *s, int c)
{
    const unsigned char *t = sqlite3_column_text(s, c);
    snprintf(dst, n, "%s", t ? (const char *)t : "");
}

int location_search(ira_db_t *db, const char *query, int limit, int offset,
                    place_result_t *out, int max)
{
    sqlite3 *h = db_handle(db);
    if (!h || !out || max <= 0) return -1;
    if (limit <= 0 || limit > LOCATION_MAX_RESULTS) limit = 10;
    if (limit > max) limit = max;
    if (offset < 0 || offset > 1000) offset = 0;

    char match[512];
    if (!location_build_match(query, match, sizeof match)) return 0;

    /* Exact name matches first, then more important place types (city before
     * village), then FTS relevance, then population. */
    const char *sql =
        "SELECT p.place_id, p.osm_id, p.name, p.name_en, p.type, p.state, p.district, "
        "       p.near, p.latitude, p.longitude, p.population "
        "FROM places_fts f JOIN places p ON p.place_id = f.rowid "
        "WHERE places_fts MATCH ?1 "
        "ORDER BY (lower(p.name) = lower(?2) OR lower(coalesce(p.name_en,'')) = lower(?2)) DESC, "
        "         p.type_rank ASC, bm25(places_fts) ASC, coalesce(p.population,0) DESC "
        "LIMIT ?3 OFFSET ?4";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(h, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, match, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, limit);
    sqlite3_bind_int(s, 4, offset);

    int n = 0, rc;
    while (n < limit && (rc = sqlite3_step(s)) == SQLITE_ROW) {
        place_result_t *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->id = sqlite3_column_int64(s, 0);
        r->osm_id = sqlite3_column_int64(s, 1);
        col(r->name, sizeof r->name, s, 2);
        col(r->name_en, sizeof r->name_en, s, 3);
        col(r->type, sizeof r->type, s, 4);
        col(r->state, sizeof r->state, s, 5);
        col(r->district, sizeof r->district, s, 6);
        col(r->near, sizeof r->near, s, 7);
        r->lat = sqlite3_column_double(s, 8);
        r->lon = sqlite3_column_double(s, 9);
        r->population = sqlite3_column_int64(s, 10);
    }
    sqlite3_finalize(s);
    return n;
}
