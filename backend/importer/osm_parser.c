#include "osm_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *osm_tag(const readosm_tag *tags, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (tags[i].key && strcmp(tags[i].key, key) == 0) return tags[i].value;
    return NULL;
}

const char *way_reason_str(way_reason_t r)
{
    static const char *s[] = {"ok", "not_highway", "not_routable_highway_type",
                              "no_vehicle_access", "area", "track_excluded"};
    return (unsigned)r < WAY_REASON_COUNT ? s[r] : "unknown";
}

static bool eq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

double osm_parse_maxspeed(const char *v)
{
    if (!v || !*v) return 0;
    char *end;
    double x = strtod(v, &end);
    if (end == v || x <= 0) return 0;          /* "IN:urban", "none", "signals" */
    while (*end == ' ') end++;
    if (strncmp(end, "mph", 3) == 0) x *= 1.609344;
    else if (*end && strncmp(end, "km/h", 4) != 0 && strncmp(end, "kmh", 3) != 0 &&
             *end != ';') return 0;
    if (x < 5 || x > 150) return 0;            /* implausible values ignored */
    return x;
}

typedef struct { const char *tag; road_class_t cls; bool link; } hw_map_t;

static const hw_map_t HW[] = {
    {"motorway", ROAD_MOTORWAY, false},   {"motorway_link", ROAD_MOTORWAY, true},
    {"trunk", ROAD_TRUNK, false},         {"trunk_link", ROAD_TRUNK, true},
    {"primary", ROAD_PRIMARY, false},     {"primary_link", ROAD_PRIMARY, true},
    {"secondary", ROAD_SECONDARY, false}, {"secondary_link", ROAD_SECONDARY, true},
    {"tertiary", ROAD_TERTIARY, false},   {"tertiary_link", ROAD_TERTIARY, true},
    {"unclassified", ROAD_UNCLASSIFIED, false}, {"road", ROAD_UNCLASSIFIED, false},
    {"residential", ROAD_RESIDENTIAL, false},   {"living_street", ROAD_RESIDENTIAL, false},
    {"service", ROAD_SERVICE, false},     {"track", ROAD_TRACK, false},
};

/* Most specific access tag wins. */
static const char *vehicle_access(const readosm_tag *t, int n)
{
    const char *keys[] = {"motorcar", "motor_vehicle", "vehicle", "access"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = osm_tag(t, n, keys[i]);
        if (v) return v;
    }
    return NULL;
}

way_reason_t osm_classify_way(const readosm_tag *t, int n, const osm_rules_t *rules,
                              way_attrs_t *out)
{
    memset(out, 0, sizeof *out);
    const char *hw = osm_tag(t, n, "highway");
    if (!hw) return WAY_NOT_HIGHWAY;
    const hw_map_t *m = NULL;
    for (size_t i = 0; i < sizeof HW / sizeof HW[0]; i++)
        if (strcmp(hw, HW[i].tag) == 0) { m = &HW[i]; break; }
    if (!m) return WAY_NOT_ROUTABLE_TYPE;
    if (m->cls == ROAD_TRACK && rules && !rules->include_tracks) return WAY_TRACK_EXCLUDED;
    if (eq(osm_tag(t, n, "area"), "yes")) return WAY_AREA;

    const char *acc = vehicle_access(t, n);
    if (eq(acc, "no") || eq(acc, "private") || eq(acc, "use_sidepath")) return WAY_NO_ACCESS;
    if (eq(acc, "destination") || eq(acc, "delivery") || eq(acc, "customers") ||
        eq(acc, "permit") || eq(acc, "agricultural") || eq(acc, "forestry"))
        out->restricted = true;
    /* A service road tagged as private parking aisle etc. is already dropped
     * above; plain service roads stay routable. */

    out->cls = m->cls;
    out->is_link = m->link;

    const char *ow = osm_tag(t, n, "oneway");
    const char *junction = osm_tag(t, n, "junction");
    if (eq(ow, "yes") || eq(ow, "true") || eq(ow, "1")) out->oneway = 1;
    else if (eq(ow, "-1") || eq(ow, "reverse")) out->oneway = -1;
    else if (eq(ow, "no") || eq(ow, "false") || eq(ow, "0")) out->oneway = 0;
    else if (eq(hw, "motorway") || eq(hw, "motorway_link") ||
             eq(junction, "roundabout") || eq(junction, "circular")) out->oneway = 1;

    out->toll = eq(osm_tag(t, n, "toll"), "yes");
    out->maxspeed_kmh = osm_parse_maxspeed(osm_tag(t, n, "maxspeed"));

    /* Travel speed: class default (links slower), capped by the legal limit;
     * a tagged limit can raise the estimate by at most 25 % over the class
     * default, because Indian traffic rarely reaches posted limits. */
    double def = road_class_default_speed_kmh(m->cls) * (m->link ? 0.75 : 1.0);
    double sp = def;
    if (out->maxspeed_kmh > 0) {
        sp = out->maxspeed_kmh < def * 1.25 ? out->maxspeed_kmh : def * 1.25;
        if (out->maxspeed_kmh < sp) sp = out->maxspeed_kmh;
    }
    out->speed_kmh = sp;

    const char *name = osm_tag(t, n, "name:en");
    if (!name) name = osm_tag(t, n, "name");
    const char *ref = osm_tag(t, n, "ref");
    if (name && ref) snprintf(out->name, sizeof out->name, "%s (%s)", name, ref);
    else if (name) snprintf(out->name, sizeof out->name, "%s", name);
    else if (ref) snprintf(out->name, sizeof out->name, "%s", ref);
    return WAY_OK;
}

int osm_place_rank(const char *v)
{
    static const struct { const char *t; int r; } P[] = {
        {"city", 1}, {"state", 2}, {"district", 2}, {"town", 3}, {"suburb", 4},
        {"village", 5}, {"neighbourhood", 6}, {"quarter", 6}, {"locality", 6},
        {"hamlet", 7}, {"isolated_dwelling", 8},
    };
    if (!v) return -1;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (strcmp(v, P[i].t) == 0) return P[i].r;
    return -1;
}
