# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A comprehensive performance benchmark comparing text processing implementations across C, Rust, Go, C#, JavaScript, and PHP. The project includes a hyperoptimized C implementation using AVX-512, CRC32C hardware hashing, and V-Cache aware threading that achieves 4.67 GB/s throughput.

## Build and Run Commands

### Quick Start
```bash
# Install all dependencies (Ubuntu/WSL2)
./install-deps.sh

# Run full benchmark across all languages (release mode)
taskset -c 0-23 ./bench.sh --runs=10

# Run with validation (ensures all implementations match reference)
taskset -c 0-23 ./bench.sh --validate --runs=3
```

### C Implementation Commands

#### Standard C (Reference Implementation)
```bash
# Build
gcc -O3 -march=native -flto wordcount.c -o wordcount_c

# Run
./wordcount_c book.txt
```

#### C Hyperoptimized (Performance Target)
```bash
# Build (requires AVX-512 support)
gcc -O3 -march=znver5 -mtune=znver5 -flto -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# Build with fallback for older GCC
gcc -O3 -march=native -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# Run (6-thread default, optimized for small files)
./wordcount_hopt book.txt

# Run with custom thread count
NUM_THREADS=12 ./wordcount_hopt book.txt

# Run with SIMD disabled (fallback mode)
WORDCOUNT_SIMD=0 ./wordcount_hopt book.txt
```

#### C Hyperopt Benchmarking Tool
```bash
# Release mode: 6-thread and 12-thread variants, 10 runs
./bench_c.sh --hyperonly --large --runs=10 --pin=0-23

# Thread count exploration
./bench_c.sh --hyperonly --large --scan-threads=6,8,12,16 --runs=10 --pin=0-23

# Debug mode with full instrumentation and perf profiling
./bench_c.sh -d --hyperonly --large --profile \
  --events="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses" \
  --pin=0-23 --runs=3 --bundle

# Custom thread count
./bench_c.sh --hyperonly --threads=8 --runs=5
```

### Other Languages

#### Rust
```bash
# Build (standard)
rustc -O wordcount.rs -o wordcount_rust

# Build (hyperoptimized)
RUSTFLAGS="-C target-cpu=native -C opt-level=3 -C lto=fat -C codegen-units=1" rustc wordcount.rs -o wordcount_rust_opt

# Run
./wordcount_rust book.txt
```

#### Go
```bash
# Build
go build -o wordcount_go wordcount.go

# Build (optimized)
go build -ldflags="-s -w" -o wordcount_go wordcount.go

# Run (disable garbage collector for performance)
GOGC=off ./wordcount_go book.txt
```

#### C#
```bash
# Build
dotnet build -c Release

# Run
./bin/Release/net8.0/WordCount book.txt
```

#### JavaScript
```bash
# Run
node wordcount.js book.txt

# Run with increased heap size
node --max-old-space-size=4096 wordcount.js book.txt
```

#### PHP
```bash
# Run
php wordcount.php book.txt
```

### Code Quality Tools (C only)
```bash
# Run all quality checks (format, tidy, cppcheck)
./c-quality.sh

# Run with custom parallelism
JOBS=16 ./c-quality.sh

# Run from specific source root
./c-quality.sh /path/to/source
```

### CMake Build (Alternative for C reference)
```bash
# Configure and build
mkdir -p build && cd build
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
make

# Run
./wordcount_c ../book.txt
```

## Architecture and Design

### Word Definition Specification

This benchmark uses **ASCII letter-only** word boundaries, NOT `wc -w` style whitespace boundaries.

**Word Definition:**
- A word is a maximal run of ASCII letters: `[A-Za-z]+`
- All other bytes (digits, punctuation, non-ASCII) are separators
- Case-insensitive counting (all words lowercased)
- Maximum word length may be truncated internally (64-100 bytes), but counting semantics unchanged

**Examples:**
- `"foo-bar"` → 2 words: `"foo"`, `"bar"`
- `"it's"` → 2 words: `"it"`, `"s"`
- `"123abc456"` → 1 word: `"abc"`

**Expected Counts for book.txt:**
- Benchmark spec (ASCII letters): **928,012 words**
- `wc -w` (whitespace-delimited): **901,325 words**
- PHP implementation (regex `\b`): **927,930 words** (-82 due to boundary differences)

### Implementation Comparison

**C Reference (`wordcount.c`):**
- Ground truth for validation
- FNV-1a hash with chaining
- 8KB streaming buffer
- Arena allocator for strings
- ~220 MB/s throughput

**C Hyperoptimized (`wordcount_hyperopt.c`):**
- Multi-threaded (6-12 threads optimal)
- AVX-512 SIMD text processing (64 bytes/iteration)
- CRC32C hardware hashing with 16-bit fingerprint
- V-Cache aware CPU affinity for AMD Ryzen
- Per-thread hash tables with memory pools (32MB)
- Lock-free processing, single-threaded merge
- 685 MB/s (6 threads, small files) to 4.67 GB/s (12 threads, large files)

**Rust (`wordcount.rs`):**
- FNV HashMap
- Zero-copy string views
- Full file loaded into memory
- ~20ms for 5.3MB file

**Go (`wordcount.go`):**
- Built-in map with 64KB buffer
- Run with `GOGC=off` for best performance
- ~30ms for 5.3MB file

**JavaScript (`wordcount.js`):**
- Direct buffer processing
- Map for counting
- Full file in memory
- ~90ms for 5.3MB file

**C# (`WordCount.cs`):**
- Pre-sized Dictionary
- Full file in memory
- ~90ms for 5.3MB file

**PHP (`wordcount.php`):**
- Regex-based extraction with `preg_match_all`
- `\b[a-z]+\b` pattern (differs from spec)
- Native C regex engine
- ~51ms for 5.3MB file

### File Structure

**Source Files:**
- `wordcount.c` - Reference C implementation (canonical spec)
- `wordcount_hyperopt.c` - Hyperoptimized parallel C implementation
- `wordcount.rs` - Rust implementation
- `wordcount.go` - Go implementation
- `WordCount.cs` - C# implementation
- `wordcount.js` - JavaScript (Node.js) implementation
- `wordcount.php` - PHP implementation

**Build Scripts:**
- `bench.sh` - Cross-language benchmark runner
- `bench_c.sh` - C-specific benchmark and optimization tool
- `install-deps.sh` - Dependency installer for Ubuntu/WSL2
- `c-quality.sh` - Code quality checker (format, tidy, cppcheck)

**Configuration:**
- `CMakeLists.txt` - CMake build configuration
- `.clang-format` - Code formatting rules
- `.clang-tidy` - Static analysis configuration
- `.claude/settings.local.json` - Claude Code permissions

**Test Data:**
- `book.txt` - 5.3MB test file (Moby Dick)
- `book2.txt` - 53MB test file (10x concatenation)
- `book3.txt` - 261MB test file (50x concatenation)

**Documentation:**
- `README.MD` - Project overview and results
- `WORDCOUNT_HYPEROPT_DOCS.MD` - Detailed hyperopt documentation
- `LICENSE` - MIT license

## Key Implementation Details

### C Hyperopt Threading Model

**Thread Count Selection:**
- Small files (5.3MB): 6 threads optimal (685 MB/s)
- Large files (261MB): 12 threads optimal (4.67 GB/s)
- Controlled via `NUM_THREADS` environment variable
- Build multiple variants with `bench_c.sh --scan-threads=6,8,12,16`

**V-Cache Affinity:**
- Automatically detects AMD Ryzen V-Cache topology
- Parses `/sys/devices/system/cpu/*/cache/` for L3 cache sizes
- Pins threads to CCD with largest L3 (96MB+ on 9950X)
- 10-15% performance improvement on AMD processors

**Work Distribution:**
- File divided into equal byte ranges
- Threads adjust boundaries to avoid splitting words
- Barrier synchronization ensures simultaneous start
- Per-thread hash tables merged in single-threaded phase

### Hash Table Design

**C Reference:**
- Chaining with linked lists
- FNV-1a hash function
- 16K buckets (2^14)
- Arena allocation for nodes

**C Hyperopt:**
- Open addressing with linear probing
- CRC32C hardware hash with MurmurHash3 finalization
- 16-bit fingerprint for fast rejection
- Power-of-2 sizing (starts at 2^18, grows to 2^20)
- 90% load factor threshold triggers resize

### Validation Specification

**Validation Process:**
1. Run C reference to obtain ground truth counts
2. Run all other implementations on same file
3. Verify `total_words` and `unique_words` match reference
4. Verify top 10 words list matches exactly

**Validation Modes:**
- `bench.sh --validate` - Validates all languages
- `bench_c.sh --validate` - Validates C variants only

**Known Differences:**
- PHP differs by -82 words due to regex `\b` boundaries
- All other implementations must match exactly

### Performance Profiling

**Debug Mode Instrumentation:**
```bash
# Full debug build with metrics
./bench_c.sh -d --hyperonly --large --profile --runs=3 --bundle
```

**Collected Metrics:**
- Per-thread timing and work distribution
- Hash table statistics (collisions, probe lengths, load factor)
- Memory allocation tracking (pool vs malloc)
- SIMD vs scalar chunk counts
- UTF-8 error detection
- CPU affinity verification
- perf stat hardware counters

**Output:**
- Debug logs per thread and phase
- System information bundle (`debug_bundle.tar.gz`)
- Perf stat results for hardware analysis

## Development Workflows

### Adding a New Language Implementation

1. Implement word counting following the ASCII-letter spec
2. Add build/run commands to `bench.sh`
3. Test with validation: `./bench.sh --validate --runs=3`
4. Verify counts match: 928,012 total, check top 10 words
5. Add to README.MD performance table

### Optimizing C Hyperopt

1. Build debug variant: `./bench_c.sh -d --hyperonly --runs=3`
2. Profile with perf: `./bench_c.sh -d --profile --events="cycles,instructions,cache-misses"`
3. Analyze debug logs for bottlenecks
4. Test thread count scaling: `./bench_c.sh --scan-threads=4,6,8,12,16 --runs=10`
5. Validate correctness: `./bench_c.sh --validate`

### Code Quality Checks

The `c-quality.sh` script runs three tools:

1. **clang-format**: Auto-formats all `.c` and `.h` files in-place
2. **clang-tidy**: Static analysis with `.clang-tidy` configuration
3. **cppcheck**: Additional static analysis for bugs and style

**Usage:**
- Run before committing C code changes
- Uses `compile_commands.json` if available
- Parallelizes across all CPU cores
- Non-zero exit on issues

### Benchmark Methodology

**Standard Testing:**
- 200 runs per test for statistical accuracy
- C hyperopt: 100 runs for precise measurement
- Three file sizes: 5.3MB, 53MB, 261MB
- CPU pinning with `taskset -c 0-23` for consistency

**Environment:**
- Platform: WSL2 Ubuntu 24.04
- CPU: AMD Ryzen 9 9950X3D (32-core, 64-thread)
- RAM: 64GB
- Features: AVX-512, CRC32C, V-Cache

**Validation:**
- `--validate` flag required for correctness verification
- All implementations must produce identical results
- Top 10 words must match exactly (count and order)

## Common Gotchas

### C Hyperopt

- **AVX-512 requirement**: Falls back to scalar if unavailable, check `WORDCOUNT_SIMD=0`
- **Thread count tuning**: Default 6 threads may not be optimal for your hardware
- **CPU pinning**: Use `taskset` or `--pin` for consistent results
- **Debug builds**: Much slower, use only for instrumentation
- **Memory pools**: 32MB per thread, may exhaust on very large unique word counts

### Cross-Language Differences

- **PHP word count**: -82 words due to `\b` regex boundary semantics
- **Go GC**: Always use `GOGC=off` for performance testing
- **JavaScript heap**: May need `--max-old-space-size` for large files
- **C# JIT warmup**: First run may be slower due to JIT compilation

### Validation Failures

- Check word definition implementation (ASCII letters only)
- Verify case-insensitive lowercasing (per-byte for ASCII)
- Ensure word truncation doesn't split tokens
- Confirm top 10 sorting (descending count, ascending lexicographic)

## Environment Variables

- `NUM_THREADS`: Override hyperopt thread count (default: 6)
- `WORDCOUNT_SIMD`: Set to 0 to disable AVX-512 (hyperopt)
- `GOGC`: Set to `off` for Go performance testing
- `LC_ALL=C` and `LANG=C`: Set by scripts for stable locale handling
- `JOBS`: Override parallelism for `c-quality.sh` (default: nproc)

## Testing Best Practices

- Always use `taskset` for CPU pinning in benchmarks
- Run validation mode before publishing results
- Test on all three file sizes for scaling analysis
- Use debug mode only when investigating specific issues
- Compare against reference C implementation, not `wc -w`
