/*
 * traffic.h — traffic observations and road incidents, and how they change
 * edge weights.
 *
 * The routing algorithms never look at this module: it only writes each
 * edge's traffic_time_s and CLOSED flag, and later searches pick them up.
 *
 * Effective weight of an edge (recomputed by traffic_reapply_all):
 *   1. time = newest unexpired observation with confidence >= min_confidence,
 *      otherwise the baseline estimate;
 *   2. verified ACCIDENT / ROADWORK incidents multiply that time by a
 *      configured delay factor (an explicitly documented MODEL, reported as
 *      "estimated", never as measured traffic);
 *   3. verified CLOSURE incidents set the CLOSED flag (route avoids the edge).
 * Pending (unverified) or rejected incidents never affect routing.
 *
 * Every observation carries its source so the API can always say whether
 * travel times are live, admin-entered, or simulated.
 *
 * Not thread-safe: the server serialises calls with a write lock.
 */
#ifndef IRA_TRAFFIC_H
#define IRA_TRAFFIC_H

#include "graph.h"
#include "spatial_index.h"

typedef enum {
    TSRC_LIVE = 0,      /* external provider feed */
    TSRC_ADMIN,         /* operator entered via admin API */
    TSRC_SIMULATED,     /* demo data — must always be labelled as such */
    TSRC_COUNT
} traffic_source_t;

typedef enum {
    INC_ACCIDENT = 0,
    INC_CLOSURE,
    INC_CONGESTION,
    INC_ROADWORK,
    INC_HAZARD,
    INC_TYPE_COUNT
} incident_type_t;

typedef enum {
    INC_PENDING = 0,
    INC_VERIFIED,
    INC_REJECTED,
    INC_EXPIRED,
    INC_STATUS_COUNT
} incident_status_t;

typedef struct {
    uint64_t         id;
    edge_idx_t       edge;
    float            time_s;
    float            confidence;      /* 0..1 */
    traffic_source_t source;
    int64_t          observed_at;
    int64_t          expires_at;
    char             provider[48];
} traffic_obs_t;

typedef struct {
    uint64_t          id;
    incident_type_t   type;
    incident_status_t status;
    double            lat, lon;
    edge_idx_t        edge;
    edge_idx_t        reverse_edge;   /* IRA_INVALID_IDX if none / one direction */
    int64_t           reported_at;
    int64_t           expires_at;
    bool              simulated;
    char              description[256];
} incident_t;

typedef struct {
    double min_confidence;          /* default 0.5 */
    double accident_delay_factor;   /* default 2.0 (model) */
    double roadwork_delay_factor;   /* default 1.5 (model) */
    int64_t live_stale_after_s;     /* live data older than this = not live (900) */
    size_t max_observations;        /* default 200000 */
    size_t max_incidents;           /* default 20000 */
} traffic_params_t;

typedef struct {
    graph_t        *g;
    traffic_params_t p;
    traffic_obs_t  *obs;
    size_t          obs_count, obs_cap;
    incident_t     *inc;
    size_t          inc_count, inc_cap;
    edge_idx_t     *modified;       /* edges whose weight/flag we changed */
    size_t          modified_count, modified_cap;
    uint64_t        next_obs_id, next_inc_id;
    int64_t         last_live_update;
    char            live_provider[64];
    char            live_attribution[160];
} traffic_state_t;

typedef struct {
    size_t active_by_source[TSRC_COUNT];
    size_t incidents_by_status[INC_STATUS_COUNT];
    size_t closed_edges;
    bool   live_available;          /* live data present and fresh */
    bool   simulated_active;
    bool   admin_active;
    bool   incident_model_active;   /* delay factors from verified incidents */
} traffic_summary_t;

void traffic_default_params(traffic_params_t *p);
bool traffic_init(traffic_state_t *t, graph_t *g, const traffic_params_t *p);
void traffic_free(traffic_state_t *t);

/* Adds one observation and re-applies weights. Returns 0 or a negative error
 * (-1 invalid value, -2 stale/future timestamp, -3 capacity). */
int traffic_add_observation(traffic_state_t *t, edge_idx_t edge, double time_s,
                            double confidence, traffic_source_t src,
                            int64_t observed_at, int64_t expires_at,
                            const char *provider, int64_t now, uint64_t *out_id);

/* Batch form without re-apply; call traffic_reapply_all() afterwards. */
int traffic_add_observation_noapply(traffic_state_t *t, edge_idx_t edge, double time_s,
                                    double confidence, traffic_source_t src,
                                    int64_t observed_at, int64_t expires_at,
                                    const char *provider, int64_t now, uint64_t *out_id);

size_t traffic_clear_source(traffic_state_t *t, traffic_source_t src);

/* Incidents. edge must already be matched (spatial_nearest_edge). If
 * both_directions, the reverse twin is affected too. */
int traffic_add_incident(traffic_state_t *t, incident_type_t type, incident_status_t status,
                         double lat, double lon, edge_idx_t edge, bool both_directions,
                         int64_t reported_at, int64_t expires_at, bool simulated,
                         const char *description, uint64_t *out_id);
int traffic_set_incident_status(traffic_state_t *t, uint64_t id, incident_status_t st);
const incident_t *traffic_find_incident(const traffic_state_t *t, uint64_t id);

/* Drops expired observations, marks expired incidents, re-applies weights if
 * anything changed. Returns the number of items expired. */
size_t traffic_expire(traffic_state_t *t, int64_t now);

/* Clears every change this module made and applies all active data again. */
void traffic_reapply_all(traffic_state_t *t);

void traffic_summary(const traffic_state_t *t, int64_t now, traffic_summary_t *s);

/* Generates clearly labelled simulated congestion on up to `count` random
 * edges within radius_m of a point. Returns number of observations created. */
size_t traffic_simulate(traffic_state_t *t, const spatial_index_t *si,
                        double lat, double lon, double radius_m, size_t count,
                        uint32_t seed, int64_t now, int64_t duration_s);

const char *traffic_source_str(traffic_source_t s);
const char *incident_type_str(incident_type_t t);
const char *incident_status_str(incident_status_t s);
bool incident_type_from_str(const char *s, incident_type_t *out);

#endif
