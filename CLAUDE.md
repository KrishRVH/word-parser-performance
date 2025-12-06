# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a performance benchmark comparing word frequency counting implementations across multiple languages (C, Rust, Go, C#, JavaScript, PHP). The project focuses on demonstrating optimization techniques, particularly hardware-specific optimizations with AVX-512 and multi-threading.

## Build Commands

### C Implementations

```bash
# Reference C implementation (idiomatic, memory-mapped)
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
    ./wordcount.c -o wordcount_c

# C hyperopt (6-thread variant)
gcc -O3 -march=native -pthread -DNUM_THREADS=6 \
    wordcount_hyperopt.c -o wordcount_hopt_t6 -lm

# C hyperopt (12-thread variant)
gcc -O3 -march=native -pthread -DNUM_THREADS=12 \
    wordcount_hyperopt.c -o wordcount_hopt_t12 -lm

# For AMD Ryzen with GCC >= 14:
gcc -O3 -march=znver5 -mtune=znver5 -mavx512f -mavx512bw -mavx512vl -msse4.2 \
    -flto -fomit-frame-pointer -funroll-loops -pthread -DNUM_THREADS=6 \
    wordcount_hyperopt.c -o wordcount_hopt_t6 -lm
```

### Other Languages

```bash
# Rust
rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 \
    wordcount.rs -o wordcount_rust

# Go
go build -gcflags="-B" -ldflags="-s -w" -o wordcount_go wordcount.go

# C# (.NET)
dotnet build -c Release

# JavaScript/Node.js - no build needed
# PHP - no build needed
```

### CMake Build (library + tests)

```bash
mkdir -p build && cd build
cmake ..
make

# Run tests
ctest
# or
./wc_test
```

## Running Benchmarks

### Quick Benchmark (All Languages)

```bash
# Standard benchmark with validation
taskset -c 0-23 ./bench.sh --validate --runs=10

# Benchmark only (no validation)
taskset -c 0-23 ./bench.sh --runs=3
```

### C-Only Benchmark (Detailed Comparison)

```bash
# Quick comparison (reference vs hyperopt)
./bench_c.sh --runs=5

# Hyperopt only (skip reference)
./bench_c.sh --hyperonly --runs=10

# Test with large files (5x and 25x)
./bench_c.sh --large --runs=10 --pin=0-23

# Scan multiple thread counts
./bench_c.sh --scan-threads=4,6,8,12 --pin=0-23

# With validation output
./bench_c.sh --validate --hyperonly --large
```

### Run Individual Implementations

```bash
./wordcount_c book.txt
./wordcount_hopt_t6 book.txt
./wordcount_rust book.txt
GOGC=off ./wordcount_go book.txt
./bin/Release/net8.0/WordCount book.txt
node wordcount.js book.txt
php wordcount.php book.txt
```

## Architecture Overview

### Word Definition Specification

**Critical**: This benchmark uses a strict ASCII-letter-only word definition, NOT `wc -w`'s whitespace-delimited definition.

- A word is a maximal run of ASCII letters only (`A-Z`, `a-z`)
- All other bytes (digits, punctuation, non-ASCII) are separators
- Case-insensitive: `"Apple"` == `"APPLE"` == `"apple"`
- Example: `"it's"` → two words: `"it"`, `"s"` (apostrophe is separator)

### Implementation Approaches

**C Reference (`wordcount.c`)**:
- Memory-mapped I/O with parallel processing
- Per-thread hash tables with arena allocation
- FNV-1a hashing
- No shared mutable state in hot path
- Cross-platform (Windows/POSIX via conditional compilation)

**C Hyperopt (`wordcount_hyperopt.c`)**:
- AVX-512 SIMD tokenization with scalar fallback
- CRC32C hardware hashing (FNV-1a fallback)
- Per-thread hash tables with arena pools
- Configurable thread count via `-DNUM_THREADS=N`
- V-Cache aware thread pinning for AMD Zen 4+
- Huge page hints for performance
- Open addressing hash table
- Environment: `WORDCOUNT_SIMD=0` to disable SIMD

**Other Languages**:
- Rust: Byte-level processing, FNV HashMap, zero-copy strings
- Go: 64KB buffer, byte-level scanning, `GOGC=off` required for performance
- C#: Pre-sized dictionary, byte-level processing
- JavaScript: Direct buffer processing with Node.js
- PHP: Regex-based (`\b[a-z]+\b`), produces slightly different counts (-82 words)

### Validation System

The benchmark includes cross-language validation:
- C reference implementation is ground truth
- All implementations must match on total/unique word counts
- Top 10 words must be identical across implementations
- Use `--validate` flag with bench scripts

### Performance Characteristics

**Threading Sweet Spots**:
- Small files (5.3MB): 6 threads optimal
- Large files (261MB): 12 threads optimal
- Platform: WSL2/Ubuntu on AMD Ryzen 9 9950X3D

**Expected Results** (book.txt ~5.3MB):
- C Hyperopt (6T): ~0.008s (baseline, 685 MB/s)
- Rust: ~0.020s (2.9x slower)
- C Reference: ~0.022s (3.1x slower)
- Go: ~0.030s (4.3x slower)
- PHP: ~0.051s (7.3x slower)
- C#/.NET: ~0.090s (12.9x slower)
- JavaScript: ~0.090s (12.9x slower)

## Key Implementation Details

### Hash Table Design

**C Reference**: Chaining with separate allocations
**C Hyperopt**: Open addressing with linear probing, 16-bit fingerprints

### Thread Safety

**C implementations**: Embarrassingly parallel - each thread has its own hash table, merged at end. No locks in hot path.

### Memory Management

**Arena Allocation**: Both C implementations use arena allocators to eliminate per-word allocation overhead. The hyperopt version includes overflow handling for when the arena exhausts.

### SIMD Tokenization (Hyperopt)

AVX-512 is used for parallel byte classification when available. Scalar fallback ensures portability. The SIMD path processes 64 bytes at a time, identifying word boundaries in parallel.

### Platform Compatibility

`wordcount.c` supports both Windows and POSIX via conditional compilation at the top of the file. Windows uses `CreateFileMapping`/`MapViewOfFile`, POSIX uses `mmap`.

## Development Workflow

### Testing Changes to C Implementations

```bash
# Compile and validate output matches reference
gcc -O3 -march=native wordcount.c -o wordcount_test
./wordcount_test book.txt > test_output.txt

# Compare with reference
gcc -O3 -march=native wordcount.c -o wordcount_ref
./wordcount_ref book.txt > ref_output.txt
diff test_output.txt ref_output.txt
```

### Debugging Performance

```bash
# Compile with debug symbols
gcc -O3 -march=native -g wordcount_hyperopt.c -o wordcount_debug -pthread -lm

# Profile with perf (Linux)
perf record -e cycles,instructions,cache-misses ./wordcount_debug book.txt
perf report

# Run with address sanitizer
gcc -O1 -g -fsanitize=address wordcount_hyperopt.c -o wordcount_asan -pthread -lm
./wordcount_asan book.txt
```

### Quality Checks

```bash
# Use c-quality.sh for static analysis
./c-quality.sh
```

## Common Pitfalls

1. **Go requires `GOGC=off`**: Go's GC significantly impacts performance without this
2. **PHP word count differs**: Uses `\b` regex boundaries, produces 927,930 words vs 928,012
3. **Thread count tuning**: Optimal thread count depends on file size and CPU
4. **CPU pinning matters**: Use `taskset` for consistent results
5. **File size**: `wc -w` reports 901,325 words (whitespace-delimited), but benchmark spec requires 928,012 (ASCII-letter tokens)
6. **Hyperopt thread count**: Must be set at compile time via `-DNUM_THREADS=N`

## File Organization

```
.
├── wordcount.c               # Reference C implementation (parallel, portable)
├── wordcount_hyperopt.c      # Optimized C with AVX-512/CRC32C
├── wordcount.{rs,go,js,php}  # Other language implementations
├── WordCount.cs              # C# implementation
├── bench.sh                  # Multi-language benchmark runner
├── bench_c.sh                # C-only detailed benchmark
├── c-quality.sh              # Static analysis script
├── library/                  # C99 library implementation
│   ├── wordcount.c           # Core library (C99, portable)
│   ├── wc_main.c            # CLI wrapper
│   ├── wc_test.c            # Unit tests
│   └── research.md          # Design philosophy and patterns
├── CMakeLists.txt           # Build configuration
└── README.md                # Detailed documentation
```

## Notes for AI Assistants

- When modifying C implementations, preserve the word definition spec exactly
- Performance changes should be validated with `bench_c.sh --validate`
- The hyperopt implementation is platform-specific (x86-64 with AVX-512); maintain scalar fallbacks
- Cross-platform code should follow the pattern in `wordcount.c` (compile-time platform detection)
- Thread counts are compile-time constants for hyperopt to enable better optimization
- The research.md file contains detailed design philosophy from C masters - reference when making architectural decisions
