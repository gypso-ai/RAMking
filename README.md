# RAMking - Safe C Memory Management

RAMking wraps the standard C allocation functions (`malloc`, `calloc`,
`realloc`, `free`) and tracks every live allocation in a configurable record
table.  It detects **double-free**, **unregistered-pointer free**, and provides
rich diagnostic APIs — all without allocating bookkeeping storage by default.

## Features

| Feature | Description |
|---|---|
| `safe_malloc` | Allocate memory and register the pointer |
| `safe_calloc` | Zero-initialised allocation |
| `safe_realloc` | Resize and atomically update the record |
| `safe_free` | Free and mark record; warns on double-free or unregistered pointer |
| `safe_alloc_set_allocators` | Replace the underlying malloc/calloc/realloc/free hooks |
| `safe_alloc_set_record_buffer` | Use caller-provided memory as the record table |
| `safe_alloc_alive_count` | Currently live (non-freed) allocations |
| `safe_alloc_peak_count` | Maximum simultaneously live allocations observed |
| `safe_alloc_total_allocs` | Cumulative successful allocation count |
| `safe_alloc_total_frees` | Cumulative successful free count |
| `safe_alloc_dump_alive` | Print all live allocations to stderr |
| `safe_alloc_dump_all` | Print full allocation history to stderr |
| `safe_alloc_get_records` | Copy records into a caller-supplied array |
| `safe_alloc_reset` | Reset all state (for unit tests) |

## Quick Start

```c
#include "safe_alloc.h"

int main(void) {
    char *buf = (char *)safe_malloc(64);
    buf = (char *)safe_realloc(buf, 128);
    safe_free(buf);
    safe_free(buf);   /* double-free → warning on stderr, no crash */

    safe_alloc_dump_all();
    return 0;
}
```

## Build

```
make        # build test binary
make test   # build and run all tests
make clean  # remove build artefacts
```

### Configuration

Override `SAFE_ALLOC_MAX_RECORDS` to change the maximum number of
simultaneously tracked allocations (default 1024):

```
make CFLAGS="-DSAFE_ALLOC_MAX_RECORDS=4096 -Wall -Wextra -std=c99 -g"

You can also switch allocator hooks or the record buffer at runtime when there
are no live allocations:

```c
SafeAllocRecord records[256];

safe_alloc_set_allocators(custom_malloc, custom_calloc, custom_realloc, custom_free);
safe_alloc_set_record_buffer(records, 256);
```

## Files

```
include/safe_alloc.h   Public API header
src/safe_alloc.c       Implementation
test/test_safe_alloc.c Tests / usage demo
Makefile               Build rules
```
