# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a word frequency counter performance benchmark comparing implementations across C, Rust, Go, C#, JavaScript, and PHP. The project focuses on optimizing text processing performance, with a hyperoptimized C implementation achieving 4.67 GB/s throughput using AVX-512 SIMD, CRC32C hashing, and parallel processing.

## Key Commands

### Building All Implementations
```bash
# Install all dependencies (Ubuntu/WSL2)
./install-deps.sh

# Build C implementations
gcc -O3 -march=native wordcount.c -o wordcount_c
gcc -O3 -march=native -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# Build Rust
rustc -O wordcount.rs -o wordcount_rust
# Or with full optimizations:
RUSTFLAGS="-C target-cpu=native -C opt-level=3 -C lto=fat -C codegen-units=1" rustc wordcount.rs -o wordcount_rust_opt

# Build Go
go build -o wordcount_go wordcount.go

# Build C# (.NET 8)
dotnet build -c Release
```

### Running Benchmarks

```bash
# Quick benchmark (all languages, 3 runs)
taskset -c 0-23 ./bench.sh --runs=3

# Full benchmark (all languages, 10 runs)
taskset -c 0-23 ./bench.sh --runs=10

# Validation mode (ensures all implementations produce same results)
taskset -c 0-23 ./bench.sh --validate --runs=3

# C-only hyperoptimized benchmark (release mode)
./bench_c.sh --hyperonly --large --runs=10 --pin=0-23

# C-only with profiling and debugging
./bench_c.sh -d --hyperonly --large --profile --events="cycles,instructions,cache-misses" --pin=0-23 --runs=3 --bundle

# Thread count scanning for optimization
./bench_c.sh --hyperonly --large --scan-threads=6,8,12,16 --runs=10 --pin=0-23
```

### Running Individual Implementations
```bash
# C implementations
./wordcount_c book.txt
./wordcount_hopt book.txt

# Rust
./wordcount_rust book.txt

# Go (with GC disabled for better performance)
GOGC=off ./wordcount_go book.txt

# C#
./bin/Release/net8.0/WordCount book.txt

# JavaScript
node wordcount.js book.txt

# PHP
php wordcount.php book.txt
```

## Architecture

### File Structure
- `wordcount.c` - Reference C implementation using FNV-1a hash
- `wordcount_hyperopt.c` - Hyperoptimized C with AVX-512, CRC32C, parallel processing
- `wordcount.rs` - Rust implementation with FNV HashMap
- `wordcount.go` - Go implementation with 64KB buffer streaming
- `WordCount.cs` - C# implementation with pre-sized dictionary
- `wordcount.js` - JavaScript with direct buffer processing
- `wordcount.php` - PHP using regex extraction

### Key Design Decisions

1. **Word Definition**: All implementations except PHP use byte-level processing (ASCII letters [a-zA-Z]). PHP uses regex word boundaries.

2. **Hash Table Strategy**: 
   - C hyperopt: Open addressing with linear probing, 16-bit fingerprints
   - C reference: Custom hash table with FNV-1a
   - Others: Language-native hash maps

3. **Parallelization**: Only `wordcount_hyperopt.c` uses multi-threading (configurable, default 6 threads)

4. **Memory Management**:
   - C hyperopt: Per-thread memory pools (32MB pre-allocated)
   - Streaming: C and Go use buffered streaming
   - Full load: Rust, JS, C#, PHP load entire file

### Performance Critical Paths

1. **AVX-512 Processing** (`wordcount_hyperopt.c`):
   - `process_chunk_avx512()` - Processes 64 bytes per iteration
   - Falls back to scalar for non-AVX-512 systems

2. **CRC32C Hashing** (`wordcount_hyperopt.c`):
   - Hardware CRC32 instructions via SSE4.2
   - Incremental computation during word extraction

3. **V-Cache Optimization** (`wordcount_hyperopt.c`):
   - Auto-detects AMD V-Cache topology
   - Pins threads to optimal cores

## Test Files

- `book.txt` - 5.3MB test file (~900K words)
- `book2.txt` - 53MB test file (if present)
- `book3.txt` - 261MB test file (if present)

Use `--large` flag with `bench_c.sh` to test all three files.

## Optimization Flags

- **C Hyperopt**: `-O3 -march=native -pthread` (or `-march=znver5` with GCC 14+)
- **C Reference**: `-O3 -march=native -flto`
- **Rust**: `-C opt-level=3 -C target-cpu=native -C lto=fat`
- **Go**: Build with standard flags, run with `GOGC=off`
- **C#**: Release configuration
- **JavaScript**: Run with `--max-old-space-size=4096` if needed
- **PHP**: Ensure `opcache.jit=tracing` is enabled

## Debugging and Profiling

### C Implementation Debugging
```bash
# Build debug version with sanitizers
gcc -g -O0 -fsanitize=address -DDEBUG wordcount.c -o wordcount_debug

# Profile with perf
perf stat -e cycles,instructions,cache-misses ./wordcount_hopt book.txt

# Generate detailed debug bundle
./bench_c.sh -d --profile --bundle
```

### Validation
All implementations except PHP should produce exactly 928,012 words for `book.txt`. PHP produces 927,930 due to regex boundary differences.

## Performance Targets

- **C Hyperopt**: Target 3.0+ GB/s on large files (currently achieves 4.67 GB/s)
- **Standard implementations**: Within 3x of C reference implementation
- **Consistency**: Standard deviation < 5ms for small files

## Thread Count Optimization

For `wordcount_hyperopt.c`, optimal thread counts vary by file size:
- Small files (5.3MB): 6 threads optimal
- Large files (261MB): 12 threads optimal

Use `--scan-threads` flag to find optimal configuration for your system.