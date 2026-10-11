/*
 * osm_parser.h — interpretation of OpenStreetMap tags for vehicle routing.
 *
 * Pure functions over tag arrays so the rules are unit-testable without
 * reading any file. Rules (documented in docs/data_pipeline.md):
 *   - highway=motorway..tertiary(+_link), unclassified, residential,
 *     living_street, service, road, track(optional) are routable;
 *     footway/path/cycleway/steps/pedestrian/construction/proposed/... are not;
 *   - access precedence motorcar > motor_vehicle > vehicle > access:
 *     no/private  -> way dropped;  destination/delivery/customers/permit/
 *     agricultural/forestry -> routable but flagged RESTRICTED;
 *   - area=yes dropped; oneway=yes/true/1 forward, -1/reverse backward;
 *     motorway, motorway_link and junction=roundabout/circular imply oneway;
 *   - toll=yes -> TOLL flag; maxspeed parsed as km/h or "N mph".
 */
#ifndef IRA_OSM_PARSER_H
#define IRA_OSM_PARSER_H

#include <readosm.h>
#include <stdbool.h>
#include <stdint.h>

#include "graph.h"

typedef enum {
    WAY_OK = 0,
    WAY_NOT_HIGHWAY,
    WAY_NOT_ROUTABLE_TYPE,
    WAY_NO_ACCESS,
    WAY_AREA,
    WAY_TRACK_EXCLUDED,
    WAY_REASON_COUNT
} way_reason_t;

typedef struct {
    road_class_t cls;
    bool         is_link;
    int          oneway;          /* 0 both, 1 forward, -1 backward */
    bool         toll;
    bool         restricted;
    double       maxspeed_kmh;    /* 0 = not tagged / unparseable */
    double       speed_kmh;       /* speed used for travel time */
    char         name[160];       /* "Name (Ref)" or ref or name */
} way_attrs_t;

typedef struct {
    bool include_tracks;          /* default true: many Indian village roads are tracks */
} osm_rules_t;

const char *osm_tag(const readosm_tag *tags, int n, const char *key);
way_reason_t osm_classify_way(const readosm_tag *tags, int n, const osm_rules_t *rules,
                              way_attrs_t *out);
const char *way_reason_str(way_reason_t r);

/* Parses "50", "50 km/h", "30 mph" -> km/h; returns 0 for none/unknown. */
double osm_parse_maxspeed(const char *v);

/* Place nodes: returns rank (lower = more important) or -1 if not a place
 * we index. type_out receives the place type. */
int osm_place_rank(const char *place_value);

#endif
