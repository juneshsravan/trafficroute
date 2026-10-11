/*
 * route_geometry.h — turn a routed edge sequence into map geometry and basic
 * turn-by-turn instructions.
 *
 * Instructions are derived only from road names, road classes and the change
 * in bearing at junctions. They do NOT know about lanes, turn restrictions or
 * signage, so they are "basic" instructions whose quality depends on how
 * completely the source data names its roads.
 */
#ifndef IRA_ROUTE_GEOMETRY_H
#define IRA_ROUTE_GEOMETRY_H

#include "routing.h"

typedef struct { double lat, lon; } geo_pt_t;

typedef enum {
    INSTR_DEPART = 0,
    INSTR_CONTINUE,
    INSTR_SLIGHT_LEFT,
    INSTR_SLIGHT_RIGHT,
    INSTR_LEFT,
    INSTR_RIGHT,
    INSTR_SHARP_LEFT,
    INSTR_SHARP_RIGHT,
    INSTR_UTURN,
    INSTR_ARRIVE
} instruction_type_t;

typedef struct {
    instruction_type_t type;
    char   text[256];
    char   road[160];
    double distance_m;       /* distance until the next instruction */
    double duration_s;
    double lat, lon;         /* where the manoeuvre happens */
    size_t geometry_index;   /* index into points[] */
} route_instruction_t;

typedef struct {
    geo_pt_t            *points;
    size_t               point_count;
    route_instruction_t *instructions;
    size_t               instruction_count;
} route_geometry_t;

bool route_geometry_build(const graph_t *g, const route_result_t *r, route_geometry_t *out);
void route_geometry_free(route_geometry_t *geom);

const char *instruction_type_str(instruction_type_t t);

/* Initial bearing from A to B in degrees [0, 360). */
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/* Google encoded polyline (precision 1e5). Caller frees. */
char *polyline_encode(const geo_pt_t *pts, size_t n);

#endif
