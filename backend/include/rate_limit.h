/*
 * rate_limit.h — per-client token buckets (fixed-size table, thread-safe).
 * Each client key (IP address) gets one bucket per class. Buckets refill
 * continuously at `per_period / period_s` tokens per second up to `burst`.
 * When the table is full, the least recently used slot is recycled, so
 * memory stays bounded however many clients connect.
 */
#ifndef IRA_RATE_LIMIT_H
#define IRA_RATE_LIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RL_GENERAL = 0, RL_ROUTE, RL_INCIDENT, RL_CLASS_COUNT } rl_class_t;

typedef struct rate_limiter rate_limiter_t;

rate_limiter_t *rl_create(size_t slots);
void            rl_free(rate_limiter_t *rl);
/* per_period tokens every period_s seconds, burst = max bucket size. */
void            rl_configure(rate_limiter_t *rl, rl_class_t cls, double per_period,
                             double period_s, double burst);
/* Takes one token; returns false if the client is over its limit.
 * now_s is a monotonic clock in seconds. retry_after_s gets a hint. */
bool            rl_allow(rate_limiter_t *rl, const char *key, rl_class_t cls,
                         double now_s, int *retry_after_s);

#endif
