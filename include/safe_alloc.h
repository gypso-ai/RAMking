/**
 * safe_alloc.h — Safe C Memory Management Interface
 *
 * Wraps malloc / calloc / realloc / free and tracks every allocation in a
 * compile-time-sized static record table.  Detects double-free, free of
 * unregistered pointers, and provides diagnostic helpers.
 *
 * Configuration macros (define BEFORE including this header, or via -D):
 *   SAFE_ALLOC_MAX_RECORDS   maximum number of simultaneously tracked
 *                            allocations  (default: 1024)
 */

#ifndef SAFE_ALLOC_H
#define SAFE_ALLOC_H

#include <stddef.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */
#ifndef SAFE_ALLOC_MAX_RECORDS
#  define SAFE_ALLOC_MAX_RECORDS 1024
#endif

/* -------------------------------------------------------------------------
 * Public record type (read-only view exposed to diagnostics)
 * ---------------------------------------------------------------------- */
typedef struct {
    void        *ptr;    /**< Allocated pointer                        */
    size_t       size;   /**< Requested allocation size in bytes       */
    int          freed;  /**< 0 = alive, 1 = freed                     */
    unsigned int seq;    /**< Monotonically increasing allocation index */
} SafeAllocRecord;

/* -------------------------------------------------------------------------
 * Core allocation API
 * ---------------------------------------------------------------------- */

/**
 * safe_malloc — allocate `size` bytes; returns NULL on failure.
 * Equivalent to malloc(size) but registers the pointer in the record table.
 */
void *safe_malloc(size_t size);

/**
 * safe_calloc — allocate `nmemb * size` bytes, zero-initialised.
 * Equivalent to calloc(nmemb, size) but registers the pointer.
 */
void *safe_calloc(size_t nmemb, size_t size);

/**
 * safe_realloc — resize a previously registered allocation.
 * Equivalent to realloc(ptr, new_size):
 *   - ptr == NULL  → behaves like safe_malloc(new_size)
 *   - new_size == 0 → behaves like safe_free(ptr), returns NULL
 *   - On failure the original allocation is left intact and NULL is returned.
 * Updates the record table atomically on success.
 */
void *safe_realloc(void *ptr, size_t new_size);

/**
 * safe_free — release a previously registered allocation.
 *   - ptr == NULL  → no-op (C standard behaviour)
 *   - Unregistered ptr → warning printed, pointer NOT freed
 *   - Already-freed ptr → double-free warning, pointer NOT freed again
 */
void safe_free(void *ptr);

/* -------------------------------------------------------------------------
 * Diagnostic / statistics API
 * ---------------------------------------------------------------------- */

/**
 * safe_alloc_alive_count — number of allocations currently not freed.
 */
unsigned int safe_alloc_alive_count(void);

/**
 * safe_alloc_peak_count — maximum number of simultaneously live allocations
 * observed since program start (or last safe_alloc_reset).
 */
unsigned int safe_alloc_peak_count(void);

/**
 * safe_alloc_total_allocs — cumulative number of successful allocations
 * (malloc + calloc + realloc-new).
 */
unsigned int safe_alloc_total_allocs(void);

/**
 * safe_alloc_total_frees — cumulative number of successful frees.
 */
unsigned int safe_alloc_total_frees(void);

/**
 * safe_alloc_dump_alive — print all currently-alive (non-freed) records to
 * stderr in a human-readable table.
 */
void safe_alloc_dump_alive(void);

/**
 * safe_alloc_dump_all — print every record (alive and freed) to stderr.
 */
void safe_alloc_dump_all(void);

/**
 * safe_alloc_get_records — fill `out` with up to `max_count` records and
 * return the actual number written.  Records are copied; caller owns nothing.
 */
unsigned int safe_alloc_get_records(SafeAllocRecord *out, unsigned int max_count);

/**
 * safe_alloc_reset — wipe the entire record table and reset all counters.
 * Intended for unit tests that want a clean slate between test cases.
 * WARNING: does NOT free any outstanding memory; use only in tests.
 */
void safe_alloc_reset(void);

#endif /* SAFE_ALLOC_H */
