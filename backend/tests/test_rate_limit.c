#include "rate_limit.h"
#include "test_util.h"

static void test_bucket(void)
{
    rate_limiter_t *rl = rl_create(4);
    rl_configure(rl, RL_ROUTE, 60, 60, 3);   /* 1 token/s, burst 3 */
    int ra = 0;
    CHECK(rl_allow(rl, "a", RL_ROUTE, 100.0, &ra));
    CHECK(rl_allow(rl, "a", RL_ROUTE, 100.0, &ra));
    CHECK(rl_allow(rl, "a", RL_ROUTE, 100.0, &ra));
    CHECK(!rl_allow(rl, "a", RL_ROUTE, 100.0, &ra));
    CHECK(ra == 1);
    CHECK(rl_allow(rl, "b", RL_ROUTE, 100.0, &ra));          /* independent client */
    CHECK(rl_allow(rl, "a", RL_ROUTE, 101.0, &ra));          /* refilled one token */
    CHECK(!rl_allow(rl, "a", RL_ROUTE, 101.0, &ra));
    CHECK(rl_allow(rl, "a", RL_GENERAL, 101.0, &ra));        /* other class unaffected */
    /* many distinct clients: table recycles slots without failing */
    char key[32];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof key, "client-%d", i);
        CHECK(rl_allow(rl, key, RL_ROUTE, 200.0 + i, &ra));
    }
    rl_free(rl);
}

int main(void)
{
    printf("rate limit tests\n");
    RUN(test_bucket);
    TEST_SUMMARY();
}
