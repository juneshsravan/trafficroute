#ifndef IRA_GEOSPATIAL_H
#define IRA_GEOSPATIAL_H

#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_M 6371008.8   /* mean Earth radius (IUGG) in metres */

/* Great-circle distance in metres between two WGS84 points (haversine). */
double geo_haversine_m(double lat1, double lon1, double lat2, double lon2);

/* True if lat in [-90,90], lon in [-180,180] and both finite. */
bool geo_valid_coord(double lat, double lon);

/* Rough India bounding box incl. islands (used for request validation). */
bool geo_in_india_bbox(double lat, double lon);

#endif
