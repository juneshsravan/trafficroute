/*
 * app.h — the server's shared state and its lifecycle.
 *
 * Concurrency model:
 *   - graph topology, node data, spatial index, and places DB are read-only
 *     after start-up;
 *   - edge traffic times / closure flags and the traffic store change at run
 *     time, guarded by `lock` (pthread rwlock): route searches hold it for
 *     reading, traffic/incident updates for writing;
 *   - a fixed pool of search workspaces bounds concurrent searches and their
 *     memory (route_workers x ~37 bytes x node_count).
 */
#ifndef IRA_APP_H
#define IRA_APP_H

#include <pthread.h>
#include <stdatomic.h>

#include "config.h"
#include "database.h"
#include "graph.h"
#include "rate_limit.h"
#include "routing.h"
#include "spatial_index.h"
#include "traffic.h"

typedef struct tile_cache tile_cache_t;

#define IRA_VERSION "1.0.0"

typedef struct {
    ira_config_t       cfg;
    graph_t           *g;
    spatial_index_t   *si;
    ira_db_t          *db;
    traffic_state_t    traffic;
    pthread_rwlock_t   lock;
    bool               sample_mode;      /* synthetic CSV network loaded */
    bool               osm_data;         /* graph built from OpenStreetMap */
    double             main_component_share;

    route_workspace_t **ws;
    bool               *ws_busy;
    int                 ws_count;
    pthread_mutex_t     ws_mu;
    pthread_cond_t      ws_cv;

    rate_limiter_t     *rl;

    int64_t             started_at;
    atomic_long         requests;
    atomic_long         routes_ok;
    atomic_long         routes_failed;
    atomic_long         route_us_total;
    atomic_llong        last_expire_s;

    /* live provider status (guarded by `lock`) */
    pthread_t           provider_thread;
    bool                provider_running;
    atomic_bool         provider_stop;
    int64_t             provider_last_attempt;
    int64_t             provider_last_success;
    char                provider_last_error[256];
    tile_cache_t       *tile_cache;
} app_t;

bool app_init(app_t *app, const ira_config_t *cfg, char *err, size_t err_len);
void app_free(app_t *app);

/* Workspace pool: blocks up to timeout_ms; NULL if none became free. */
route_workspace_t *app_acquire_workspace(app_t *app, int timeout_ms);
void               app_release_workspace(app_t *app, route_workspace_t *ws);

/* Expires stale traffic at most once per second (takes the write lock). */
void app_maybe_expire(app_t *app, int64_t now);

/* Starts/stops the live traffic polling thread (no-op if no feed URL). */
bool app_start_provider(app_t *app);
void app_stop_provider(app_t *app);
/* One synchronous poll (used by the thread and by tests). */
bool app_poll_provider_once(app_t *app, int *retry_after_s);

int64_t app_now(void);
double  app_monotonic_s(void);

#endif
