/*
 * heap.h — indexed binary min-heap keyed by node index.
 *
 * Each node appears at most once. pos[node] stores where the node sits in the
 * heap array (or HEAP_NOT_IN), which makes decrease_key O(log n) instead of
 * the O(n) search a plain heap would need. This is what keeps Dijkstra at
 * O((V + E) log V).
 */
#ifndef IRA_HEAP_H
#define IRA_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HEAP_NOT_IN UINT32_MAX

typedef struct {
    uint32_t *items;     /* heap array of node indices */
    double   *keys;      /* key per node index (size = capacity) */
    uint32_t *pos;       /* position of node in items[], or HEAP_NOT_IN */
    size_t    size;
    size_t    capacity;  /* max node index + 1 */
} min_heap_t;

min_heap_t *heap_create(size_t capacity);
void        heap_free(min_heap_t *h);
void        heap_clear(min_heap_t *h);

static inline bool heap_empty(const min_heap_t *h) { return h->size == 0; }
static inline bool heap_contains(const min_heap_t *h, uint32_t node) {
    return node < h->capacity && h->pos[node] != HEAP_NOT_IN;
}

/* Inserts node with key, or lowers its key if already present and key is
 * smaller. Returns false on invalid node index. */
bool heap_push_or_decrease(min_heap_t *h, uint32_t node, double key);

/* Removes the minimum. Returns false if empty. */
bool heap_pop(min_heap_t *h, uint32_t *node, double *key);

#endif /* IRA_HEAP_H */
