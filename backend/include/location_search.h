/*
 * location_search.h — place search over the local SQLite FTS5 index that the
 * importer builds from OpenStreetMap place=* nodes (states, districts,
 * cities, towns, villages, hamlets, suburbs, localities).
 *
 * No external geocoding service is called, so there are no API keys,
 * quotas or usage-policy limits at query time.
 */
#ifndef IRA_LOCATION_SEARCH_H
#define IRA_LOCATION_SEARCH_H

#include "database.h"

#define LOCATION_MAX_RESULTS 25
#ifdef near
#undef near
#endif

typedef struct {
    int64_t id;
    int64_t osm_id;
    char    name[160];
    char    name_en[160];
    char    type[32];
    char    state[96];
    char    district[96];
    char    near[160];
    double  lat, lon;
    int64_t population;
} place_result_t;

/* Returns number of results written (0..max), or -1 on error.
 * The query is sanitised to letters/digits (any script) and spaces; each
 * word is matched as a prefix. */
int location_search(ira_db_t *db, const char *query, int limit, int offset,
                    place_result_t *out, int max);

/* Exposed for tests: builds the FTS5 MATCH expression. Returns false if the
 * query has no searchable characters. */
bool location_build_match(const char *query, char *buf, size_t n);

#endif
