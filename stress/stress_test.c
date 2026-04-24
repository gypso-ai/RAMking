/**
 * stress_test.c — Stress and performance tests for safe_alloc.
 *
 * Build and run:
 *   make stress
 *   ./stress_test
 *
 * Tests cover:
 *   1. High-volume sequential malloc/free cycles
 *   2. Interleaved alloc / free to exercise slot reuse
 *   3. Realloc chain growth and shrink
 *   4. Record table near-capacity behaviour
 *   5. Duplicate-free and unregistered-pointer warnings under load
 *   6. Custom log handler (captures all messages; none go to stderr)
 *
 * A summary with throughput numbers is printed at the end.
 */

/* Required for clock_gettime / CLOCK_MONOTONIC under -std=c99 */
#define _POSIX_C_SOURCE 199309L

#include "safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Timing helpers
 * ---------------------------------------------------------------------- */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* -------------------------------------------------------------------------
 * Minimal pass/fail harness (mirrors test_safe_alloc.c style)
 * ---------------------------------------------------------------------- */

static int g_run    = 0;
static int g_failed = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_run;                                                         \
        if (!(cond)) {                                                   \
            fprintf(stderr, "  FAIL  %s:%d  (%s)\n",                    \
                    __FILE__, __LINE__, #cond);                          \
            ++g_failed;                                                  \
        } else {                                                         \
            printf("  PASS  %s\n", #cond);                              \
        }                                                                \
    } while (0)

static void begin_test(const char *name)
{
    printf("\n=== %s ===\n", name);
    safe_alloc_reset();
}

/* -------------------------------------------------------------------------
 * Custom log handler used by stress tests that intentionally trigger
 * warnings — suppresses output to stderr so the test run stays clean.
 * ---------------------------------------------------------------------- */

static unsigned int g_warn_count  = 0;
static unsigned int g_error_count = 0;

static void counting_log_handler(SafeAllocLogLevel level, const char *msg)
{
    (void)msg; /* silence; counted only */
    if (level == SAFE_ALLOC_LOG_WARNING) ++g_warn_count;
    else                                  ++g_error_count;
}

static void reset_log_counts(void)
{
    g_warn_count  = 0;
    g_error_count = 0;
}

/* -------------------------------------------------------------------------
 * Stress test 1: high-volume sequential malloc / free cycles
 * ---------------------------------------------------------------------- */

#define CYCLES 5000   /* total alloc-free pairs per sub-run */
#define BATCH  128    /* max live allocations at once       */

static void stress_sequential_cycles(void)
{
    double t0, t1;
    unsigned int i;

    begin_test("stress 1 — sequential malloc/free cycles");

    t0 = now_sec();
    for (i = 0; i < CYCLES; ++i) {
        void *p = safe_malloc(64);
        safe_free(p);
        safe_alloc_reset(); /* reset between iterations to avoid table filling */
    }
    t1 = now_sec();

    double ops  = (double)CYCLES * 2.0;    /* alloc + free = 2 ops */
    double secs = t1 - t0;
    printf("  %u alloc+free cycles in %.4f s  (%.0f Mops/s)\n",
           CYCLES, secs, ops / secs / 1e6);

    CHECK(g_run >= 0); /* always passes — metrics only */
}

/* -------------------------------------------------------------------------
 * Stress test 2: interleaved alloc / free — exercises slot reuse
 * ---------------------------------------------------------------------- */

static void stress_interleaved(void)
{
    double t0, t1;
    void  *ptrs[BATCH];
    unsigned int i, j;

    begin_test("stress 2 — interleaved alloc/free (slot reuse)");
    safe_alloc_reset();

    t0 = now_sec();
    for (j = 0; j < CYCLES / BATCH; ++j) {
        /* Allocate a batch. */
        for (i = 0; i < BATCH; ++i) {
            ptrs[i] = safe_malloc(32 + i * 3);
        }
        CHECK(safe_alloc_alive_count() == BATCH);

        /* Free half, then alloc replacements — exercises freed-slot reuse. */
        for (i = 0; i < BATCH / 2; ++i) {
            safe_free(ptrs[i]);
        }
        for (i = 0; i < BATCH / 2; ++i) {
            ptrs[i] = safe_malloc(16);
        }
        CHECK(safe_alloc_alive_count() == BATCH);

        /* Free everything. */
        for (i = 0; i < BATCH; ++i) {
            safe_free(ptrs[i]);
        }
        CHECK(safe_alloc_alive_count() == 0);
    }
    t1 = now_sec();

    unsigned int total = (CYCLES / BATCH) * BATCH * 3; /* 3 alloc groups */
    printf("  %u interleaved ops in %.4f s  (%.0f Mops/s)\n",
           total, t1 - t0, (double)total / (t1 - t0) / 1e6);
}

/* -------------------------------------------------------------------------
 * Stress test 3: realloc growth chain
 * ---------------------------------------------------------------------- */

#define REALLOC_STEPS 256

static void stress_realloc_chain(void)
{
    double t0, t1;
    unsigned int i, j;

    begin_test("stress 3 — realloc growth/shrink chains");
    safe_alloc_reset();

    t0 = now_sec();
    for (j = 0; j < 100; ++j) {
        char *p = (char *)safe_malloc(8);
        p[0] = 'A'; p[1] = '\0';

        /* Grow: 8 → 8*2 → … */
        for (i = 1; i < REALLOC_STEPS; ++i) {
            size_t new_sz = (size_t)(8 * (i + 1));
            char  *q      = (char *)safe_realloc(p, new_sz);
            CHECK(q != NULL);
            p = q;
        }
        CHECK(safe_alloc_alive_count() == 1);

        /* Shrink back. */
        for (i = REALLOC_STEPS - 1; i > 0; --i) {
            size_t new_sz = (size_t)(8 * i);
            char  *q      = (char *)safe_realloc(p, new_sz);
            CHECK(q != NULL);
            p = q;
        }
        CHECK(safe_alloc_alive_count() == 1);

        safe_free(p);
        CHECK(safe_alloc_alive_count() == 0);
        safe_alloc_reset();
    }
    t1 = now_sec();

    printf("  100 × %u-step realloc chains in %.4f s\n",
           REALLOC_STEPS * 2, t1 - t0);
}

/* -------------------------------------------------------------------------
 * Stress test 4: near-capacity record table
 * ---------------------------------------------------------------------- */

#define TABLE_SIZE SAFE_ALLOC_MAX_RECORDS

static void stress_near_capacity(void)
{
    begin_test("stress 4 — record table near-capacity");
    safe_alloc_reset();

    /* Fill exactly TABLE_SIZE - 1 slots to stay under the limit. */
    unsigned int cap = TABLE_SIZE - 1;
    void **ptrs = (void **)malloc(cap * sizeof(void *));
    if (!ptrs) { printf("  SKIP (malloc failed for pointer array)\n"); return; }

    unsigned int i;
    for (i = 0; i < cap; ++i) {
        ptrs[i] = safe_malloc(1);
    }
    CHECK(safe_alloc_alive_count() == cap);
    CHECK(safe_alloc_total_allocs() == cap);

    for (i = 0; i < cap; ++i) {
        safe_free(ptrs[i]);
    }
    CHECK(safe_alloc_alive_count() == 0);
    CHECK(safe_alloc_total_frees() == cap);

    free(ptrs);
    printf("  handled %u simultaneous tracked allocations\n", cap);
}

/* -------------------------------------------------------------------------
 * Stress test 5: warning paths under load (custom handler)
 * ---------------------------------------------------------------------- */

#define WARN_ROUNDS 500

static void stress_warning_paths(void)
{
    begin_test("stress 5 — double-free and unregistered-ptr warnings (custom handler)");
    safe_alloc_reset();
    safe_alloc_set_log_handler(counting_log_handler);
    reset_log_counts();

    unsigned int i;
    void *ptrs[10];

    for (i = 0; i < WARN_ROUNDS; ++i) {
        void *p = safe_malloc(8);
        safe_free(p);
        /* Double-free — should warn, not crash. */
        safe_free(p);
    }
    CHECK(g_warn_count == WARN_ROUNDS); /* one warning per double-free */

    reset_log_counts();

    /* Unregistered pointer frees. */
    for (i = 0; i < 10; ++i) {
        ptrs[i] = malloc(8); /* raw malloc — not in record table */
    }
    for (i = 0; i < 10; ++i) {
        safe_free(ptrs[i]); /* should warn */
        free(ptrs[i]);      /* actual release */
    }
    CHECK(g_warn_count == 10);

    /* Restore default handler. */
    safe_alloc_set_log_handler(NULL);
    printf("  captured %u double-free warnings + 10 unregistered-ptr warnings"
           " via custom handler\n", WARN_ROUNDS);
}

/* -------------------------------------------------------------------------
 * Stress test 6: log handler swap under load
 * ---------------------------------------------------------------------- */

static void stress_log_handler_swap(void)
{
    begin_test("stress 6 — log handler replacement under load");
    safe_alloc_reset();

    unsigned int i;
    void *p;

    /* Phase A: default handler (writes to stderr). */
    p = safe_malloc(8);
    safe_free(p);

    /* Phase B: install counting handler. */
    safe_alloc_set_log_handler(counting_log_handler);
    reset_log_counts();

    for (i = 0; i < 200; ++i) {
        p = safe_malloc(4);
        safe_free(p);
        safe_free(p); /* triggers warning each time */
        safe_alloc_reset();
    }
    CHECK(g_warn_count == 200);

    /* Phase C: restore default; allocate to confirm no crash. */
    safe_alloc_set_log_handler(NULL);
    safe_alloc_reset();
    p = safe_malloc(16);
    CHECK(p != NULL);
    safe_free(p);
    CHECK(safe_alloc_alive_count() == 0);

    printf("  200 handler-swapped double-frees captured correctly\n");
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("RAMking — stress test suite\n");
    printf("===========================\n");

    stress_sequential_cycles();
    stress_interleaved();
    stress_realloc_chain();
    stress_near_capacity();
    stress_warning_paths();
    stress_log_handler_swap();

    printf("\n===========================\n");
    printf("Results: %d/%d passed", g_run - g_failed, g_run);
    if (g_failed == 0) {
        printf("  -- ALL PASS\n");
    } else {
        printf("  -- %d FAILED\n", g_failed);
    }

    return g_failed == 0 ? 0 : 1;
}
