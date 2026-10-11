/*
 * route_geometry.c — geometry assembly and basic instructions.
 */
#include "route_geometry.h"
#include "geospatial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *instruction_type_str(instruction_type_t t)
{
    static const char *names[] = {
        "depart", "continue", "slight_left", "slight_right", "left", "right",
        "sharp_left", "sharp_right", "uturn", "arrive",
    };
    return (unsigned)t <= INSTR_ARRIVE ? names[t] : "unknown";
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / M_PI;
    return fmod(b + 360.0, 360.0);
}

static const char *compass(double b)
{
    static const char *dirs[] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};
    int i = (int)floor((b + 22.5) / 45.0) % 8;
    return dirs[i < 0 ? 0 : i];
}

static void road_label(const graph_t *g, const graph_edge_t *e, char *buf, size_t n)
{
    const char *name = graph_edge_name(g, e);
    if (name[0]) snprintf(buf, n, "%s", name);
    else snprintf(buf, n, "unnamed %s road", road_class_name((road_class_t)e->road_class));
}

/* Two consecutive edges belong to the same instruction "leg" when they carry
 * the same road name (or are both unnamed roads of the same class). */
static bool same_road(const graph_edge_t *a, const graph_edge_t *b)
{
    if (a->name_id || b->name_id) return a->name_id == b->name_id;
    return a->road_class == b->road_class;
}

static instruction_type_t classify_turn(double delta)
{
    double a = fabs(delta);
    if (a < 25)  return INSTR_CONTINUE;
    if (a < 60)  return delta > 0 ? INSTR_SLIGHT_RIGHT : INSTR_SLIGHT_LEFT;
    if (a < 135) return delta > 0 ? INSTR_RIGHT : INSTR_LEFT;
    if (a < 170) return delta > 0 ? INSTR_SHARP_RIGHT : INSTR_SHARP_LEFT;
    return INSTR_UTURN;
}

static const char *turn_phrase(instruction_type_t t)
{
    switch (t) {
    case INSTR_SLIGHT_LEFT:  return "Bear left";
    case INSTR_SLIGHT_RIGHT: return "Bear right";
    case INSTR_LEFT:         return "Turn left";
    case INSTR_RIGHT:        return "Turn right";
    case INSTR_SHARP_LEFT:   return "Turn sharp left";
    case INSTR_SHARP_RIGHT:  return "Turn sharp right";
    case INSTR_UTURN:        return "Make a U-turn";
    default:                 return "Continue";
    }
}

void route_geometry_free(route_geometry_t *geom)
{
    if (!geom) return;
    free(geom->points);
    free(geom->instructions);
    memset(geom, 0, sizeof *geom);
}

bool route_geometry_build(const graph_t *g, const route_result_t *r, route_geometry_t *out)
{
    memset(out, 0, sizeof *out);
    if (!g || !r || r->node_count == 0) return false;

    /* ---- points ---- */
    size_t npts = 1;
    for (size_t i = 0; i < r->edge_count; i++) npts += g->edges[r->edges[i]].geom_count + 1;
    out->points = malloc(npts * sizeof *out->points);
    size_t *edge_start = malloc((r->edge_count + 1) * sizeof *edge_start);
    out->instructions = malloc((r->edge_count + 2) * sizeof *out->instructions);
    if (!out->points || !edge_start || !out->instructions) {
        free(edge_start);
        route_geometry_free(out);
        return false;
    }

    size_t k = 0;
    const graph_node_t *n0 = &g->nodes[r->nodes[0]];
    out->points[k++] = (geo_pt_t){ n0->lat, n0->lon };
    for (size_t i = 0; i < r->edge_count; i++) {
        const graph_edge_t *e = &g->edges[r->edges[i]];
        edge_start[i] = k - 1;
        for (uint32_t s = 0; s < e->geom_count; s++) {
            const graph_point_t *p = &g->shape[e->geom_offset + s];
            out->points[k++] = (geo_pt_t){ p->lat, p->lon };
        }
        const graph_node_t *nt = &g->nodes[e->to];
        out->points[k++] = (geo_pt_t){ nt->lat, nt->lon };
    }
    edge_start[r->edge_count] = k - 1;
    out->point_count = k;

    /* ---- instructions ---- */
    size_t ni = 0;
    char road[160];
    if (r->edge_count == 0) {
        route_instruction_t *a = &out->instructions[ni++];
        memset(a, 0, sizeof *a);
        a->type = INSTR_ARRIVE;
        snprintf(a->text, sizeof a->text, "You are already at the destination");
        a->lat = n0->lat; a->lon = n0->lon;
        out->instruction_count = ni;
        free(edge_start);
        return true;
    }

    for (size_t i = 0; i < r->edge_count; i++) {
        const graph_edge_t *e = &g->edges[r->edges[i]];
        bool starts_leg = (i == 0) || !same_road(&g->edges[r->edges[i - 1]], e);
        if (!starts_leg) {
            out->instructions[ni - 1].distance_m += (double)e->distance_m;
            out->instructions[ni - 1].duration_s += graph_edge_time(e);
            continue;
        }
        route_instruction_t *in = &out->instructions[ni++];
        memset(in, 0, sizeof *in);
        road_label(g, e, road, sizeof road);
        snprintf(in->road, sizeof in->road, "%s", road);
        size_t gi = edge_start[i];
        in->geometry_index = gi;
        in->lat = out->points[gi].lat;
        in->lon = out->points[gi].lon;
        in->distance_m = (double)e->distance_m;
        in->duration_s = graph_edge_time(e);

        double out_b = geo_bearing_deg(out->points[gi].lat, out->points[gi].lon,
                                       out->points[gi + 1].lat, out->points[gi + 1].lon);
        if (i == 0) {
            in->type = INSTR_DEPART;
            snprintf(in->text, sizeof in->text, "Head %s on %s", compass(out_b), road);
        } else {
            double in_b = geo_bearing_deg(out->points[gi - 1].lat, out->points[gi - 1].lon,
                                          out->points[gi].lat, out->points[gi].lon);
            double delta = fmod(out_b - in_b + 540.0, 360.0) - 180.0;  /* (-180,180] */
            in->type = classify_turn(delta);
            if (in->type == INSTR_CONTINUE)
                snprintf(in->text, sizeof in->text, "Continue onto %s", road);
            else
                snprintf(in->text, sizeof in->text, "%s onto %s", turn_phrase(in->type), road);
        }
    }
    route_instruction_t *a = &out->instructions[ni++];
    memset(a, 0, sizeof *a);
    a->type = INSTR_ARRIVE;
    snprintf(a->text, sizeof a->text, "Arrive at your destination");
    a->geometry_index = out->point_count - 1;
    a->lat = out->points[out->point_count - 1].lat;
    a->lon = out->points[out->point_count - 1].lon;
    out->instruction_count = ni;
    free(edge_start);
    return true;
}

/* Google polyline algorithm: zig-zag encode deltas, 5-bit chunks + 63. */
static size_t encode_value(long v, char *dst)
{
    unsigned long u = v < 0 ? ~((unsigned long)v << 1) : ((unsigned long)v << 1);
    size_t n = 0;
    while (u >= 0x20) {
        dst[n++] = (char)((0x20 | (u & 0x1f)) + 63);
        u >>= 5;
    }
    dst[n++] = (char)(u + 63);
    return n;
}

char *polyline_encode(const geo_pt_t *pts, size_t n)
{
    if (n > (SIZE_MAX - 1) / 24) return NULL;
    char *buf = malloc(n * 24 + 1);   /* <= 12 chars per coordinate */
    if (!buf) return NULL;
    size_t k = 0;
    long plat = 0, plon = 0;
    for (size_t i = 0; i < n; i++) {
        long la = lround(pts[i].lat * 1e5), lo = lround(pts[i].lon * 1e5);
        k += encode_value(la - plat, buf + k);
        k += encode_value(lo - plon, buf + k);
        plat = la; plon = lo;
    }
    buf[k] = '\0';
    return buf;
}
