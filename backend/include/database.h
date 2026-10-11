/*
 * database.h — SQLite data-access layer.
 *
 * All SQL lives in database.c / location_search.c and every statement uses
 * parameter binding. The routing engine never touches the database: at run
 * time it reads graph.bin; SQLite holds the place-search index, import
 * metadata, and an audit trail of incidents and traffic observations.
 *
 * The connection is opened in SQLite's serialized (full-mutex) mode so the
 * HTTP worker threads can share it. Moving to PostgreSQL/PostGIS later means
 * re-implementing these functions with libpq; callers do not change.
 */
#ifndef IRA_DATABASE_H
#define IRA_DATABASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "traffic.h"

typedef struct ira_db ira_db_t;

/* path ":memory:" gives a private in-memory database. */
ira_db_t *db_open(const char *path, bool create, char *err, size_t err_len);
void      db_close(ira_db_t *db);
void     *db_handle(ira_db_t *db);            /* sqlite3* for location_search.c */

/* Creates all tables/indexes if missing. */
bool db_init_schema(ira_db_t *db, char *err, size_t err_len);
const char *db_schema_sql(void);

bool db_begin(ira_db_t *db);
bool db_commit(ira_db_t *db);
bool db_rollback(ira_db_t *db);

/* ---- places (written by the importer / CSV loader) ---- */
typedef struct {
    int64_t osm_id;
    const char *name;
    const char *name_en;
    const char *type;        /* city, town, village, ... */
    int type_rank;           /* lower = more important */
    const char *state;
    const char *district;
    const char *near;        /* nearest town/city for small places */
    double lat, lon;
    int64_t population;
} db_place_t;

bool db_insert_place(ira_db_t *db, const db_place_t *p);
/* Deletes places, road_nodes and road_edges (start of a fresh import). */
bool db_clear_import_tables(ira_db_t *db);
bool db_rebuild_place_index(ira_db_t *db);
int64_t db_place_count(ira_db_t *db);

/* Loads a small places CSV (name,type,lat,lon,state,district) — sample mode. */
bool db_load_places_csv(ira_db_t *db, const char *path, char *err, size_t err_len);

/* ---- import metadata ---- */
typedef struct {
    char    dataset_version[64];
    char    source[256];
    int64_t import_time;
    int64_t node_count;
    int64_t edge_count;
    char    import_status[32];
    char    report_json[4096];
} db_import_meta_t;

bool db_insert_import_meta(ira_db_t *db, const db_import_meta_t *m);
bool db_latest_import_meta(ira_db_t *db, db_import_meta_t *m);

/* ---- incidents & observations ---- */
bool db_save_incident(ira_db_t *db, const incident_t *in, const char *dataset);
bool db_update_incident_status(ira_db_t *db, uint64_t id, incident_status_t st);
/* Calls fn for every stored incident not yet expired at `now`. Returns the
 * highest incident id seen (0 if none). */
uint64_t db_load_incidents(ira_db_t *db, int64_t now,
                           void (*fn)(void *ctx, const incident_t *in), void *ctx);
bool db_log_observation(ira_db_t *db, const traffic_obs_t *o, double lat, double lon);

/* ---- optional road-network export (importer --export-edges) ---- */
bool db_insert_road_node(ira_db_t *db, int64_t node_id, double lat, double lon, int64_t source_id);
bool db_insert_road_edge(ira_db_t *db, int64_t edge_id, int64_t src, int64_t dst,
                         double dist, double time_s, const char *road_type, int speed,
                         int access_flags, int one_way, int64_t geometry_ref);

#endif
