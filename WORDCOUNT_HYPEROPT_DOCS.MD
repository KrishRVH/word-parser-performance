# WORDCOUNT_HYPEROPT_DOCS.MD - Hyperoptimized Word Counter for AMD Ryzen 9 9950X3D

## Hardware
- AMD Ryzen 9 9950X3D: 16 cores, 32 threads, Zen 5
- 144MB L3 total: CCD0 has 32MB, CCD1 has 128MB (3D V-Cache)
- 64GB DDR5 RAM
- Intel Optane P5800x SSD
- RTX 4090 (unused)
- WSL2 Ubuntu on Windows 11

## Workload
- 5-250MB UTF-8 text files (novels)
- ~15% non-ASCII characters (smart quotes, em dashes, accents)
- Heavy word frequency skew (the, a, and = ~10% of total)
- 10-50K unique words from 500K-50M total

## Build Commands

```bash
# Production - maximum speed (use znver4 if compiler doesn't support znver5)
gcc -O3 -march=znver4 -mtune=znver4 -mavx512f -mavx512bw -mavx512vl \
    -msse4.2 -flto -fomit-frame-pointer -funroll-loops -pthread \
    wordcount_hyperopt.c -o wordcount -lm

# Alternative: Native architecture detection
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer \
    -funroll-loops -pthread wordcount_hyperopt.c -o wordcount -lm

# Try 256-bit vectors (sometimes faster on Zen 5)
gcc -O3 -march=znver4 -mprefer-vector-width=256 -msse4.2 -flto \
    -fomit-frame-pointer -funroll-loops -pthread \
    wordcount_hyperopt.c -o wordcount_256 -lm

# Debug build - IMPORTANT: Use -O2 for meaningful metrics, not -O0
gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=native -pthread \
    wordcount_hyperopt.c -o wordcount_debug -lm

# Profile-guided optimization (10-20% boost)
gcc -O3 -march=znver4 -fprofile-generate -pthread wordcount_hyperopt.c -o wordcount_pgo -lm
./wordcount_pgo typical_file.txt
gcc -O3 -march=znver4 -fprofile-use -pthread wordcount_hyperopt.c -o wordcount_final -lm
```

## Current Implementation

### Key Design Decisions
- **IMPORTANT**: 8 threads optimal on 9950X3D (matches V-Cache CCD physical cores)
  - 16 threads: ~497 MB/s throughput
  - 8 threads: ~715 MB/s throughput (44% faster!)
- Threads pinned to physical cores (no SMT to avoid cache thrashing)
- Open addressing hash table with linear probing
- Dynamic resize at 70% load factor
- Per-thread 32MB string pools with malloc fallback tracking
- AVX-512 run extraction using bit manipulation
- CRC32C hardware hashing with full fmix64 finalization
- Prehashed insertion (hash computed while extracting)
- Min-heap for top-K extraction when unique count > 1000
- V-Cache CCD detection by L3 size (128MB vs 32MB)

### Critical Code Paths

1. **AVX-512 tokenization** (`process_chunk_avx512`)
   - Processes 64 bytes at once
   - Uses `_mm512_cmplt_epu8_mask` for ASCII detection
   - Extracts letter runs with `__builtin_ctzll`
   - Computes CRC32C while building words
   - Falls back to scalar for UTF-8 sequences

2. **Hash table insertion** (`table_insert_hashed`)
   - Linear probing with stored hash values
   - 16-bit fingerprints for fast rejection
   - Prefetches next probe locations
   - Automatic resize at 70% load

3. **Merge phase** (`merge_tables`)
   - Single-threaded (bottleneck on large files)
   - Power-of-2 global capacity
   - Uses fingerprints to avoid most memcmps

## Debug Metrics

Run with `-DDEBUG` build and check `wordcount_debug.log`:

```bash
# Build debug version
gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=native -pthread \
    wordcount_hyperopt.c -o wordcount_debug -lm

# Run and generate log
./wordcount_debug book.txt
cat wordcount_debug.log
```

### Debug Log Contents

The debug log provides detailed performance breakdown:

```
[debug_init:63] Debug initialized - PID: 15538
[discover_vcache_cpus:226] V-Cache CCD detected: 24 CPUs with 100663296 bytes L3
[main:930] mmap: 0.001 ms
[main:958] init: 4.319 ms
[main:990] processing: 15.702 ms  # Main work phase
[main:995] merge: 3.167 ms        # Single-threaded bottleneck
[main:1035] top-k: 0.206 ms
```

### Critical Indicators

**V-Cache CCD detection:**
```
V-Cache CCD detected: 8 CPUs with 134217728 bytes L3
```
If it shows 33554432 (32MB), threads are on the wrong CCD.

**Hash table health:**
```
avg_probe=1.8, max_probe=25    # Good
avg_probe=10+, max_probe=100+  # Bad - hash clustering
```

**SIMD efficiency:**
```
simd_chunks=48000 scalar_chunks=42000  # Normal (~1.1:1 ratio expected)
simd_chunks=5000 scalar_chunks=45000   # Bad (SIMD not engaging)
```
Note: scalar_chunks increments for any 64B block containing letters (by design)

**Memory:**
```
Pool exhaustions: 0           # Good
Pool exhaustions: 1000+       # Need bigger pools
Memory leaked: 0              # Must be 0
```

## Known Issues

### WSL2 Limitations
- V-Cache topology may not be exposed (falls back to sequential CPU IDs)
- `MAP_POPULATE` can increase startup time
- Some perf counters unavailable

### Edge Cases
- Words > 100 chars are truncated
- UTF-8 sequences treated as delimiters (by design)
- Very high unique word counts (>5M) may exhaust pools

## Next Optimizations

### 1. Compact Entry (10-20% speedup)
Current Entry is 24 bytes. Can reduce to 16:
```c
typedef struct {
    uint32_t pool_offset;  // Not pointer
    uint32_t count;
    uint32_t hash;
    uint16_t len;
    uint16_t fp16;
} CompactEntry;  // 16 bytes
```
Reduces memory bandwidth by 33%.

### 2. Parallel Merge (10-15% on large files)
Currently single-threaded. Could partition by hash prefix:
```c
for (int shard = 0; shard < 16; shard++) {
    pthread_create(&merger[shard], ...);
}
```

### 3. Better Initial Capacity
Currently uses fixed heuristics. Could sample first 1MB:
```c
size_t sample_unique = quick_count(data, 1<<20);
capacity = next_pow2(sample_unique * file_size / (1<<20) * 1.5);
```

### 4. Robin Hood Hashing
Would reduce probe variance and worst-case behavior.

## Benchmarking Tools

### bench_c.sh - C Implementation Comparison

A dedicated script for comparing C implementations:

```bash
# Basic benchmark
./bench_c.sh

# Debug mode with detailed logs
./bench_c.sh -d

# Test with different thread counts
./bench_c.sh --threads=8

# Profile with perf
./bench_c.sh --profile

# Test 256-bit vectors
./bench_c.sh --256bit

# Validate outputs match
./bench_c.sh --validate

# Custom run count
./bench_c.sh --runs=10

# Skip cleanup prompt
./bench_c.sh --no-cleanup
```

The script compares:
- wordcount.c (reference implementation)
- wordcount_hyperopt.c with various optimizations
- Debug builds with full instrumentation
- PGO (Profile-Guided Optimization) builds

### bench.sh - Full Language Comparison

Compares all language implementations including the hyperopt version:

```bash
# Run full benchmark
./bench.sh

# Validate all implementations match
./bench.sh --validate

# Custom iteration count
./bench.sh --runs=10
```

## Testing Checklist

### Correctness
```bash
# Single word
yes "word" | head -n 1000000 > repeat.txt
./wordcount repeat.txt  # Should show 1 unique

# UTF-8 handling
echo "café naïve 北京" > utf8.txt
./wordcount_debug utf8.txt
grep "UTF-8" wordcount_debug.log

# Thread boundaries
# Create file with long word spanning chunks
```

### Performance
```bash
# Warm cache
cat test.txt > /dev/null

# Multiple runs, take minimum
for i in {1..5}; do
    time ./wordcount test.txt 2>&1 | grep real
done

# Profile
perf record -g ./wordcount test.txt
perf report --stdio | head -50

# Cache behavior
perf stat -e cache-misses,cache-references,LLC-loads,LLC-load-misses ./wordcount test.txt
```

## Performance Results

### Current Performance (5.3MB test file)
- **Reference C implementation**: 260 MB/s
- **Hyperopt with 16 threads**: 497-520 MB/s
- **Hyperopt with 8 threads**: 715 MB/s (optimal)
- **Target**: 3.0 GB/s
- **Current achievement**: 24% of target

### Performance Breakdown
- Processing phase: 83% of time (15.7ms)
- Merge phase: 17% of time (3.2ms)
- Hash table health: Excellent (avg probe 1.09)
- SIMD utilization: Good (48K SIMD vs 42K scalar chunks)

## Key Invariants

1. Hash table capacity is always power of 2
2. Words are lowercase ASCII after extraction
3. Thread chunks never split mid-word
4. Total words = sum of all entry counts
5. Pool fallback mallocs are tracked and freed
6. CRC is reset after every word flush

## Troubleshooting

### Common Issues and Solutions

1. **Compiler doesn't recognize znver5**
   - Solution: Use `znver4` or `march=native` instead
   - The Zen 5 architecture may not be supported in older GCC versions

2. **Performance below expectations (~500 MB/s instead of 3 GB/s)**
   - Try 8 threads instead of 16: `gcc -DNUM_THREADS=8 ...`
   - 8 threads optimal for V-Cache CCD on 9950X3D
   - Check debug log for thread imbalance

3. **"free(): invalid pointer" error**
   - Run with AddressSanitizer: `gcc -fsanitize=address ...`
   - Check debug log for memory statistics
   - May indicate pool exhaustion or buffer overflow

4. **Debug builds with -O0 show no SIMD usage**
   - Always use `-O2 -g -DDEBUG` for debug builds, not `-O0`
   - `-O0` disables vectorization entirely

5. **MAP_POPULATE causing slow startup**
   - Remove MAP_POPULATE flag from mmap call
   - Already fixed in current version

6. **NUM_THREADS redefinition warning**
   - Fixed with `#ifndef NUM_THREADS` guard
   - Can override with `-DNUM_THREADS=N` at compile time

## Failure Modes

If the code crashes or produces wrong results, check:

1. **Wrong word counts**: CRC not reset between words (fixed in v3.3)
2. **Segfault**: Pool exhaustion, hash table truly full, or ctzll(0)
3. **Memory leak**: Pool fallback mallocs not tracked (check debug log)
4. **Slow performance**: Wrong CCD, hash clustering, or excessive UTF-8
5. **Thread imbalance**: Poor chunk boundaries or skewed word distribution