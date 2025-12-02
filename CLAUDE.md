# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a multi-language word frequency counter benchmark comparing performance across C, Rust, Go, C#, JavaScript (Node.js), and PHP. The project focuses on optimizing word parsing and counting from large text files, with particular emphasis on C hyperoptimization techniques.

## Build Commands

### Quick Build and Run (All Languages)
```bash
# Install dependencies (Ubuntu/Debian)
./install-deps.sh

# Run full benchmark (all languages, 3 runs each)
./bench.sh

# Run with validation
./bench.sh --validate

# Run with custom number of runs
./bench.sh --runs=10
```

### Individual Language Builds

**C (Reference Implementation)**
```bash
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
    ./build/wordcount.c -o wordcount_c
```

**C (Hyperoptimized - 6 threads)**
```bash
gcc -O3 -march=znver5 -mtune=znver5 -mavx512f -mavx512bw -mavx512vl -msse4.2 \
    -flto -fomit-frame-pointer -funroll-loops -pthread \
    -DNUM_THREADS=6 wordcount_hyperopt.c -o wordcount_hopt_t6 -lm
```

**C (Hyperoptimized - 12 threads)**
```bash
gcc -O3 -march=znver5 -mtune=znver5 -mavx512f -mavx512bw -mavx512vl -msse4.2 \
    -flto -fomit-frame-pointer -funroll-loops -pthread \
    -DNUM_THREADS=12 wordcount_hyperopt.c -o wordcount_hopt_t12 -lm
```

**Rust**
```bash
rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 \
    wordcount.rs -o wordcount_rust
```

**Go**
```bash
go build -gcflags="-B" -ldflags="-s -w" -o wordcount_go wordcount.go
```

**C# (.NET 8)**
```bash
dotnet build -c Release --nologo --verbosity quiet
```

**JavaScript**
```bash
# No build needed, run directly with Node.js
node wordcount.js book.txt
```

**PHP**
```bash
# No build needed, run directly
php wordcount.php book.txt
```

### C-Specific Benchmarking and Debugging

**Basic C comparison (reference vs hyperopt)**
```bash
./bench_c.sh
```

**Debug mode with full instrumentation**
```bash
./bench_c.sh -d --profile --runs=5
```

**Hyperopt-only mode (skip reference)**
```bash
./bench_c.sh --hyperonly --runs=10
```

**Test with larger files**
```bash
./bench_c.sh --large --runs=5
```

**Scan multiple thread counts**
```bash
./bench_c.sh --hyperonly --scan-threads=4,6,8,12,16 --runs=10
```

**CPU pinning for stable measurements**
```bash
./bench_c.sh --hyperonly --pin=0-23 --runs=10
```

**Perf profiling with custom events**
```bash
./bench_c.sh -d --profile \
  --events="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses" \
  --runs=3
```

## Architecture

### Core Implementations

**wordcount.c (Reference)**
- Simple chained hash table implementation
- Single-threaded, no SIMD
- FNV-1a hashing
- Used as performance baseline

**wordcount_hyperopt.c (Performance-Optimized)**
- Multi-threaded with configurable NUM_THREADS (default: 6 or 12)
- SIMD-accelerated parsing using AVX-512 and SSE4.2
- Memory-mapped file I/O with madvise hints
- Open-addressed hash table with quadratic probing
- CPU affinity pinning to V-Cache CCDs (Zen 5 optimization)
- Lock-free per-thread hash tables merged at end
- Custom memory pool allocator

### Language Implementation Patterns

All implementations follow a similar structure:
1. Read entire file into memory
2. Parse words (lowercase alphabetic sequences)
3. Count word frequencies using hash tables
4. Sort by frequency (descending), then alphabetically
5. Output top 100 words with statistics

Hash functions:
- **C/C hyperopt**: FNV-1a (32-bit or 64-bit)
- **Rust**: FNV-1a (64-bit)
- **Go**: FNV-1a (32-bit)
- **C#**: .NET default Dictionary hash
- **JavaScript/PHP**: Language default

### Benchmark Scripts

**bench.sh**
- Cross-language benchmark runner
- Supports book.txt (1.2MB), book2.txt (5x), book3.txt (25x)
- Automatic dependency checking and compilation
- Validation mode compares output consistency
- Performance ranking with slowdown ratios

**bench_c.sh**
- Focused C implementation comparison tool
- Debug mode with extensive instrumentation (SIMD efficiency, hash statistics, thread performance)
- perf integration for hardware counter profiling
- Thread count scanning
- CPU pinning support
- Creates detailed debug logs and bundles

### Output Files

Each implementation generates `{filename}_{lang}_results.txt` containing:
- Top 100 most frequent words
- Total and unique word counts
- Execution time
- Percentage distribution

## Performance Targets

The hyperoptimized C implementation targets 3.0 GB/s throughput on Zen 5 architecture (9950X3D). Key optimization techniques:

1. **SIMD Parsing**: AVX-512 vector instructions for whitespace detection
2. **Thread Affinity**: Pin to V-Cache CCDs for maximum cache hit rate
3. **Memory Layout**: Lock-free per-thread tables reduce contention
4. **Hash Table**: Open addressing with optimized probing reduces indirection
5. **Compiler Flags**: `-march=znver5` enables Zen 5-specific optimizations

## Test Data

The default test file is Moby Dick from Project Gutenberg:
```bash
curl -s https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt
```

Larger test files can be generated by concatenating book.txt multiple times (bench.sh handles this automatically).

## Development Notes

- The C hyperopt version has both DEBUG and release modes. DEBUG mode adds extensive logging to wordcount_debug.log
- GCC 14+ is recommended for best Zen 5 performance (-march=znver5 support)
- For GCC <14, bench scripts fall back to -march=native or -march=znver4
- The .NET implementation uses ReadyToRun compilation and trimming for optimal startup
- Go version should be run with GOGC=off to disable garbage collection during benchmarks
- All implementations use similar word parsing rules: lowercase ASCII letters only, case-insensitive
