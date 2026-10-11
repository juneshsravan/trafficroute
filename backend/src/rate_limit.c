#include "rate_limit.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define KEY_LEN 64

typedef struct {
    char   key[KEY_LEN];
    double tokens[RL_CLASS_COUNT];
    double last[RL_CLASS_COUNT];
    double last_seen;
    bool   used;
} rl_slot_t;

struct rate_limiter {
    rl_slot_t      *slots;
    size_t          n;
    double          rate[RL_CLASS_COUNT];   /* tokens per second */
    double          burst[RL_CLASS_COUNT];
    pthread_mutex_t mu;
};

rate_limiter_t *rl_create(size_t slots)
{
    if (slots == 0) slots = 4096;
    rate_limiter_t *rl = calloc(1, sizeof *rl);
    if (!rl) return NULL;
    rl->slots = calloc(slots, sizeof *rl->slots);
    if (!rl->slots) { free(rl); return NULL; }
    rl->n = slots;
    for (int c = 0; c < RL_CLASS_COUNT; c++) { rl->rate[c] = 10; rl->burst[c] = 20; }
    pthread_mutex_init(&rl->mu, NULL);
    return rl;
}

void rl_free(rate_limiter_t *rl)
{
    if (!rl) return;
    pthread_mutex_destroy(&rl->mu);
    free(rl->slots);
    free(rl);
}

void rl_configure(rate_limiter_t *rl, rl_class_t cls, double per_period,
                  double period_s, double burst)
{
    if (!rl || (unsigned)cls >= RL_CLASS_COUNT || period_s <= 0) return;
    rl->rate[cls] = per_period / period_s;
    rl->burst[cls] = burst < 1 ? 1 : burst;
}

static uint64_t hash_key(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

bool rl_allow(rate_limiter_t *rl, const char *key, rl_class_t cls,
              double now_s, int *retry_after_s)
{
    if (!rl || !key || (unsigned)cls >= RL_CLASS_COUNT) return true;
    char k[KEY_LEN];
    size_t len = strlen(key);
    if (len >= KEY_LEN) len = KEY_LEN - 1;
    memcpy(k, key, len);
    k[len] = '\0';

    pthread_mutex_lock(&rl->mu);
    /* Linear probing within a small window; recycle the stalest slot. */
    size_t start = (size_t)(hash_key(k) % rl->n);
    rl_slot_t *slot = NULL, *oldest = NULL;
    for (size_t i = 0; i < 16 && i < rl->n; i++) {
        rl_slot_t *s = &rl->slots[(start + i) % rl->n];
        if (s->used && strcmp(s->key, k) == 0) { slot = s; break; }
        if (!s->used) { if (!oldest || oldest->used) oldest = s; continue; }
        if (!oldest || (oldest->used && s->last_seen < oldest->last_seen)) oldest = s;
    }
    if (!slot) {
        slot = oldest;
        memset(slot, 0, sizeof *slot);
        memcpy(slot->key, k, len + 1);
        slot->used = true;
        for (int c = 0; c < RL_CLASS_COUNT; c++) { slot->tokens[c] = rl->burst[c]; slot->last[c] = now_s; }
    }
    slot->last_seen = now_s;
    double t = slot->tokens[cls] + (now_s - slot->last[cls]) * rl->rate[cls];
    if (t > rl->burst[cls]) t = rl->burst[cls];
    slot->last[cls] = now_s;
    bool ok = t >= 1.0;
    if (ok) t -= 1.0;
    slot->tokens[cls] = t;
    if (!ok && retry_after_s) {
        double wait = rl->rate[cls] > 0 ? (1.0 - t) / rl->rate[cls] : 60.0;
        *retry_after_s = (int)ceil(wait);
        if (*retry_after_s < 1) *retry_after_s = 1;
    }
    pthread_mutex_unlock(&rl->mu);
    return ok;
}
