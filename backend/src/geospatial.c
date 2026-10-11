#include "geospatial.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double deg2rad(double d) { return d * (M_PI / 180.0); }

double geo_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double phi1 = deg2rad(lat1), phi2 = deg2rad(lat2);
    double dphi = deg2rad(lat2 - lat1), dlmb = deg2rad(lon2 - lon1);
    double a = sin(dphi / 2) * sin(dphi / 2) +
               cos(phi1) * cos(phi2) * sin(dlmb / 2) * sin(dlmb / 2);
    if (a > 1.0) a = 1.0;              /* guard rounding before sqrt/asin */
    return 2.0 * EARTH_RADIUS_M * asin(sqrt(a));
}

bool geo_valid_coord(double lat, double lon)
{
    return isfinite(lat) && isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

bool geo_in_india_bbox(double lat, double lon)
{
    /* Generous box: Indira Point (~6.7N) to Ladakh (~37.1N),
     * Gujarat (~68.1E) to Arunachal (~97.4E). Coarse pre-check only. */
    return geo_valid_coord(lat, lon) &&
           lat >= 6.0 && lat <= 37.6 && lon >= 68.0 && lon <= 97.6;
}
