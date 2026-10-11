/*
 * heap.c — indexed binary min-heap (see heap.h).
 */
#include "heap.h"

#include <stdlib.h>
#include <string.h>

min_heap_t *heap_create(size_t capacity)
{
    if (capacity == 0 || capacity >= HEAP_NOT_IN) return NULL;
    min_heap_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->items = malloc(capacity * sizeof *h->items);
    h->keys  = malloc(capacity * sizeof *h->keys);
    h->pos   = malloc(capacity * sizeof *h->pos);
    if (!h->items || !h->keys || !h->pos) {
        heap_free(h);
        return NULL;
    }
    h->capacity = capacity;
    memset(h->pos, 0xFF, capacity * sizeof *h->pos);   /* all HEAP_NOT_IN */
    return h;
}

void heap_free(min_heap_t *h)
{
    if (!h) return;
    free(h->items);
    free(h->keys);
    free(h->pos);
    free(h);
}

void heap_clear(min_heap_t *h)
{
    /* Only reset entries actually in the heap: O(size), not O(capacity). */
    for (size_t i = 0; i < h->size; i++) h->pos[h->items[i]] = HEAP_NOT_IN;
    h->size = 0;
}

static void swap_at(min_heap_t *h, size_t a, size_t b)
{
    uint32_t na = h->items[a], nb = h->items[b];
    h->items[a] = nb; h->pos[nb] = (uint32_t)a;
    h->items[b] = na; h->pos[na] = (uint32_t)b;
}

static void sift_up(min_heap_t *h, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->keys[h->items[parent]] <= h->keys[h->items[i]]) break;
        swap_at(h, i, parent);
        i = parent;
    }
}

static void sift_down(min_heap_t *h, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, smallest = i;
        if (l < h->size && h->keys[h->items[l]] < h->keys[h->items[smallest]])
            smallest = l;
        if (r < h->size && h->keys[h->items[r]] < h->keys[h->items[smallest]])
            smallest = r;
        if (smallest == i) return;
        swap_at(h, i, smallest);
        i = smallest;
    }
}

bool heap_push_or_decrease(min_heap_t *h, uint32_t node, double key)
{
    if (!h || node >= h->capacity) return false;
    uint32_t p = h->pos[node];
    if (p == HEAP_NOT_IN) {
        size_t i = h->size++;
        h->items[i] = node;
        h->pos[node] = (uint32_t)i;
        h->keys[node] = key;
        sift_up(h, i);
    } else if (key < h->keys[node]) {
        h->keys[node] = key;
        sift_up(h, p);
    }
    return true;
}

bool heap_pop(min_heap_t *h, uint32_t *node, double *key)
{
    if (!h || h->size == 0) return false;
    uint32_t top = h->items[0];
    if (node) *node = top;
    if (key)  *key = h->keys[top];

    h->size--;
    if (h->size > 0) {
        h->items[0] = h->items[h->size];
        h->pos[h->items[0]] = 0;
        sift_down(h, 0);
    }
    h->pos[top] = HEAP_NOT_IN;
    return true;
}
