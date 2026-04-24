/**
 * safe_alloc.c — Implementation of the safe C memory management interface.
 *
 * All allocation records are kept in a single static array so the module
 * never calls malloc for its own bookkeeping (avoids re-entrance issues and
 * keeps the implementation free of recursive allocation).
 */

#include "safe_alloc.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Log buffer size for safe_log internal formatting */
#define SAFE_LOG_BUFFER_SIZE 512

/* -------------------------------------------------------------------------
 * Pluggable log handler
 * ---------------------------------------------------------------------- */

/** Default handler: write to stderr. */
static void default_log_handler(SafeAllocLogLevel level, const char *msg)
{
    (void)level; /* severity visible in the "[safe_alloc] WARNING/ERROR" prefix */
    fprintf(stderr, "%s", msg);
}

static SafeAllocLogFn g_log_fn = default_log_handler;

void safe_alloc_set_log_handler(SafeAllocLogFn fn)
{
    g_log_fn = (fn != NULL) ? fn : default_log_handler;
}

/**
 * Internal helper — format a log message and dispatch it through the
 * currently-installed handler.
 */
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void safe_log(SafeAllocLogLevel level, const char *fmt, ...)
{
    char buf[SAFE_LOG_BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log_fn(level, buf);
}

/* -------------------------------------------------------------------------
 * Internal record table
 * ---------------------------------------------------------------------- */

/* One slot per tracked allocation; freed slots are reused. */
static SafeAllocRecord g_records[SAFE_ALLOC_MAX_RECORDS];

/* Number of slots that have ever been written (high-water mark into the array,
   not the same as alive count). */
static unsigned int g_used = 0;

/* Monotonically increasing sequence number assigned on each alloc. */
static unsigned int g_seq = 0;

/* Statistics */
static unsigned int g_alive       = 0;  /* current live allocations   */
static unsigned int g_peak        = 0;  /* maximum observed g_alive   */
static unsigned int g_total_allocs = 0; /* cumulative alloc calls     */
static unsigned int g_total_frees  = 0; /* cumulative free calls      */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Find the index of a live (non-freed) record for `ptr`, or -1. */
static int find_alive(void *ptr)
{
    unsigned int i;
    for (i = 0; i < g_used; ++i) {
        if (g_records[i].ptr == ptr && g_records[i].freed == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** Find ANY record (alive or freed) for `ptr`, or -1. */
static int find_any(void *ptr)
{
    unsigned int i;
    for (i = 0; i < g_used; ++i) {
        if (g_records[i].ptr == ptr) {
            return (int)i;
        }
    }
    return -1;
}

/** Find the index of a freed record slot that can be reused, or -1. */
static int find_free_slot(void)
{
    unsigned int i;
    for (i = 0; i < g_used; ++i) {
        if (g_records[i].freed) {
            return (int)i;
        }
    }
    /* No freed slot found; try to extend the array. */
    if (g_used < SAFE_ALLOC_MAX_RECORDS) {
        return (int)g_used; /* caller must increment g_used */
    }
    return -1; /* table full */
}

/** Register a new allocation. Returns 0 on success, -1 if table is full. */
static int register_alloc(void *ptr, size_t size)
{
    int idx = find_free_slot();
    if (idx < 0) {
        safe_log(SAFE_ALLOC_LOG_WARNING,
                 "[safe_alloc] WARNING: record table full (%u slots), "
                 "cannot track ptr=%p size=%zu\n",
                 SAFE_ALLOC_MAX_RECORDS, ptr, size);
        return -1;
    }

    if ((unsigned int)idx == g_used) {
        ++g_used;
    }

    g_records[idx].ptr   = ptr;
    g_records[idx].size  = size;
    g_records[idx].freed = 0;
    g_records[idx].seq   = ++g_seq;

    ++g_alive;
    ++g_total_allocs;
    if (g_alive > g_peak) {
        g_peak = g_alive;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Core allocation API
 * ---------------------------------------------------------------------- */

void *safe_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL) {
        safe_log(SAFE_ALLOC_LOG_ERROR,
                 "[safe_alloc] ERROR: malloc(%zu) failed\n", size);
        return NULL;
    }
    if (register_alloc(ptr, size) != 0) {
        /* Table full — still return the memory but warn that it's untracked. */
        safe_log(SAFE_ALLOC_LOG_WARNING,
                 "[safe_alloc] WARNING: allocation ptr=%p is untracked\n", ptr);
    }
    return ptr;
}

void *safe_calloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (ptr == NULL) {
        safe_log(SAFE_ALLOC_LOG_ERROR,
                 "[safe_alloc] ERROR: calloc(%zu, %zu) failed\n", nmemb, size);
        return NULL;
    }
    if (register_alloc(ptr, nmemb * size) != 0) {
        safe_log(SAFE_ALLOC_LOG_WARNING,
                 "[safe_alloc] WARNING: allocation ptr=%p is untracked\n", ptr);
    }
    return ptr;
}

void *safe_realloc(void *ptr, size_t new_size)
{
    int   old_idx;
    void *new_ptr;

    /* realloc(NULL, size) is defined to behave like malloc(size). */
    if (ptr == NULL) {
        return safe_malloc(new_size);
    }

    /* realloc(ptr, 0) is implementation-defined; treat as free. */
    if (new_size == 0) {
        safe_free(ptr);
        return NULL;
    }

    old_idx = find_alive(ptr);
    if (old_idx < 0) {
        /* Check if it was already freed (double-realloc). */
        int any = find_any(ptr);
        if (any >= 0 && g_records[any].freed) {
            safe_log(SAFE_ALLOC_LOG_WARNING,
                     "[safe_alloc] WARNING: safe_realloc called on already-freed "
                     "ptr=%p (seq=%u)\n",
                     ptr, g_records[any].seq);
        } else {
            safe_log(SAFE_ALLOC_LOG_WARNING,
                     "[safe_alloc] WARNING: safe_realloc called on unregistered "
                     "ptr=%p\n", ptr);
        }
        /* Still attempt the realloc so the caller's memory is not lost. */
    }

    new_ptr = realloc(ptr, new_size);
    if (new_ptr == NULL) {
        safe_log(SAFE_ALLOC_LOG_ERROR,
                 "[safe_alloc] ERROR: realloc(ptr=%p, %zu) failed — "
                 "original allocation preserved\n", ptr, new_size);
        return NULL;
    }

    /* Update (or create) the record. */
    if (old_idx >= 0) {
        /* Mark old record freed if the pointer moved. */
        if (new_ptr != ptr) {
            g_records[old_idx].freed = 1;
            --g_alive;
            ++g_total_frees;
            /* Register the new pointer. */
            register_alloc(new_ptr, new_size);
        } else {
            /* Pointer unchanged — just update the size. */
            g_records[old_idx].size = new_size;
        }
    } else {
        /* Was untracked; register the new pointer. */
        register_alloc(new_ptr, new_size);
    }

    return new_ptr;
}

void safe_free(void *ptr)
{
    int idx;

    /* C standard: free(NULL) is a no-op. */
    if (ptr == NULL) {
        return;
    }

    idx = find_alive(ptr);
    if (idx < 0) {
        /* Check for double-free. */
        int any = find_any(ptr);
        if (any >= 0 && g_records[any].freed) {
            safe_log(SAFE_ALLOC_LOG_WARNING,
                     "[safe_alloc] WARNING: double-free detected for "
                     "ptr=%p (originally allocated as seq=%u, size=%zu)\n",
                     ptr, g_records[any].seq, g_records[any].size);
        } else {
            safe_log(SAFE_ALLOC_LOG_WARNING,
                     "[safe_alloc] WARNING: free of unregistered ptr=%p\n",
                     ptr);
        }
        /* Do NOT call free() — prevents undefined behaviour. */
        return;
    }

    g_records[idx].freed = 1;
    --g_alive;
    ++g_total_frees;
    free(ptr);
}

/* -------------------------------------------------------------------------
 * Diagnostic / statistics API
 * ---------------------------------------------------------------------- */

unsigned int safe_alloc_alive_count(void)  { return g_alive; }
unsigned int safe_alloc_peak_count(void)   { return g_peak; }
unsigned int safe_alloc_total_allocs(void) { return g_total_allocs; }
unsigned int safe_alloc_total_frees(void)  { return g_total_frees; }

void safe_alloc_dump_alive(void)
{
    unsigned int i;
    unsigned int count = 0;
    char line[128];

    safe_log(SAFE_ALLOC_LOG_WARNING, "[safe_alloc] --- alive allocations ---\n");
    safe_log(SAFE_ALLOC_LOG_WARNING, "  seq     ptr                size\n");
    for (i = 0; i < g_used; ++i) {
        if (g_records[i].freed == 0) {
            snprintf(line, sizeof(line), "  %-6u  %-18p  %zu\n",
                     g_records[i].seq, g_records[i].ptr, g_records[i].size);
            safe_log(SAFE_ALLOC_LOG_WARNING, "%s", line);
            ++count;
        }
    }
    snprintf(line, sizeof(line), "[safe_alloc] total alive: %u\n", count);
    safe_log(SAFE_ALLOC_LOG_WARNING, "%s", line);
}

void safe_alloc_dump_all(void)
{
    unsigned int i;
    char line[128];

    safe_log(SAFE_ALLOC_LOG_WARNING, "[safe_alloc] --- full allocation history ---\n");
    safe_log(SAFE_ALLOC_LOG_WARNING, "  seq     ptr                size        status\n");
    for (i = 0; i < g_used; ++i) {
        snprintf(line, sizeof(line), "  %-6u  %-18p  %-10zu  %s\n",
                 g_records[i].seq,
                 g_records[i].ptr,
                 g_records[i].size,
                 g_records[i].freed ? "freed" : "ALIVE");
        safe_log(SAFE_ALLOC_LOG_WARNING, "%s", line);
    }
    snprintf(line, sizeof(line),
             "[safe_alloc] stats: alive=%u  peak=%u  "
             "total_allocs=%u  total_frees=%u\n",
             g_alive, g_peak, g_total_allocs, g_total_frees);
    safe_log(SAFE_ALLOC_LOG_WARNING, "%s", line);
}

unsigned int safe_alloc_get_records(SafeAllocRecord *out, unsigned int max_count)
{
    unsigned int i, n = 0;
    if (out == NULL || max_count == 0) {
        return 0;
    }
    for (i = 0; i < g_used && n < max_count; ++i) {
        out[n++] = g_records[i];
    }
    return n;
}

void safe_alloc_reset(void)
{
    memset(g_records, 0, sizeof(g_records));
    g_used        = 0;
    g_seq         = 0;
    g_alive       = 0;
    g_peak        = 0;
    g_total_allocs  = 0;
    g_total_frees   = 0;
}
