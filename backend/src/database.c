/*
 * database.c — SQLite implementation of the data-access layer.
 */
#include "database.h"

#include <sqlite3.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ira_db {
    sqlite3 *h;
};

/* Single source of truth for the schema. database/schema.sql is generated
 * from this string (`ira_import --print-schema`). */
static const char *SCHEMA_SQL =
    "PRAGMA foreign_keys = ON;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS import_metadata (\n"
    "  id               INTEGER PRIMARY KEY,\n"
    "  dataset_version  TEXT NOT NULL,\n"
    "  source           TEXT NOT NULL,\n"
    "  import_time      INTEGER NOT NULL,\n"
    "  node_count       INTEGER NOT NULL CHECK (node_count >= 0),\n"
    "  edge_count       INTEGER NOT NULL CHECK (edge_count >= 0),\n"
    "  import_status    TEXT NOT NULL CHECK (import_status IN ('ok','warnings','failed')),\n"
    "  report_json      TEXT\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS places (\n"
    "  place_id    INTEGER PRIMARY KEY,\n"
    "  osm_id      INTEGER,\n"
    "  name        TEXT NOT NULL,\n"
    "  name_en     TEXT,\n"
    "  type        TEXT NOT NULL,\n"
    "  type_rank   INTEGER NOT NULL DEFAULT 9,\n"
    "  state       TEXT,\n"
    "  district    TEXT,\n"
    "  near        TEXT,\n"
    "  latitude    REAL NOT NULL CHECK (latitude BETWEEN -90 AND 90),\n"
    "  longitude   REAL NOT NULL CHECK (longitude BETWEEN -180 AND 180),\n"
    "  population  INTEGER\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_places_type ON places(type_rank);\n"
    "CREATE VIRTUAL TABLE IF NOT EXISTS places_fts USING fts5(\n"
    "  name, name_en, state, district, content='places', content_rowid='place_id',\n"
    "  tokenize='unicode61 remove_diacritics 2'\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS road_incidents (\n"
    "  incident_id         INTEGER PRIMARY KEY,\n"
    "  incident_type       TEXT NOT NULL CHECK (incident_type IN\n"
    "                        ('accident','closure','congestion','roadwork','hazard')),\n"
    "  latitude            REAL NOT NULL CHECK (latitude BETWEEN -90 AND 90),\n"
    "  longitude           REAL NOT NULL CHECK (longitude BETWEEN -180 AND 180),\n"
    "  affected_edge_id    INTEGER,\n"
    "  both_directions     INTEGER NOT NULL DEFAULT 1,\n"
    "  dataset_version     TEXT,\n"
    "  reported_time       INTEGER NOT NULL,\n"
    "  expiry_time         INTEGER NOT NULL CHECK (expiry_time > reported_time),\n"
    "  verification_status TEXT NOT NULL CHECK (verification_status IN\n"
    "                        ('pending','verified','rejected','expired')),\n"
    "  simulated           INTEGER NOT NULL DEFAULT 0,\n"
    "  description         TEXT\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_incidents_expiry ON road_incidents(expiry_time);\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS traffic_observations (\n"
    "  observation_id         INTEGER PRIMARY KEY,\n"
    "  edge_id                INTEGER NOT NULL,\n"
    "  latitude               REAL,\n"
    "  longitude              REAL,\n"
    "  observed_time          INTEGER NOT NULL,\n"
    "  estimated_time_seconds REAL NOT NULL CHECK (estimated_time_seconds > 0),\n"
    "  source                 TEXT NOT NULL,\n"
    "  provider               TEXT,\n"
    "  confidence             REAL NOT NULL CHECK (confidence BETWEEN 0 AND 1),\n"
    "  expiry_time            INTEGER NOT NULL\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_obs_edge ON traffic_observations(edge_id, expiry_time);\n"
    "\n"
    "-- Optional export of the routing graph for inspection (importer --export-edges).\n"
    "-- The server routes from graph.bin, not from these tables.\n"
    "CREATE TABLE IF NOT EXISTS road_nodes (\n"
    "  node_id        INTEGER PRIMARY KEY,\n"
    "  latitude       REAL NOT NULL,\n"
    "  longitude      REAL NOT NULL,\n"
    "  source_node_id INTEGER\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS road_edges (\n"
    "  edge_id             INTEGER PRIMARY KEY,\n"
    "  source_node_id      INTEGER NOT NULL REFERENCES road_nodes(node_id),\n"
    "  target_node_id      INTEGER NOT NULL REFERENCES road_nodes(node_id),\n"
    "  distance_meters     REAL NOT NULL CHECK (distance_meters > 0),\n"
    "  baseline_time_seconds REAL NOT NULL CHECK (baseline_time_seconds > 0),\n"
    "  road_type           TEXT NOT NULL,\n"
    "  speed_limit         INTEGER,\n"
    "  access_flags        INTEGER NOT NULL DEFAULT 0,\n"
    "  one_way             INTEGER NOT NULL DEFAULT 0,\n"
    "  geometry_reference  INTEGER\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_edges_source ON road_edges(source_node_id);\n"
    "CREATE INDEX IF NOT EXISTS idx_edges_target ON road_edges(target_node_id);\n";

const char *db_schema_sql(void) { return SCHEMA_SQL; }

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

ira_db_t *db_open(const char *path, bool create, char *err, size_t err_len)
{
    ira_db_t *db = calloc(1, sizeof *db);
    if (!db) { set_err(err, err_len, "out of memory"); return NULL; }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path, &db->h, flags, NULL) != SQLITE_OK) {
        set_err(err, err_len, "cannot open database %s: %s", path,
                db->h ? sqlite3_errmsg(db->h) : "unknown");
        sqlite3_close(db->h);
        free(db);
        return NULL;
    }
    sqlite3_busy_timeout(db->h, 3000);
    sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    return db;
}

void db_close(ira_db_t *db)
{
    if (!db) return;
    sqlite3_close(db->h);
    free(db);
}

void *db_handle(ira_db_t *db) { return db ? db->h : NULL; }

bool db_init_schema(ira_db_t *db, char *err, size_t err_len)
{
    char *msg = NULL;
    if (sqlite3_exec(db->h, SCHEMA_SQL, NULL, NULL, &msg) != SQLITE_OK) {
        set_err(err, err_len, "schema error: %s", msg ? msg : "unknown");
        sqlite3_free(msg);
        return false;
    }
    return true;
}

static bool exec_simple(ira_db_t *db, const char *sql)
{
    return sqlite3_exec(db->h, sql, NULL, NULL, NULL) == SQLITE_OK;
}

bool db_begin(ira_db_t *db)    { return exec_simple(db, "BEGIN IMMEDIATE"); }
bool db_commit(ira_db_t *db)   { return exec_simple(db, "COMMIT"); }
bool db_rollback(ira_db_t *db) { return exec_simple(db, "ROLLBACK"); }

static void bind_text_or_null(sqlite3_stmt *s, int i, const char *v)
{
    if (v && *v) sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(s, i);
}

/* Runs a prepared statement once; returns true on SQLITE_DONE. */
static bool step_done(sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_insert_place(ira_db_t *db, const db_place_t *p)
{
    sqlite3_stmt *s;
    const char *sql =
        "INSERT INTO places(osm_id,name,name_en,type,type_rank,state,district,near,"
        "latitude,longitude,population) VALUES(?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, p->osm_id);
    sqlite3_bind_text(s, 2, p->name, -1, SQLITE_TRANSIENT);
    bind_text_or_null(s, 3, p->name_en);
    sqlite3_bind_text(s, 4, p->type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, p->type_rank);
    bind_text_or_null(s, 6, p->state);
    bind_text_or_null(s, 7, p->district);
    bind_text_or_null(s, 8, p->near);
    sqlite3_bind_double(s, 9, p->lat);
    sqlite3_bind_double(s, 10, p->lon);
    if (p->population > 0) sqlite3_bind_int64(s, 11, p->population);
    else sqlite3_bind_null(s, 11);
    return step_done(s);
}

bool db_clear_import_tables(ira_db_t *db)
{
    return exec_simple(db, "DELETE FROM places; DELETE FROM road_edges; DELETE FROM road_nodes;");
}

bool db_rebuild_place_index(ira_db_t *db)
{
    return exec_simple(db, "INSERT INTO places_fts(places_fts) VALUES('rebuild')");
}

int64_t db_place_count(ira_db_t *db)
{
    sqlite3_stmt *s;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db->h, "SELECT COUNT(*) FROM places", -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

bool db_load_places_csv(ira_db_t *db, const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    if (!f) { set_err(err, err_len, "cannot open %s: %s", path, strerror(errno)); return false; }
    char line[1024];
    bool header = false, ok = true;
    size_t lineno = 0;
    db_begin(db);
    while (fgets(line, sizeof line, f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;
        if (!header) { header = true; continue; }
        char *fld[6] = {0};
        int n = 0;
        char *p = line;
        while (n < 6) {
            fld[n++] = p;
            char *c = strchr(p, ',');
            if (!c) break;
            *c = '\0';
            p = c + 1;
        }
        char *end1, *end2;
        double lat = n >= 4 ? strtod(fld[2], &end1) : 0, lon = n >= 4 ? strtod(fld[3], &end2) : 0;
        if (n < 4 || *end1 || *end2 || !*fld[0]) {
            set_err(err, err_len, "%s:%zu: expected name,type,lat,lon[,state,district]", path, lineno);
            ok = false;
            break;
        }
        db_place_t pl = { .name = fld[0], .type = fld[1], .type_rank = 5,
                          .state = n > 4 ? fld[4] : NULL, .district = n > 5 ? fld[5] : NULL,
                          .lat = lat, .lon = lon };
        if (!db_insert_place(db, &pl)) { set_err(err, err_len, "insert failed"); ok = false; break; }
    }
    fclose(f);
    if (ok) { ok = db_commit(db) && db_rebuild_place_index(db); }
    else db_rollback(db);
    return ok;
}

bool db_insert_import_meta(ira_db_t *db, const db_import_meta_t *m)
{
    sqlite3_stmt *s;
    const char *sql = "INSERT INTO import_metadata(dataset_version,source,import_time,"
                      "node_count,edge_count,import_status,report_json) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, m->dataset_version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, m->source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, m->import_time);
    sqlite3_bind_int64(s, 4, m->node_count);
    sqlite3_bind_int64(s, 5, m->edge_count);
    sqlite3_bind_text(s, 6, m->import_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, m->report_json, -1, SQLITE_TRANSIENT);
    return step_done(s);
}

static void copy_col(char *dst, size_t n, sqlite3_stmt *s, int col)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    snprintf(dst, n, "%s", t ? (const char *)t : "");
}

bool db_latest_import_meta(ira_db_t *db, db_import_meta_t *m)
{
    sqlite3_stmt *s;
    memset(m, 0, sizeof *m);
    const char *sql = "SELECT dataset_version,source,import_time,node_count,edge_count,"
                      "import_status,report_json FROM import_metadata ORDER BY id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        copy_col(m->dataset_version, sizeof m->dataset_version, s, 0);
        copy_col(m->source, sizeof m->source, s, 1);
        m->import_time = sqlite3_column_int64(s, 2);
        m->node_count = sqlite3_column_int64(s, 3);
        m->edge_count = sqlite3_column_int64(s, 4);
        copy_col(m->import_status, sizeof m->import_status, s, 5);
        copy_col(m->report_json, sizeof m->report_json, s, 6);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

bool db_save_incident(ira_db_t *db, const incident_t *in, const char *dataset)
{
    sqlite3_stmt *s;
    const char *sql =
        "INSERT OR REPLACE INTO road_incidents(incident_id,incident_type,latitude,longitude,"
        "affected_edge_id,both_directions,dataset_version,reported_time,expiry_time,"
        "verification_status,simulated,description) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)in->id);
    sqlite3_bind_text(s, 2, incident_type_str(in->type), -1, SQLITE_STATIC);
    sqlite3_bind_double(s, 3, in->lat);
    sqlite3_bind_double(s, 4, in->lon);
    sqlite3_bind_int64(s, 5, in->edge);
    sqlite3_bind_int(s, 6, in->reverse_edge != IRA_INVALID_IDX);
    bind_text_or_null(s, 7, dataset);
    sqlite3_bind_int64(s, 8, in->reported_at);
    sqlite3_bind_int64(s, 9, in->expires_at);
    sqlite3_bind_text(s, 10, incident_status_str(in->status), -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 11, in->simulated);
    bind_text_or_null(s, 12, in->description);
    return step_done(s);
}

bool db_update_incident_status(ira_db_t *db, uint64_t id, incident_status_t st)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h,
            "UPDATE road_incidents SET verification_status=? WHERE incident_id=?",
            -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, incident_status_str(st), -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)id);
    return step_done(s);
}

uint64_t db_load_incidents(ira_db_t *db, int64_t now,
                           void (*fn)(void *ctx, const incident_t *in), void *ctx)
{
    uint64_t max_id = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h, "SELECT MAX(incident_id) FROM road_incidents", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) max_id = (uint64_t)sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    const char *sql =
        "SELECT incident_id,incident_type,latitude,longitude,both_directions,reported_time,"
        "expiry_time,verification_status,simulated,description FROM road_incidents "
        "WHERE expiry_time > ? AND verification_status IN ('pending','verified')";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return max_id;
    sqlite3_bind_int64(s, 1, now);
    while (sqlite3_step(s) == SQLITE_ROW) {
        incident_t in;
        memset(&in, 0, sizeof in);
        in.id = (uint64_t)sqlite3_column_int64(s, 0);
        const char *type = (const char *)sqlite3_column_text(s, 1);
        if (!incident_type_from_str(type, &in.type)) continue;
        in.lat = sqlite3_column_double(s, 2);
        in.lon = sqlite3_column_double(s, 3);
        /* Edge ids are not stable across graph versions: callers re-match by
         * coordinate. reverse_edge != INVALID signals "both directions". */
        in.edge = IRA_INVALID_IDX;
        in.reverse_edge = sqlite3_column_int(s, 4) ? 0 : IRA_INVALID_IDX;
        in.reported_at = sqlite3_column_int64(s, 5);
        in.expires_at = sqlite3_column_int64(s, 6);
        const char *st = (const char *)sqlite3_column_text(s, 7);
        in.status = (st && strcmp(st, "verified") == 0) ? INC_VERIFIED : INC_PENDING;
        in.simulated = sqlite3_column_int(s, 8) != 0;
        copy_col(in.description, sizeof in.description, s, 9);
        fn(ctx, &in);
    }
    sqlite3_finalize(s);
    return max_id;
}

bool db_log_observation(ira_db_t *db, const traffic_obs_t *o, double lat, double lon)
{
    sqlite3_stmt *s;
    const char *sql =
        "INSERT INTO traffic_observations(edge_id,latitude,longitude,observed_time,"
        "estimated_time_seconds,source,provider,confidence,expiry_time) VALUES(?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, o->edge);
    sqlite3_bind_double(s, 2, lat);
    sqlite3_bind_double(s, 3, lon);
    sqlite3_bind_int64(s, 4, o->observed_at);
    sqlite3_bind_double(s, 5, o->time_s);
    sqlite3_bind_text(s, 6, traffic_source_str(o->source), -1, SQLITE_STATIC);
    bind_text_or_null(s, 7, o->provider);
    sqlite3_bind_double(s, 8, o->confidence);
    sqlite3_bind_int64(s, 9, o->expires_at);
    return step_done(s);
}

bool db_insert_road_node(ira_db_t *db, int64_t node_id, double lat, double lon, int64_t source_id)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->h,
            "INSERT INTO road_nodes(node_id,latitude,longitude,source_node_id) VALUES(?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, node_id);
    sqlite3_bind_double(s, 2, lat);
    sqlite3_bind_double(s, 3, lon);
    sqlite3_bind_int64(s, 4, source_id);
    return step_done(s);
}

bool db_insert_road_edge(ira_db_t *db, int64_t edge_id, int64_t src, int64_t dst,
                         double dist, double time_s, const char *road_type, int speed,
                         int access_flags, int one_way, int64_t geometry_ref)
{
    sqlite3_stmt *s;
    const char *sql = "INSERT INTO road_edges(edge_id,source_node_id,target_node_id,"
                      "distance_meters,baseline_time_seconds,road_type,speed_limit,"
                      "access_flags,one_way,geometry_reference) VALUES(?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->h, sql, -1, &s, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, edge_id);
    sqlite3_bind_int64(s, 2, src);
    sqlite3_bind_int64(s, 3, dst);
    sqlite3_bind_double(s, 4, dist);
    sqlite3_bind_double(s, 5, time_s);
    sqlite3_bind_text(s, 6, road_type, -1, SQLITE_TRANSIENT);
    if (speed > 0) sqlite3_bind_int(s, 7, speed); else sqlite3_bind_null(s, 7);
    sqlite3_bind_int(s, 8, access_flags);
    sqlite3_bind_int(s, 9, one_way);
    sqlite3_bind_int64(s, 10, geometry_ref);
    return step_done(s);
}
