# wordcount - Production C99 Word Frequency Library

A production-quality, embeddable C99 library for word frequency counting with optional memory limits, comprehensive error handling, and cross-platform support.

## Design Philosophy

This library demonstrates expert-level C patterns distilled from Redis, SQLite, musl libc, and Linux kernel practices (see `research.md` for detailed rationale):

- **Arena allocation** - Single-reset cleanup, zero fragmentation
- **Open addressing hash table** - Linear probing with power-of-2 sizing
- **FNV-1a hashing** - Simple, effective, collision-resistant
- **Overflow-safe arithmetic** - All size calculations checked
- **Goto-cleanup error handling** - Canonical Linux kernel pattern
- **Memory accounting** - Optional hard limits for embedded systems

## Features

- **Pure C99** - No dependencies beyond standard library
- **Cross-platform** - Windows and POSIX via clean abstraction layer
- **Memory-bounded** - Optional hard caps on internal allocations
- **OOM-hardened** - Graceful failure even under memory pressure
- **Embeddable** - Single header + single .c file
- **Thread-safe** - Multiple instances can be used concurrently (no shared state)
- **Comprehensive tests** - 60+ unit tests including SQLite-style OOM injection

## Quick Start

### Build

```bash
# Library + CLI + tests
mkdir -p build && cd build
cmake ..
make

# Run tests
make test
# or directly:
./wc_test

# Use the CLI
./wc ../book.txt
cat file.txt | ./wc
```

### Integration

Simply include `wordcount.h` and compile with `wordcount.c`:

```bash
gcc -std=c99 -O2 wordcount.c your_program.c -o program
```

### Basic Usage

```c
#include "wordcount.h"

int main(void) {
    wc *w = wc_open(0);  // 0 = default 64-byte max word length
    if (!w) return 1;

    // Option 1: Add individual words (case-sensitive)
    wc_add(w, "hello");
    wc_add(w, "world");

    // Option 2: Scan text (lowercases automatically)
    const char *text = "Hello World! How's it going?";
    wc_scan(w, text, strlen(text));

    // Get sorted results
    wc_word *results;
    size_t count;
    if (wc_results(w, &results, &count) == WC_OK) {
        for (size_t i = 0; i < count; i++) {
            printf("%zu: %s\n", results[i].count, results[i].word);
        }
        wc_results_free(results);
    }

    printf("Total: %zu  Unique: %zu\n", wc_total(w), wc_unique(w));
    wc_close(w);
    return 0;
}
```

## API Reference

### Lifecycle

```c
wc *wc_open(size_t max_word);
```
Create word counter with default settings. `max_word`: max stored word length (0 = default 64, clamped to [4, 1024]). Returns NULL on allocation failure.

```c
wc *wc_open_ex(size_t max_word, const wc_limits *limits);
```
Create with memory limits and tuning parameters. See `wc_limits` below.

```c
void wc_close(wc *w);
```
Destroy word counter. NULL-safe.

### Word Entry

```c
int wc_add(wc *w, const char *word);
```
Add single word (**case-sensitive**). Returns `WC_OK` (0), `WC_ERROR` (1), or `WC_NOMEM` (2).

```c
int wc_scan(wc *w, const char *text, size_t len);
```
Scan text for words (**lowercases automatically**). Only ASCII letters (A-Z, a-z) are recognized; all other bytes are separators.

**Critical Difference:**
- `wc_add("Hello")` and `wc_add("hello")` create **two distinct** entries
- `wc_scan("Hello", 5)` stores as `"hello"` (normalized)

### Queries

```c
size_t wc_total(const wc *w);
size_t wc_unique(const wc *w);
```
Return total/unique word counts. NULL-safe (returns 0).

```c
int wc_results(const wc *w, wc_word **out, size_t *n);
```
Get sorted results (descending by count, then alphabetically). Caller must free via `wc_results_free()`.

```c
void wc_results_free(wc_word *results);
```
Free results array. NULL-safe.

### Utilities

```c
const char *wc_errstr(int code);
const char *wc_version(void);
```
Error code to string, version info.

## Memory Management

### Custom Allocators

Override default malloc/free by defining macros before including the header:

```c
#define WC_MALLOC(n) my_malloc(n)
#define WC_FREE(p)   my_free(p)
#include "wordcount.h"
```

### Memory Limits

For embedded systems or untrusted input, set hard memory caps:

```c
wc_limits lim = {
    .max_bytes = 1048576,  // 1MB total budget
    .init_cap = 512,       // Initial hash table size (optional)
    .block_size = 4096     // Arena block size (optional)
};

wc *w = wc_open_ex(64, &lim);
// ... use normally, wc_add/wc_scan will return WC_NOMEM when budget exhausted
```

**What counts against max_bytes:**
- Hash table storage (grows dynamically)
- Arena blocks (word storage)
- Internal scan buffer (if `WC_STACK_BUFFER=0`)

**What does NOT count:**
- The `wc` handle itself
- Results array returned by `wc_results()` (caller-owned)

### Environment Variable (CLI only)

The CLI tool respects `WC_MAX_BYTES`:

```bash
WC_MAX_BYTES=8388608 ./wc largefile.txt  # 8MB limit
```

## Word Detection Specification

This library uses **ASCII-letter-only** word detection:

- A **word** is a maximal run of ASCII letters (`A-Z`, `a-z`)
- **All other bytes are separators**: digits, punctuation, UTF-8 sequences, etc.
- Case normalization: `wc_scan()` lowercases via bitwise OR: `c | 32`

### Examples

```c
wc_scan(w, "it's", 4);        // 2 words: "it", "s"
wc_scan(w, "foo-bar", 7);     // 2 words: "foo", "bar"
wc_scan(w, "abc123def", 9);   // 2 words: "abc", "def"
wc_scan(w, "café", 5);        // 1 word: "caf" (é is separator)
```

### Truncation

Words exceeding `max_word` are truncated **during hashing**:

```c
wc *w = wc_open(4);
wc_add(w, "testing");   // Stored as "test"
wc_add(w, "tested");    // Also "test" - count increments
```

**Important:** The entire input run is consumed, but only the first `max_word` bytes contribute to hash/storage. This prevents treating one long word as multiple words.

## Architecture

### Hash Table

- **Open addressing** with linear probing
- **Power-of-2 sizing** for fast modulo via masking: `hash & (cap - 1)`
- **Load factor 0.7** - table doubles when 70% full
- **FNV-1a hash** - computed incrementally, resistant to collisions

Implementation detail: Stored hash in each slot enables fast rejection on collision chain before doing expensive `memcmp()`.

### Arena Allocator

- **Linked list of blocks** - first block created on init, new blocks added as needed
- **Zero fragmentation** - all words allocated from contiguous memory
- **Automatic alignment** - portable alignment calculation without `uintptr_t`
- **Single-reset cleanup** - `wc_close()` walks block list once

This eliminates per-word allocation overhead and makes memory leaks structurally impossible.

### Platform Abstraction (CLI)

`wc_main.c` provides zero-copy file processing via memory mapping:

**POSIX:**
```c
mmap(PROT_READ, MAP_PRIVATE) + madvise(MADV_SEQUENTIAL)
```

**Windows:**
```c
CreateFileMapping() + MapViewOfFile(FILE_MAP_READ)
```

Both paths share the same processing code via a unified `MappedFile` abstraction.

### Error Handling

The library follows the **goto-cleanup** pattern from Linux kernel style:

```c
int func(void) {
    int rc = -1;
    char *buf = NULL;

    buf = malloc(1024);
    if (!buf) goto cleanup;

    // ... work
    rc = 0;

cleanup:
    free(buf);
    return rc;
}
```

All pointers initialized to NULL, cleanup unconditional, single exit point.

## Testing

The test suite (`wc_test.c`) includes:

### Functional Tests
- Lifecycle: open/close, NULL safety, limits enforcement
- wc_add: single/duplicate/multiple words, truncation, empty strings
- wc_scan: case folding, punctuation, numbers, binary data (embedded NUL)
- wc_results: sorting (count desc + alpha asc), empty results
- Queries: NULL handling, version/error strings

### Stress Tests
- 100,000 duplicate insertions
- 10,000 unique words
- 50,000 words triggering multiple arena blocks
- Table growth through multiple doublings

### OOM Injection (glibc-specific)

Compile with `-DWC_TEST_OOM` to enable SQLite-style torture testing:

```bash
gcc -O0 -g -DWC_TEST_OOM wordcount.c wc_test.c -o wc_test_oom
./wc_test_oom
```

This interposes malloc/realloc to fail at specific call counts, verifying graceful degradation:

```c
// Test all allocation failure points
test_oom_open();      // 10 injection points during wc_open
test_oom_add();       // 20 injection points during wc_add
test_oom_scan();      // 30 injection points during wc_scan
test_oom_results();   // 10 injection points during wc_results
test_oom_growth();    // OOM during table resize
test_oom_torture();   // Exhaustive: tries 50 consecutive failure points
```

**Portability Note:** OOM harness uses glibc-specific `__libc_malloc`/`__libc_realloc`. For other platforms, compile with `-DWC_MALLOC=my_malloc -DWC_FREE=my_free` and implement interposition in your test wrapper.

## Build Configuration

### Compile-Time Options

Define before including `wordcount.h`:

```c
// Custom allocator
#define WC_MALLOC(n) arena_alloc(n)
#define WC_FREE(p)   arena_free(p)

// Use heap instead of stack for scan buffer (embedded systems)
#define WC_STACK_BUFFER 0

// Disable assertions (smaller code, less safety)
#define WC_OMIT_ASSERT

// Tune defaults for 16-bit systems
#define WC_DEFAULT_INIT_CAP 128
#define WC_DEFAULT_BLOCK_SZ 1024

#include "wordcount.h"
```

### Platform Defaults

Library automatically adapts to platform via `SIZE_MAX`:

| Platform       | SIZE_MAX       | init_cap | block_size |
|----------------|----------------|----------|------------|
| 16-bit         | ≤ 65535        | 128      | 1KB        |
| 32-bit         | ≤ 4294967295   | 1024     | 16KB       |
| 64-bit         | > 4294967295   | 4096     | 64KB       |

## Performance Characteristics

| Operation       | Time Complexity | Notes                                      |
|-----------------|-----------------|--------------------------------------------|
| wc_add          | O(1) amortized  | Hash lookup + arena alloc                  |
| wc_scan         | O(n)            | Single pass, hash computed incrementally   |
| wc_total        | O(1)            | Counter maintained                         |
| wc_unique       | O(1)            | Counter maintained                         |
| wc_results      | O(n log n)      | qsort over unique words                    |

**Memory:**
- Hash table: O(unique words) * sizeof(Slot) ≈ 24 bytes/entry on 64-bit
- Arena: O(total word bytes) + block overhead
- Peak during wc_results: +O(unique words) * sizeof(wc_word) for sorted array

## CLI Usage

The `wc` binary processes files or stdin:

```bash
# Single file
./wc book.txt

# Multiple files (combined stats)
./wc file1.txt file2.txt file3.txt

# Stdin
cat *.txt | ./wc

# With memory limit
WC_MAX_BYTES=1048576 ./wc largefile.txt
```

**Output Format:**

```
  Count  Word                  %
-------  --------------------  ------
   5432  the                   2.34
   3210  and                   1.82
    ...

Total: 928012  Unique: 33782
```

Top 10 words to stdout, summary to stderr (enables separate redirection).

## Files

```
library/
├── wordcount.h          # Public API (277 lines)
├── wordcount.c          # Core implementation (741 lines)
├── wc_main.c           # CLI with mmap (543 lines)
├── wc_test.c           # Test suite (957 lines)
├── research.md         # Design philosophy and C patterns
└── README.md           # This file
```

**Total:** ~2500 lines including comprehensive tests and documentation.

## Design Patterns Demonstrated

This library showcases C mastery patterns from leading codebases:

1. **Opaque Handles** (Redis) - Public API uses `typedef struct wc wc;`, definition hidden
2. **Arena Allocation** (SQLite) - Linked blocks with single-reset cleanup
3. **Flexible Array Member** (musl) - `char buf[]` for inline storage
4. **Compile-Time Assertions** - typedef array trick enforces platform assumptions
5. **Goto-Cleanup** (Linux kernel) - Single exit point, NULL-safe cleanup
6. **Overflow-Safe Arithmetic** - Explicit checks before all size calculations
7. **Function Pointer Dispatch** - qsort comparator demonstrates data-centric design
8. **Bit Manipulation** (musl) - Branchless ASCII checks: `((c | 32) - 'a') < 26`
9. **Power-of-2 Sizing** (LuaJIT) - Hash table uses masking instead of modulo
10. **Platform Abstraction** (libuv) - Clean separation via `#ifdef` blocks

See `research.md` for detailed rationale and expert references (Rob Pike, antirez, Rich Felker, etc.).

## Comparison to Benchmark Implementations

This library serves as the **portable, embeddable foundation** for the benchmark suite:

| Implementation         | Purpose                          | Lines | Dependencies        |
|------------------------|----------------------------------|-------|---------------------|
| `library/wordcount.c`  | Embeddable C99 library           | 741   | None (pure libc)    |
| `wordcount.c`          | Parallel benchmark (mmap)        | ~500  | POSIX/Windows APIs  |
| `wordcount_hyperopt.c` | AVX-512 SIMD + CRC32C           | ~800  | x86-64 intrinsics   |

**Use the library when you need:**
- Portable code (embedded, WASM, unusual platforms)
- Memory limits (untrusted input, resource constraints)
- API integration (not just file processing)
- Comprehensive testing and OOM hardening

**Use the benchmarks when you need:**
- Maximum throughput on x86-64 (hyperopt)
- Memory-mapped parallel processing (wordcount.c)

## License

Public domain. See individual file headers.

## Contributing

This library prioritizes **simplicity over features**. Before proposing changes:

1. Does it maintain C99 compatibility?
2. Does it avoid new dependencies?
3. Does it preserve the single-header + single-implementation model?
4. Can it be tested via the existing harness?

Performance optimizations welcome if they don't compromise portability or clarity.

## References

- **research.md** - Design philosophy, expert sources, C mastery patterns
- **CMakeLists.txt** - Build configuration with strict warnings
- **wc_test.c** - Comprehensive test coverage including OOM injection

For questions about the word detection specification, see the main benchmark README.
