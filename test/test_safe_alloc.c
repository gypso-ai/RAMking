/**
 * test_safe_alloc.c — Tests and usage demo for the safe_alloc interface.
 *
 * Build and run:
 *   make test
 *   ./test_safe_alloc
 *
 * Each test case prints PASS or FAIL.  A non-zero exit code indicates at
 * least one test failed.
 */

#include "safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_tests_run;                                                     \
        if (!(cond)) {                                                     \
            fprintf(stderr, "  FAIL  %s:%d  (%s)\n",                      \
                    __FILE__, __LINE__, #cond);                            \
            ++g_tests_failed;                                              \
        } else {                                                           \
            printf("  PASS  %s\n", #cond);                                \
        }                                                                  \
    } while (0)

static void begin_test(const char *name)
{
    printf("\n=== %s ===\n", name);
    safe_alloc_set_log_handler(NULL);
    (void)safe_alloc_set_allocators(NULL, NULL, NULL, NULL);
    (void)safe_alloc_set_record_buffer(NULL, 0);
    safe_alloc_reset();   /* fresh state for every test case */
}

/* -------------------------------------------------------------------------
 * Test cases
 * ---------------------------------------------------------------------- */

static void test_basic_malloc_free(void)
{
    begin_test("basic malloc / free");

    void *p = safe_malloc(64);
    CHECK(p != NULL);
    CHECK(safe_alloc_alive_count() == 1);
    CHECK(safe_alloc_total_allocs() == 1);

    safe_free(p);
    CHECK(safe_alloc_alive_count() == 0);
    CHECK(safe_alloc_total_frees() == 1);
}

static void test_calloc(void)
{
    begin_test("calloc zero-initialization");

    int *arr = (int *)safe_calloc(10, sizeof(int));
    CHECK(arr != NULL);
    CHECK(safe_alloc_alive_count() == 1);

    /* All bytes must be zero. */
    int all_zero = 1;
    int i;
    for (i = 0; i < 10; ++i) {
        if (arr[i] != 0) { all_zero = 0; break; }
    }
    CHECK(all_zero == 1);

    safe_free(arr);
    CHECK(safe_alloc_alive_count() == 0);
}

static void test_realloc_grow(void)
{
    begin_test("realloc grow");

    char *p = (char *)safe_malloc(8);
    CHECK(p != NULL);
    memcpy(p, "hello", 6);

    char *q = (char *)safe_realloc(p, 64);
    CHECK(q != NULL);
    CHECK(strcmp(q, "hello") == 0);
    CHECK(safe_alloc_alive_count() == 1);   /* still exactly one live alloc */
    CHECK(safe_alloc_total_allocs() >= 1);

    safe_free(q);
    CHECK(safe_alloc_alive_count() == 0);
}

static void test_realloc_null(void)
{
    begin_test("realloc(NULL) behaves like malloc");

    void *p = safe_realloc(NULL, 32);
    CHECK(p != NULL);
    CHECK(safe_alloc_alive_count() == 1);

    safe_free(p);
    CHECK(safe_alloc_alive_count() == 0);
}

static void test_realloc_zero_size(void)
{
    begin_test("realloc(ptr, 0) behaves like free");

    void *p = safe_malloc(32);
    CHECK(p != NULL);

    void *q = safe_realloc(p, 0);
    CHECK(q == NULL);
    CHECK(safe_alloc_alive_count() == 0);
}

static void test_free_null(void)
{
    begin_test("free(NULL) is a no-op");

    /* Must not crash or alter counters. */
    safe_free(NULL);
    CHECK(safe_alloc_alive_count() == 0);
    CHECK(safe_alloc_total_frees() == 0);
}

static void test_double_free_warning(void)
{
    begin_test("double-free warning (expect WARNING on stderr)");

    void *p = safe_malloc(16);
    CHECK(p != NULL);

    safe_free(p);
    CHECK(safe_alloc_alive_count() == 0);

    /* Second free should warn but NOT crash. */
    safe_free(p);
    /* alive count must remain 0 — the second free must be ignored. */
    CHECK(safe_alloc_alive_count() == 0);
    /* total_frees should only count the first real free. */
    CHECK(safe_alloc_total_frees() == 1);
}

static void test_free_unregistered(void)
{
    begin_test("free unregistered pointer warns (expect WARNING on stderr)");

    /* Allocate via plain malloc so it is NOT in the record table. */
    void *p = malloc(8);
    /* safe_free should warn but not crash and not double-free. */
    safe_free(p);

    /* Counters should be untouched. */
    CHECK(safe_alloc_alive_count() == 0);
    CHECK(safe_alloc_total_frees() == 0);

    /* Free via plain malloc so we don't leak in the test process. */
    free(p);
}

static void test_peak_count(void)
{
    begin_test("peak alive count tracking");

    void *a = safe_malloc(8);
    void *b = safe_malloc(8);
    void *c = safe_malloc(8);
    CHECK(safe_alloc_alive_count() == 3);
    CHECK(safe_alloc_peak_count()  == 3);

    safe_free(a);
    CHECK(safe_alloc_alive_count() == 2);
    CHECK(safe_alloc_peak_count()  == 3);   /* peak must not decrease */

    /* Allocate two more — alive reaches 4, new peak. */
    void *d = safe_malloc(8);
    void *e = safe_malloc(8);
    CHECK(safe_alloc_alive_count() == 4);
    CHECK(safe_alloc_peak_count()  == 4);

    safe_free(b);
    safe_free(c);
    safe_free(d);
    safe_free(e);
    CHECK(safe_alloc_alive_count() == 0);
    CHECK(safe_alloc_peak_count()  == 4);   /* peak persists */
}

static void test_list_alive(void)
{
    begin_test("dump_alive lists only non-freed records");

    void *p1 = safe_malloc(10);
    void *p2 = safe_malloc(20);
    void *p3 = safe_malloc(30);

    safe_free(p2);   /* p2 should NOT appear in alive dump */

    printf("  (dump_alive output follows on stderr)\n");
    safe_alloc_dump_alive();

    CHECK(safe_alloc_alive_count() == 2);

    /* Verify via get_records. */
    SafeAllocRecord recs[SAFE_ALLOC_MAX_RECORDS];
    unsigned int n = safe_alloc_get_records(recs, SAFE_ALLOC_MAX_RECORDS);
    /* Three records were written (one freed). */
    CHECK(n == 3);

    unsigned int alive = 0, freed = 0;
    unsigned int i;
    for (i = 0; i < n; ++i) {
        if (recs[i].freed) ++freed; else ++alive;
    }
    CHECK(alive == 2);
    CHECK(freed == 1);

    safe_free(p1);
    safe_free(p3);
}

static void test_total_stats(void)
{
    begin_test("total alloc / free counters");

    void *ptrs[5];
    int i;
    for (i = 0; i < 5; ++i) {
        ptrs[i] = safe_malloc(4);
    }
    CHECK(safe_alloc_total_allocs() == 5);
    CHECK(safe_alloc_alive_count()  == 5);

    for (i = 0; i < 3; ++i) {
        safe_free(ptrs[i]);
    }
    CHECK(safe_alloc_total_frees()  == 3);
    CHECK(safe_alloc_alive_count()  == 2);

    safe_free(ptrs[3]);
    safe_free(ptrs[4]);
    CHECK(safe_alloc_total_frees()  == 5);
    CHECK(safe_alloc_alive_count()  == 0);
}

static void test_dump_all(void)
{
    begin_test("dump_all shows full history");

    void *p = safe_malloc(100);
    safe_free(p);

    printf("  (dump_all output follows on stderr)\n");
    safe_alloc_dump_all();

    CHECK(safe_alloc_alive_count()   == 0);
    CHECK(safe_alloc_total_allocs()  == 1);
    CHECK(safe_alloc_total_frees()   == 1);
}

static unsigned int g_custom_malloc_calls  = 0;
static unsigned int g_custom_calloc_calls  = 0;
static unsigned int g_custom_realloc_calls = 0;
static unsigned int g_custom_free_calls    = 0;

static void *counting_malloc(size_t size)
{
    ++g_custom_malloc_calls;
    return malloc(size);
}

static void *counting_calloc(size_t nmemb, size_t size)
{
    ++g_custom_calloc_calls;
    return calloc(nmemb, size);
}

static void *counting_realloc(void *ptr, size_t size)
{
    ++g_custom_realloc_calls;
    return realloc(ptr, size);
}

static void counting_free(void *ptr)
{
    ++g_custom_free_calls;
    free(ptr);
}

static void test_custom_allocators(void)
{
    begin_test("custom allocator hooks");

    g_custom_malloc_calls = 0;
    g_custom_calloc_calls = 0;
    g_custom_realloc_calls = 0;
    g_custom_free_calls = 0;

    CHECK(safe_alloc_set_allocators(counting_malloc,
                                    counting_calloc,
                                    counting_realloc,
                                    counting_free) == 0);

    void *p = safe_malloc(24);
    CHECK(p != NULL);
    CHECK(g_custom_malloc_calls == 1);

    int *arr = (int *)safe_calloc(4, sizeof(int));
    CHECK(arr != NULL);
    CHECK(g_custom_calloc_calls == 1);

    p = safe_realloc(p, 48);
    CHECK(p != NULL);
    CHECK(g_custom_realloc_calls == 1);

    safe_free(p);
    safe_free(arr);
    CHECK(g_custom_free_calls == 2);
}

static void test_external_record_buffer(void)
{
    SafeAllocRecord external_records[4];
    const unsigned char dirty_pattern = 0xAB;

    begin_test("caller-provided record buffer");

    /* Pre-fill the caller buffer so the API must clear stale record contents. */
    memset(external_records, dirty_pattern, sizeof(external_records));
    CHECK(safe_alloc_set_record_buffer(external_records, 4) == 0);
    CHECK(external_records[0].ptr == NULL);

    void *p1 = safe_malloc(10);
    void *p2 = safe_malloc(20);
    CHECK(p1 != NULL);
    CHECK(p2 != NULL);
    CHECK(safe_alloc_alive_count() == 2);
    CHECK(external_records[0].ptr != NULL);
    CHECK(external_records[1].ptr != NULL);

    safe_free(p1);
    safe_free(p2);

    CHECK(safe_alloc_set_record_buffer(NULL, 0) == 0);
    CHECK(safe_alloc_alive_count() == 0);
}

static void test_reconfigure_while_alive_fails(void)
{
    begin_test("reconfigure while allocations are alive fails");

    SafeAllocRecord external_records[2];
    void *p = safe_malloc(8);

    CHECK(p != NULL);
    CHECK(safe_alloc_set_allocators(counting_malloc,
                                    counting_calloc,
                                    counting_realloc,
                                    counting_free) == -1);
    CHECK(safe_alloc_set_record_buffer(external_records, 2) == -1);
    CHECK(safe_alloc_alive_count() == 1);

    safe_free(p);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("RAMking — safe_alloc test suite\n");
    printf("================================\n");

    test_basic_malloc_free();
    test_calloc();
    test_realloc_grow();
    test_realloc_null();
    test_realloc_zero_size();
    test_free_null();
    test_double_free_warning();
    test_free_unregistered();
    test_peak_count();
    test_list_alive();
    test_total_stats();
    test_dump_all();
    test_custom_allocators();
    test_external_record_buffer();
    test_reconfigure_while_alive_fails();

    printf("\n================================\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed == 0) {
        printf("  -- ALL PASS\n");
    } else {
        printf("  -- %d FAILED\n", g_tests_failed);
    }

    return g_tests_failed == 0 ? 0 : 1;
}
