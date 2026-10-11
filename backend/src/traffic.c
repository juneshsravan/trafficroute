/*
 * traffic.c — observation/incident store and weight application.
 */
#include "traffic.h"
#include "geospatial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *traffic_source_str(traffic_source_t s)
{
    switch (s) {
    case TSRC_LIVE:      return "live";
    case TSRC_ADMIN:     return "admin";
    case TSRC_SIMULATED: return "simulated";
    default:             return "unknown";
    }
}

static const char *INC_TYPES[INC_TYPE_COUNT] = {
    "accident", "closure", "congestion", "roadwork", "hazard",
};

const char *incident_type_str(incident_type_t t)
{
    return (unsigned)t < INC_TYPE_COUNT ? INC_TYPES[t] : "unknown";
}

bool incident_type_from_str(const char *s, incident_type_t *out)
{
    for (int i = 0; s && i < INC_TYPE_COUNT; i++)
        if (strcmp(s, INC_TYPES[i]) == 0) { *out = (incident_type_t)i; return true; }
    return false;
}

const char *incident_status_str(incident_status_t s)
{
    switch (s) {
    case INC_PENDING:  return "pending";
    case INC_VERIFIED: return "verified";
    case INC_REJECTED: return "rejected";
    case INC_EXPIRED:  return "expired";
    default:           return "unknown";
    }
}

void traffic_default_params(traffic_params_t *p)
{
    p->min_confidence = 0.5;
    p->accident_delay_factor = 2.0;
    p->roadwork_delay_factor = 1.5;
    p->live_stale_after_s = 900;
    p->max_observations = 200000;
    p->max_incidents = 20000;
}

bool traffic_init(traffic_state_t *t, graph_t *g, const traffic_params_t *p)
{
    memset(t, 0, sizeof *t);
    if (!g || !g->finalized) return false;
    t->g = g;
    if (p) t->p = *p; else traffic_default_params(&t->p);
    t->next_obs_id = 1;
    t->next_inc_id = 1;
    return true;
}

void traffic_free(traffic_state_t *t)
{
    if (!t) return;
    free(t->obs);
    free(t->inc);
    free(t->modified);
    memset(t, 0, sizeof *t);
}

static bool grow(void **buf, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    if (nc > SIZE_MAX / elem) return false;
    void *p = realloc(*buf, nc * elem);
    if (!p) return false;
    *buf = p;
    *cap = nc;
    return true;
}

static void mark_modified(traffic_state_t *t, edge_idx_t e)
{
    if (grow((void **)&t->modified, &t->modified_cap, t->modified_count + 1, sizeof *t->modified))
        t->modified[t->modified_count++] = e;
}

static int cmp_obs_time(const void *a, const void *b)
{
    const traffic_obs_t *x = a, *y = b;
    if (x->observed_at != y->observed_at) return x->observed_at < y->observed_at ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}

static bool incident_active(const incident_t *in)
{
    return in->status == INC_VERIFIED;
}

static void apply_incident_edge(traffic_state_t *t, const incident_t *in, edge_idx_t e)
{
    if (e == IRA_INVALID_IDX || e >= t->g->edge_count) return;
    graph_edge_t *ed = &t->g->edges[e];
    double factor = 1.0;
    switch (in->type) {
    case INC_CLOSURE:
        graph_set_closed(t->g, e, true);
        mark_modified(t, e);
        return;
    case INC_ACCIDENT: factor = t->p.accident_delay_factor; break;
    case INC_ROADWORK: factor = t->p.roadwork_delay_factor; break;
    default:           return;   /* congestion/hazard reports are informational */
    }
    if (factor > 1.0) {
        graph_set_traffic_time(t->g, e, graph_edge_time(ed) * factor);
        mark_modified(t, e);
    }
}

void traffic_reapply_all(traffic_state_t *t)
{
    /* 1. undo everything previously applied */
    for (size_t i = 0; i < t->modified_count; i++) {
        edge_idx_t e = t->modified[i];
        if (e < t->g->edge_count) {
            t->g->edges[e].traffic_time_s = 0.0f;
            graph_set_closed(t->g, e, false);
        }
    }
    t->modified_count = 0;

    /* 2. observations, oldest first so the newest wins */
    if (t->obs_count > 1) qsort(t->obs, t->obs_count, sizeof *t->obs, cmp_obs_time);
    for (size_t i = 0; i < t->obs_count; i++) {
        const traffic_obs_t *o = &t->obs[i];
        if (o->confidence < t->p.min_confidence) continue;
        graph_set_traffic_time(t->g, o->edge, o->time_s);
        mark_modified(t, o->edge);
    }

    /* 3. verified incidents on top */
    for (size_t i = 0; i < t->inc_count; i++) {
        const incident_t *in = &t->inc[i];
        if (!incident_active(in)) continue;
        apply_incident_edge(t, in, in->edge);
        apply_incident_edge(t, in, in->reverse_edge);
    }
}

int traffic_add_observation_noapply(traffic_state_t *t, edge_idx_t edge, double time_s,
                                    double confidence, traffic_source_t src,
                                    int64_t observed_at, int64_t expires_at,
                                    const char *provider, int64_t now, uint64_t *out_id)
{
    if (!t || edge >= t->g->edge_count || !isfinite(time_s) || time_s <= 0.0 ||
        time_s > 1.0e6 || !isfinite(confidence) || confidence < 0.0 || confidence > 1.0 ||
        (unsigned)src >= TSRC_COUNT)
        return -1;
    /* Reject data from the future (allowing 60 s clock skew), already-expired
     * data, and expiries more than 24 h ahead. */
    if (observed_at > now + 60 || expires_at <= now || expires_at > now + 86400)
        return -2;
    if (t->obs_count >= t->p.max_observations) return -3;
    if (!grow((void **)&t->obs, &t->obs_cap, t->obs_count + 1, sizeof *t->obs)) return -3;

    traffic_obs_t *o = &t->obs[t->obs_count++];
    memset(o, 0, sizeof *o);
    o->id = t->next_obs_id++;
    o->edge = edge;
    o->time_s = (float)time_s;
    o->confidence = (float)confidence;
    o->source = src;
    o->observed_at = observed_at;
    o->expires_at = expires_at;
    snprintf(o->provider, sizeof o->provider, "%s", provider ? provider : traffic_source_str(src));
    if (src == TSRC_LIVE && observed_at > t->last_live_update) t->last_live_update = observed_at;
    if (out_id) *out_id = o->id;
    return 0;
}

int traffic_add_observation(traffic_state_t *t, edge_idx_t edge, double time_s,
                            double confidence, traffic_source_t src,
                            int64_t observed_at, int64_t expires_at,
                            const char *provider, int64_t now, uint64_t *out_id)
{
    int rc = traffic_add_observation_noapply(t, edge, time_s, confidence, src,
                                             observed_at, expires_at, provider, now, out_id);
    if (rc == 0) traffic_reapply_all(t);
    return rc;
}

size_t traffic_clear_source(traffic_state_t *t, traffic_source_t src)
{
    size_t w = 0, removed = 0;
    for (size_t i = 0; i < t->obs_count; i++) {
        if (t->obs[i].source == src) { removed++; continue; }
        t->obs[w++] = t->obs[i];
    }
    t->obs_count = w;
    if (src == TSRC_SIMULATED) {
        size_t wi = 0;
        for (size_t i = 0; i < t->inc_count; i++) {
            if (t->inc[i].simulated) { removed++; continue; }
            t->inc[wi++] = t->inc[i];
        }
        t->inc_count = wi;
    }
    if (removed) traffic_reapply_all(t);
    return removed;
}

int traffic_add_incident(traffic_state_t *t, incident_type_t type, incident_status_t status,
                         double lat, double lon, edge_idx_t edge, bool both_directions,
                         int64_t reported_at, int64_t expires_at, bool simulated,
                         const char *description, uint64_t *out_id)
{
    if (!t || (unsigned)type >= INC_TYPE_COUNT || (unsigned)status >= INC_STATUS_COUNT ||
        !geo_valid_coord(lat, lon) || edge >= t->g->edge_count || expires_at <= reported_at)
        return -1;
    if (t->inc_count >= t->p.max_incidents) return -3;
    if (!grow((void **)&t->inc, &t->inc_cap, t->inc_count + 1, sizeof *t->inc)) return -3;

    incident_t *in = &t->inc[t->inc_count++];
    memset(in, 0, sizeof *in);
    in->id = t->next_inc_id++;
    in->type = type;
    in->status = status;
    in->lat = lat;
    in->lon = lon;
    in->edge = edge;
    in->reverse_edge = both_directions ? graph_find_reverse(t->g, edge) : IRA_INVALID_IDX;
    in->reported_at = reported_at;
    in->expires_at = expires_at;
    in->simulated = simulated;
    snprintf(in->description, sizeof in->description, "%s", description ? description : "");
    if (out_id) *out_id = in->id;
    if (status == INC_VERIFIED) traffic_reapply_all(t);
    return 0;
}

const incident_t *traffic_find_incident(const traffic_state_t *t, uint64_t id)
{
    for (size_t i = 0; i < t->inc_count; i++)
        if (t->inc[i].id == id) return &t->inc[i];
    return NULL;
}

int traffic_set_incident_status(traffic_state_t *t, uint64_t id, incident_status_t st)
{
    if ((unsigned)st >= INC_STATUS_COUNT) return -1;
    for (size_t i = 0; i < t->inc_count; i++) {
        if (t->inc[i].id != id) continue;
        if (t->inc[i].status == INC_EXPIRED) return -2;
        t->inc[i].status = st;
        traffic_reapply_all(t);
        return 0;
    }
    return -4;   /* not found */
}

size_t traffic_expire(traffic_state_t *t, int64_t now)
{
    size_t w = 0, expired = 0;
    for (size_t i = 0; i < t->obs_count; i++) {
        if (t->obs[i].expires_at <= now) { expired++; continue; }
        t->obs[w++] = t->obs[i];
    }
    t->obs_count = w;
    for (size_t i = 0; i < t->inc_count; i++) {
        incident_t *in = &t->inc[i];
        if (in->expires_at <= now && in->status != INC_EXPIRED && in->status != INC_REJECTED) {
            in->status = INC_EXPIRED;
            expired++;
        }
    }
    /* Drop long-dead incidents (expired/rejected > 24 h ago) from memory. */
    size_t wi = 0;
    for (size_t i = 0; i < t->inc_count; i++) {
        incident_t *in = &t->inc[i];
        if ((in->status == INC_EXPIRED || in->status == INC_REJECTED) &&
            in->expires_at + 86400 <= now) continue;
        t->inc[wi++] = *in;
    }
    t->inc_count = wi;
    if (expired) traffic_reapply_all(t);
    return expired;
}

void traffic_summary(const traffic_state_t *t, int64_t now, traffic_summary_t *s)
{
    memset(s, 0, sizeof *s);
    for (size_t i = 0; i < t->obs_count; i++)
        if (t->obs[i].expires_at > now) s->active_by_source[t->obs[i].source]++;
    for (size_t i = 0; i < t->inc_count; i++) {
        const incident_t *in = &t->inc[i];
        s->incidents_by_status[in->status]++;
        if (in->status == INC_VERIFIED &&
            (in->type == INC_ACCIDENT || in->type == INC_ROADWORK))
            s->incident_model_active = true;
    }
    for (size_t i = 0; i < t->modified_count; i++) {
        edge_idx_t e = t->modified[i];
        if (e < t->g->edge_count && (t->g->edges[e].flags & EDGE_FLAG_CLOSED)) s->closed_edges++;
    }
    s->live_available = s->active_by_source[TSRC_LIVE] > 0 &&
                        now - t->last_live_update <= t->p.live_stale_after_s;
    s->simulated_active = s->active_by_source[TSRC_SIMULATED] > 0;
    s->admin_active = s->active_by_source[TSRC_ADMIN] > 0;
}

/* ---- simulation ---- */

typedef struct {
    edge_idx_t *list;
    size_t n, cap;
} edge_list_t;

static bool collect_edge(void *ctx, edge_idx_t e)
{
    edge_list_t *l = ctx;
    if (l->n >= l->cap) return false;
    /* avoid duplicates from consecutive vertices of the same edge */
    if (l->n && l->list[l->n - 1] == e) return true;
    l->list[l->n++] = e;
    return true;
}

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

size_t traffic_simulate(traffic_state_t *t, const spatial_index_t *si,
                        double lat, double lon, double radius_m, size_t count,
                        uint32_t seed, int64_t now, int64_t duration_s)
{
    if (!t || !si || !geo_valid_coord(lat, lon) || !(radius_m > 0) || count == 0) return 0;
    double dlat = radius_m / 111195.0;
    double coslat = cos(lat * M_PI / 180.0);
    double dlon = radius_m / (111195.0 * (coslat > 0.05 ? coslat : 0.05));
    edge_list_t l = { .cap = 50000 };
    l.list = malloc(l.cap * sizeof *l.list);
    if (!l.list) return 0;
    spatial_edges_in_bbox(si, lat - dlat, lon - dlon, lat + dlat, lon + dlon, collect_edge, &l);

    size_t made = 0;
    uint32_t s = seed;
    for (size_t i = 0; i < count && l.n > 0; i++) {
        size_t k = xorshift(&s) % l.n;
        edge_idx_t e = l.list[k];
        l.list[k] = l.list[--l.n];        /* sample without replacement */
        double factor = 1.3 + 2.2 * (double)(xorshift(&s) % 1000) / 1000.0;
        double base = (double)t->g->edges[e].base_time_s;
        if (traffic_add_observation_noapply(t, e, base * factor, 1.0, TSRC_SIMULATED,
                                            now, now + duration_s, "simulator", now, NULL) == 0)
            made++;
    }
    free(l.list);
    if (made) traffic_reapply_all(t);
    return made;
}
