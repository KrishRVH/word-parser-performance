# wordcount – a small, idiomatic C99 word-frequency library

This project provides:

- A reusable, C99 word-frequency library (`libwordcount.a`)
- A minimal CLI tool (`wc`) that counts word frequencies from files/stdin
- A unit test harness (`wc_test`) exercising the public API

The code is intended both as production-quality utility code and as a
didactic example of clean, portable C99:

- No global state
- Opaque handle for the main data structure
- Careful error handling with explicit status codes
- Strict, well-documented ownership and lifetimes
- Portable character handling via `<ctype.h>` following CERT STR34/STR37

The public API is declared in `include/wordcount.h`. The implementation
is in `src/wordcount.c`. The CLI and tests demonstrate correct usage.

---

## Directory layout

```text
include/
    wordcount.h     Public API header

src/
    wordcount.c     Library implementation
    wc_main.c       CLI driver (builds `wc`)

tests/
    wc_test.c       Unit tests for the library

Makefile            Builds library, CLI, and tests
README.md           This file
```

You can freely add:

- `docs/` for extended documentation
- `bin/`, `lib/`, `build/` – created by the Makefile
- `LICENSE` if you want a separate license file

The source files themselves declare their copyright / license
(`Public domain / CC0` in the header’s documentation).

---

## Building

### Prerequisites

Any reasonably modern C compiler with C99 support:

- GCC 5+ on Linux/BSD/macOS
- Clang/LLVM 3.8+ (including Apple Clang)
- MinGW-w64 GCC (via MSYS2, Cygwin, or w64devkit) on Windows

A POSIX-like shell environment is assumed for the Makefile (including
on Windows, via:

- MSYS2
- Cygwin
- WSL / WSL2
- [w64devkit](https://github.com/skeeto/w64devkit))

### Recommended compiler flags

The Makefile enables, by default:

- `-std=c99 -Wall -Wextra -Wpedantic`
- For GCC/Clang additionally:
  - `-Wshadow -Wconversion -Wcast-qual -Wwrite-strings`
  - `-Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wvla`

These follow long-standing recommendations from GCC documentation and
the CERT C Coding Standard, and are intended to catch a wide range of
defects in portable C code.

You can override or extend `CFLAGS` on the command line:

```sh
make CFLAGS='-O0 -g -std=c99 -Wall -Wextra -Wpedantic'
```

### Build everything (library, CLI, tests)

From the project root:

```sh
make
```

This produces:

- `lib/libwordcount.a`   – static library
- `bin/wc`               – command-line tool
- `bin/wc_test`          – unit test executable

### Run tests

```sh
make test
# or: make check
```

The tests use `assert(3)`; any failure aborts the process and prints the
failing assertion.

### Clean

```sh
make clean
```

Removes `build/`, `bin/`, and `lib/`.

---

## Using the library

The public header is `include/wordcount.h`. At a high level:

- `wc_table` is an opaque handle to a word-count hash table.
- `wc_create` / `wc_destroy` manage the table’s lifetime.
- `wc_add_word` inserts a pre-normalized word (no case folding).
- `wc_process_text` tokenizes raw text using `isalpha()` and `tolower()`,
  counting words case-insensitively.
- `wc_snapshot` returns a sorted array of `(word, count)` pairs.
- `wc_total_words` / `wc_unique_words` query aggregate counts.
- `wc_version` exposes the library version string at runtime.

### Example: linking against the library

After building (`make`), you can link your own program against
`libwordcount.a` like this:

```sh
cc -std=c99 -Iinclude -c my_program.c -o my_program.o
cc my_program.o -Llib -lwordcount -o my_program
```

Inside `my_program.c`:

```c
#include "wordcount.h"
#include <stdio.h>

int main(void) {
    const char *text = "The quick brown fox jumps over the lazy dog.";

    wc_table *t = wc_create(NULL);
    if (!t) {
        /* handle out-of-memory */
        return 1;
    }

    wc_status st = wc_process_text(t, text, strlen(text));
    if (st != WC_OK) {
        /* handle error */
        wc_destroy(t);
        return 1;
    }

    wc_entry *entries = NULL;
    size_t    count   = 0;

    st = wc_snapshot(t, &entries, &count);
    if (st == WC_OK) {
        for (size_t i = 0; i < count; ++i) {
            printf("%zu %s\n", entries[i].count, entries[i].word);
        }
    }

    wc_free_snapshot(entries);
    wc_destroy(t);
    return (st == WC_OK) ? 0 : 1;
}
```

---

## CLI usage

Once built, the `wc` binary provides a simple, portable word-frequency
tool:

```sh
# From the project root after `make`
bin/wc file1.txt file2.txt

# Or read from stdin:
cat file.txt | bin/wc
```

Behavior:

- If one or more file names are given, each is opened (`"rb"`), read
  fully into memory, and passed to `wc_process_text()`.
- If no files are specified, stdin is read instead.
- Word counts are printed to stdout in descending frequency, with
  alphabetical order as a tiebreaker.
- A summary (`Total words`, `Unique words`) is printed to stderr.

---

## Tests

`bin/wc_test` exercises:

- Table creation/destruction and configuration options
- Insertion via `wc_add_word` and via `wc_process_text`
- Case-folding and tokenization semantics
- Snapshot sorting and tie-breaking rules
- Behavior on empty input and error conditions
- Stress scenarios (many unique words, many duplicates, truncation,
  large initial capacities)

Run them via:

```sh
make test
# or:
bin/wc_test
```

---

## Portability and locale / Unicode notes

Character handling follows the CERT C rules STR34-C and STR37-C:

- All calls to `isalpha()` and `tolower()` receive arguments cast from
  `unsigned char` (or `EOF`), avoiding undefined behavior for negative
  `char` values.
- The definition of a “word” is locale-dependent:
  - A word is a maximal sequence for which `isalpha()` is non-zero.
  - This depends on the active C locale (`setlocale()`).

If you need predictable, ASCII-only behavior:

- Ensure the `"C"` locale is active for the process before calling
  into the library:

  ```c
  #include <locale.h>

  setlocale(LC_CTYPE, "C");
  ```

Unicode:

- The library does **not** attempt full Unicode or UTF-8 parsing.
- Multi-byte sequences are treated byte-by-byte through `<ctype.h>`.
- For Unicode-aware tokenization, consider doing higher-level parsing in
  your application, then feeding normalized words to `wc_add_word()`.

---

## Thread-safety

- Each `wc_table` instance is self-contained; there is no global state.
- A single `wc_table` is **not** internally synchronized:
  - Do not access the same table concurrently from multiple threads
    without external synchronization.
- Different `wc_table` instances may be used freely from different
  threads.

---

## Non-goals and design choices

- **Not streaming-aware**: `wc_process_text()` operates on a contiguous
  buffer; it does not track state across chunks. Streaming use cases
  should handle boundary conditions above this layer.
- **Not cryptographic**: FNV-1a is chosen for speed and distribution in
  hash tables, not for security.
- **Static library by default**: Shared-library build systems (e.g.
  CMake) are intentionally omitted to keep this example focused and
  self-contained. It is straightforward to wrap this library in CMake,
  Meson, etc., if desired.

---

## License

The header and source files declare:

> Public domain / CC0

You may use, modify, and redistribute this code without restriction.
If you package this as a standalone project, you may wish to add a
separate `LICENSE` file mirroring that statement.