# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is a comprehensive **word frequency counter performance benchmark** comparing text processing implementations across C, Rust, Go, C#, JavaScript, and PHP. The primary focus is on demonstrating extreme performance optimization techniques in C while maintaining production-quality code across all language implementations.

## Architecture

### Implementation Variants

1. **Standard C (`wordcount.c`)**: Reference implementation using FNV-1a hash and chaining-based hash table
2. **Hyperoptimized C (`wordcount_hyperopt.c`)**: Highly optimized parallel implementation featuring:
   - AVX-512 SIMD text processing (64-byte chunks)
   - Hardware CRC32C hashing (SSE4.2)
   - V-Cache aware CPU affinity for AMD processors
   - Memory pool allocation (32MB per thread)
   - Lock-free multi-threading (default 6 threads)
   - Open addressing hash tables with linear probing
3. **Language Variants**: Idiomatic implementations in Rust, Go, C#, JavaScript (Node.js), and PHP

### Key Design Patterns

- **Word Definition**: All byte-level implementations use ASCII letters [a-zA-Z] only. PHP uses regex `\b[a-z]+\b` pattern, resulting in slightly different counts (-82 words)
- **Hash Tables**: Standard C uses chaining; hyperopt uses open addressing with 70% load factor threshold
- **Memory Strategy**: Hyperopt pre-allocates 32MB string pools per thread to minimize allocations
- **CPU Affinity**: Hyperopt auto-detects AMD V-Cache topology by parsing `/sys/devices/system/cpu/`
- **Thread Count Tuning**: 6 threads optimal for small files (5MB), 12 threads better for very large files (>100MB)

## Build Commands

### Standard Builds
```bash
# C reference
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
    wordcount.c -o wordcount_c

# C hyperopt (6-thread default)
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread \
    wordcount_hyperopt.c -o wordcount_hopt -lm

# Rust
rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 \
    wordcount.rs -o wordcount_rust

# Go
go build -gcflags="-B" -ldflags="-s -w" -o wordcount_go wordcount.go

# C#
dotnet build -c Release

# JavaScript/PHP - no build needed
```

### Architecture-Specific Builds (AMD Zen 5)
```bash
# For GCC >= 14 with Zen 5 support
gcc -O3 -march=znver5 -mtune=znver5 -mavx512f -mavx512bw -mavx512vl -msse4.2 \
    -flto -fomit-frame-pointer -funroll-loops -pthread \
    wordcount_hyperopt.c -o wordcount_hopt -lm

# For older compilers
gcc -O3 -march=znver4 -mtune=znver4 -mavx512f -mavx512bw -mavx512vl -msse4.2 \
    -flto -fomit-frame-pointer -funroll-loops -pthread \
    wordcount_hyperopt.c -o wordcount_hopt -lm
```

### Custom Thread Count
```bash
# Build with specific thread count
gcc -O3 -march=native -DNUM_THREADS=12 -pthread \
    wordcount_hyperopt.c -o wordcount_hopt_12t -lm
```

### Debug Builds
```bash
# Debug with full instrumentation (use -O2, not -O0)
gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=native -pthread \
    wordcount_hyperopt.c -o wordcount_hopt_debug -lm
```

## Running Tests

### Individual Implementations
```bash
./wordcount_c book.txt
./wordcount_hopt book.txt
./wordcount_rust book.txt
GOGC=off ./wordcount_go book.txt
./bin/Release/net8.0/WordCount book.txt
node --max-old-space-size=4096 wordcount.js book.txt
php wordcount.php book.txt
```

### Benchmark Scripts

**Full language comparison** (`bench.sh`):
```bash
# Standard benchmark (3 runs default)
taskset -c 0-23 ./bench.sh --runs=10

# With validation (compares all implementations)
taskset -c 0-23 ./bench.sh --validate --runs=3

# Test with larger files (interactive prompt)
# Will create book2.txt (5x) and book3.txt (25x) if you answer 'y'
./bench.sh --runs=10
```

**C implementation comparison** (`bench_c.sh`):
```bash
# Release mode: Compare 6-thread and 12-thread builds
./bench_c.sh --hyperonly --large --runs=10 --pin=0-23

# Thread count scan
./bench_c.sh --hyperonly --large --scan-threads=6,8,12,16 --runs=10 --pin=0-23

# Debug mode: Full instrumentation and analysis
./bench_c.sh -d --hyperonly --large --profile \
  --events="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses" \
  --pin=0-23 --runs=3 --bundle

# Validation mode
./bench_c.sh --validate --runs=3
```

### Environment Variables
```bash
# Go - disable GC for consistent performance
GOGC=off ./wordcount_go book.txt

# C# - disable tiered compilation
DOTNET_TieredCompilation=0 ./bin/Release/net8.0/WordCount book.txt

# SIMD control for hyperopt
WORDCOUNT_SIMD=0 ./wordcount_hopt book.txt  # Disable AVX-512
```

## Performance Characteristics

### Expected Throughput
- **C Hyperopt (6-thread)**: 712 MB/s on 5.3MB, 4.58 GB/s on large files
- **Standard C**: ~220 MB/s (consistent across file sizes)
- **Rust**: ~265 MB/s on 5.3MB
- **Go**: ~180 MB/s on 5.3MB
- **C#/JavaScript**: ~60 MB/s on 5.3MB
- **PHP**: ~100 MB/s on 5.3MB

### Scaling Behavior
All implementations scale linearly with file size. Hyperopt shows superlinear scaling due to better cache utilization and memory bandwidth saturation on large files.

## Debug and Profiling

### Debug Mode Features
When built with `-DDEBUG`, the hyperopt implementation logs:
- Per-thread timing and throughput
- Hash table collision statistics (avg/max probe length)
- Memory allocation tracking (pool usage, exhaustions)
- SIMD vs scalar chunk counts
- UTF-8 sequence statistics
- CPU affinity verification
- Hash distribution analysis

Output: `wordcount_debug.log` and console summary

### Performance Profiling
```bash
# Using perf
perf record -g ./wordcount_hopt book.txt
perf report --stdio | head -50

# Using bench_c.sh with perf stat
./bench_c.sh -d --profile --events="cycles,instructions,cache-misses" --runs=3

# Generate debug bundle with system info
./bench_c.sh -d --bundle --runs=3
```

## Validation and Word Counts

All implementations except PHP produce **928,012 words** on book.txt (5.3MB). PHP produces **927,930 words** (-82) due to regex boundary handling differences.

The `--validate` flag compares:
- Total word count
- Unique word count
- Top 10 most frequent words (exact ordering)

## Test Files

- `book.txt` (5.3MB): Primary test file (Moby Dick from Project Gutenberg)
- `book2.txt` (27MB): 5x multiplied version for medium-scale testing
- `book3.txt` (131MB): 25x multiplied version for large-scale testing

Files are auto-downloaded if missing. Use `--large` flag with bench_c.sh to test all three sizes.

## Important Implementation Details

### wordcount_hyperopt.c Specifics
- **Thread Count**: Default 6 threads (optimal for AMD Ryzen 9 9950X3D on small/medium files)
- **Memory Pools**: 32MB per thread, increase `STRING_POOL_SIZE` if pool exhaustions occur
- **Max Word Length**: 100 characters (longer words truncated)
- **Cache Line Size**: 64 bytes (structures aligned to prevent false sharing)
- **Initial Hash Capacity**: 65536 entries (16 bits), grows at 70% load factor
- **V-Cache Detection**: Parses sysfs to find CPUs with largest L3 cache
- **SIMD Fallback**: Automatically falls back to scalar processing if AVX-512 unavailable

### Platform Requirements
- **OS**: Linux (uses mmap, sysfs, pthread)
- **Architecture**: x86-64 for hyperopt (AVX-512, SSE4.2 for CRC32C)
- **Compiler**: GCC 9+ recommended, GCC 14+ for Zen 5 optimizations
- **WSL2**: Fully supported with ~5% overhead vs native Linux

## Compiler-Specific Notes

### Critical Flags for Hyperopt
- `-O3`: Maximum optimization
- `-march=native` or `-march=znver5`: Enable all CPU features
- `-flto`: Link-time optimization (critical for inlining across TUs)
- `-fomit-frame-pointer`: Free up register
- `-funroll-loops`: Unroll small loops
- `-pthread`: POSIX threads support
- `-lm`: Math library (for topology calculations)

### Preprocessor Macros
- `NUM_THREADS=N`: Set thread count (default 6)
- `DEBUG`: Enable detailed logging and metrics
- `__AVX512BW__`: Auto-detected for AVX-512 support
- `__SSE4_2__`: Auto-detected for CRC32C support

## Known Issues and Limitations

1. **UTF-8 Handling**: Non-ASCII characters treated as word separators (by design)
2. **Word Length Limit**: 100 characters in hyperopt, 64 in standard C
3. **Platform**: Hyperopt requires Linux x86-64
4. **V-Cache on WSL2**: Topology may not be fully exposed, falls back gracefully
5. **Very High Unique Word Counts**: >5M unique words may exhaust memory pools

## Directory Structure

```
.
├── wordcount.c              # Reference C implementation
├── wordcount_hyperopt.c     # Hyperoptimized parallel C
├── wordcount.rs             # Rust implementation
├── wordcount.go             # Go implementation
├── WordCount.cs             # C# implementation
├── wordcount.js             # JavaScript (Node.js) implementation
├── wordcount.php            # PHP implementation
├── bench.sh                 # Cross-language benchmark script
├── bench_c.sh               # C-specific comparison tool
├── install-deps.sh          # Dependency installation script
├── nerd c/                  # Alternative C library-style implementation
│   ├── wordcount.c          # Core library
│   ├── wordcount.h          # Header file
│   ├── wc_main.c            # Main executable
│   └── wc_test.c            # Test suite
├── README.MD                # Performance results and methodology
└── WORDCOUNT_HYPEROPT_DOCS.MD  # Detailed hyperopt documentation
```

## Tips for Modifications

1. **Adding New Optimizations**: Always benchmark before/after with `bench_c.sh --runs=100`
2. **Changing Hash Function**: Update both `hash_word()` and fingerprint calculation
3. **Thread Count Tuning**: Test 4, 6, 8, 12, 16 threads with `--scan-threads`
4. **Memory Pool Size**: Monitor pool exhaustions in debug mode, adjust `STRING_POOL_SIZE`
5. **Hash Table Sizing**: Balance initial capacity vs resize overhead
6. **SIMD Optimization**: Check SIMD/scalar ratio in debug logs (>3:1 is good)

## Common Commands Reference

```bash
# Install all dependencies (Ubuntu/WSL2)
./install-deps.sh

# Quick benchmark
./bench.sh --runs=10

# Full C analysis with profiling
./bench_c.sh -d --hyperonly --large --profile --pin=0-23 --runs=10 --bundle

# Validate correctness
./bench.sh --validate

# Test custom thread count
gcc -O3 -march=native -DNUM_THREADS=8 -pthread wordcount_hyperopt.c -o wc8 -lm
./wc8 book.txt

# Profile with perf
perf stat -e cycles,instructions,cache-misses ./wordcount_hopt book.txt
```
