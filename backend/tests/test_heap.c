#include "heap.h"
#include "test_util.h"

static void test_pop_order(void)
{
    min_heap_t *h = heap_create(10);
    double keys[10] = {5, 3, 8, 1, 9, 2, 7, 0, 6, 4};
    for (uint32_t i = 0; i < 10; i++) CHECK(heap_push_or_decrease(h, i, keys[i]));
    double prev = -1;
    uint32_t n; double k; int count = 0;
    while (heap_pop(h, &n, &k)) {
        CHECK(k >= prev);
        CHECK(k == keys[n]);
        prev = k;
        count++;
    }
    CHECK(count == 10);
    CHECK(heap_empty(h));
    heap_free(h);
}

static void test_decrease_key(void)
{
    min_heap_t *h = heap_create(4);
    heap_push_or_decrease(h, 0, 10);
    heap_push_or_decrease(h, 1, 20);
    heap_push_or_decrease(h, 2, 30);
    heap_push_or_decrease(h, 2, 5);    /* decrease: now the minimum */
    heap_push_or_decrease(h, 0, 50);   /* larger key: ignored */
    CHECK(h->size == 3);               /* no duplicates */
    uint32_t n; double k;
    heap_pop(h, &n, &k); CHECK(n == 2 && k == 5);
    heap_pop(h, &n, &k); CHECK(n == 0 && k == 10);
    heap_pop(h, &n, &k); CHECK(n == 1 && k == 20);
    CHECK(!heap_pop(h, &n, &k));
    heap_free(h);
}

static void test_bounds_and_clear(void)
{
    CHECK(heap_create(0) == NULL);
    min_heap_t *h = heap_create(3);
    CHECK(!heap_push_or_decrease(h, 3, 1.0));   /* out of range rejected */
    heap_push_or_decrease(h, 1, 1.0);
    CHECK(heap_contains(h, 1));
    heap_clear(h);
    CHECK(heap_empty(h) && !heap_contains(h, 1));
    heap_free(h);
}

static void test_random_against_sort(void)
{
    enum { N = 2000 };
    min_heap_t *h = heap_create(N);
    srand(42);
    for (uint32_t i = 0; i < N; i++) {
        heap_push_or_decrease(h, i, (double)(rand() % 100000));
        if (i % 3 == 0) heap_push_or_decrease(h, i / 2, (double)(rand() % 100));
    }
    double prev = -1; uint32_t n; double k; int bad = 0, count = 0;
    while (heap_pop(h, &n, &k)) { if (k < prev) bad++; prev = k; count++; }
    CHECK(bad == 0);
    CHECK(count == N);
    heap_free(h);
}

int main(void)
{
    printf("heap tests\n");
    RUN(test_pop_order);
    RUN(test_decrease_key);
    RUN(test_bounds_and_clear);
    RUN(test_random_against_sort);
    TEST_SUMMARY();
}
