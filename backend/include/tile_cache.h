/*
 * tile_cache.h — caching proxy for OpenStreetMap tiles.
 */
#ifndef IRA_TILE_CACHE_H
#define IRA_TILE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <microhttpd.h>

typedef struct tile_cache tile_cache_t;

tile_cache_t *tile_cache_create(const char *cache_dir);
void tile_cache_free(tile_cache_t *tc);

enum MHD_Result tile_cache_serve(tile_cache_t *tc, struct MHD_Connection *c,
                                 const char *url, bool head);

#endif
