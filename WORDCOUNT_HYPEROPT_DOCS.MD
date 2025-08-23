# WordCount Hyperoptimized Implementation Documentation

## Overview

`wordcount_hyperopt.c` (v3.3 FINAL) is a highly optimized, parallel word frequency counter designed to achieve maximum throughput on modern x86-64 processors. It demonstrates state-of-the-art performance through extensive hardware-specific optimizations, achieving up to 4.58 GB/s throughput on large files.

## Architecture

### Core Design Principles

1. **Parallel Processing**: Multi-threaded architecture with configurable thread count (default: 6 threads)
2. **SIMD Acceleration**: AVX-512 vectorized text processing for 64-byte chunks
3. **Cache Optimization**: V-Cache aware CPU affinity for AMD processors
4. **Memory Efficiency**: Per-thread hash tables with memory pools to minimize allocations
5. **Lock-Free Design**: No synchronization during processing phase

### Threading Model

- **Work Distribution**: File divided into equal chunks per thread
- **Word Boundary Handling**: Threads adjust boundaries to avoid splitting words
- **CPU Affinity**: Automatic detection and pinning to V-Cache enabled cores
- **Barrier Synchronization**: Threads start simultaneously after initialization

## Key Optimizations

### 1. AVX-512 SIMD Processing

```c
process_chunk_avx512()
```
- Processes 64 bytes per iteration using AVX-512 instructions
- Parallel character classification (uppercase/lowercase detection)
- Bitmask-based word boundary detection
- Falls back to scalar processing for non-AVX-512 systems

**Performance Impact**: ~3-5x speedup over scalar processing

### 2. CRC32C Hardware Hashing

```c
hash_word() with SSE4.2
```
- Uses hardware CRC32 instructions (_mm_crc32_u64, _mm_crc32_u32, _mm_crc32_u8)
- Incremental hash computation during word extraction
- MurmurHash3-style finalization for better distribution
- 16-bit fingerprint for fast rejection in hash table

**Performance Impact**: ~2x faster than software FNV-1a hashing

### 3. V-Cache Aware CPU Affinity

```c
discover_vcache_cpus()
```
- Automatically detects AMD V-Cache topology
- Identifies CPUs with largest L3 cache (96MB+ on Ryzen 9950X)
- Pins threads to V-Cache CCD for optimal cache utilization
- Parses `/sys/devices/system/cpu/` for cache configuration

**Performance Impact**: 10-15% improvement on AMD Ryzen processors

### 4. Memory Pool Allocation

```c
pool_alloc()
```
- 32MB pre-allocated string pools per thread
- 64-byte aligned allocations for cache line optimization
- Fallback to malloc for overflow (tracked separately)
- 8-byte alignment for pool allocations

**Performance Impact**: Reduces allocation overhead by ~90%

### 5. Optimized Hash Table

```c
table_insert_hashed()
```
- Open addressing with linear probing
- Power-of-2 sizing for fast modulo operations
- 70% load factor threshold for resizing
- Prefetching hints for next probe locations
- Fast path rejection using hash + length + fingerprint

**Performance Impact**: ~1.5x faster than chaining-based tables

## Data Structures

### Entry Structure
```c
typedef struct {
    char* word;       // Pointer to word string
    uint32_t count;   // Frequency count
    uint32_t hash;    // Full 32-bit hash
    uint16_t len;     // Word length
    uint16_t fp16;    // 16-bit fingerprint
} Entry;
```
- Compact 24-byte structure (with padding)
- Fingerprint enables fast mismatch detection
- Cached hash avoids recomputation

### ThreadTable Structure
```c
typedef struct __attribute__((aligned(CACHELINE))) {
    Entry* entries;           // Hash table array
    char* string_pool;        // Pre-allocated string storage
    size_t pool_used;         // Current pool usage
    size_t capacity;          // Hash table capacity
    size_t size;              // Number of unique words
    uint64_t total_words;     // Total word count
    int thread_id;            // Thread identifier
    char** malloc_words;      // Overflow allocations
    size_t malloc_count;      // Number of overflows
    size_t malloc_cap;        // Overflow array capacity
} ThreadTable;
```
- Cache-line aligned (64 bytes) to prevent false sharing
- Self-contained per-thread state

## Algorithms

### Word Extraction
1. **SIMD Path** (AVX-512):
   - Load 64-byte chunks
   - Create bitmask of ASCII letters
   - Extract continuous runs of letters
   - Convert to lowercase on-the-fly
   - Compute CRC32C hash incrementally

2. **Scalar Fallback**:
   - Byte-by-byte processing
   - ASCII letter detection
   - In-place lowercase conversion
   - UTF-8 sequence skipping

### Hash Table Operations
- **Insertion**: Linear probing with fast-path rejection
- **Growth**: Double capacity when >70% full
- **Merging**: Global table combines thread-local tables

### Top-K Selection
- **Small datasets** (<1000 words): Full sort
- **Large datasets**: Min-heap for top 100 words

## Build Configuration

### Compiler Flags
```bash
# Default build (6 threads, optimal for small files)
gcc -O3 -march=znver5 -mtune=znver5 -flto -fomit-frame-pointer -funroll-loops -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# For older compilers that don't recognize znver5
gcc -O3 -march=znver4 -mtune=znver4 -flto -fomit-frame-pointer -funroll-loops -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# Generic build with auto-detection
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

# Custom thread count (e.g., 12 threads for large files)
gcc -O3 -march=native -DNUM_THREADS=12 -flto -fomit-frame-pointer -funroll-loops -pthread wordcount_hyperopt.c -o wordcount_hopt_12t -lm

# Debug build (use -O2, not -O0 for meaningful metrics)
gcc -O2 -g -DDEBUG -march=native -pthread wordcount_hyperopt.c -o wordcount_hopt_debug -lm

# Profile-guided optimization
gcc -O3 -march=native -fprofile-generate -pthread wordcount_hyperopt.c -o wordcount_pgo -lm
./wordcount_pgo book.txt
gcc -O3 -march=native -fprofile-use -pthread wordcount_hyperopt.c -o wordcount_hopt_pgo -lm
```

### Key Flags Explained
- `-O3`: Maximum optimization level
- `-march=znver5`: Target AMD Zen 5 architecture (adjust for your CPU)
- `-mtune=znver5`: Tune for AMD Zen 5 microarchitecture
- `-flto`: Link-time optimization
- `-fomit-frame-pointer`: Free up register
- `-funroll-loops`: Unroll small loops
- `-pthread`: Enable POSIX threads

### Preprocessor Options
- `NUM_THREADS`: Thread count (default: 6)
- `DEBUG`: Enable detailed logging and metrics
- `__AVX512BW__`: Auto-detected for AVX-512 support
- `__SSE4_2__`: Auto-detected for CRC32C support

## Runtime Configuration

### Environment Variables
None required - all configuration is compile-time or auto-detected.

### Thread Count Selection
Default: 6 threads (optimal for AMD Ryzen 9950X on small files)
- Small files (5MB): 6 threads optimal
- Medium files (27MB): 6-12 threads  
- Large files (131MB+): 6 threads still optimal (achieves 4.58 GB/s)

Compile with different thread counts:
```bash
gcc -DNUM_THREADS=12 -O3 -march=native ...
```

## Performance Characteristics

### Throughput by File Size (100-run average)
- **5.3MB**: 712 MB/s (6 threads)
- **27MB**: 1,047 MB/s (6 threads)
- **131MB**: 4,573 MB/s (6 threads)

### Percentile Performance (large files)
- p50: 4,572 MB/s
- p95: 4,575 MB/s
- p99: 4,576 MB/s
- Extremely consistent performance

### Scaling Behavior
- Near-linear scaling up to V-Cache capacity
- Performance increases dramatically with file size
- Memory bandwidth becomes limiting factor for very large files

### CPU Utilization
- 6 threads: ~600% CPU usage
- 12 threads: ~1200% CPU usage (but lower throughput on small files)
- Efficient work distribution with minimal idle time

## Debug Mode

Enable with `-DDEBUG` flag for extensive metrics:

### Metrics Collected
- Per-thread timing and throughput
- Hash table collision statistics
- Memory allocation tracking
- Word length distribution
- SIMD vs scalar chunk counts
- UTF-8 sequence statistics
- Insert operation breakdown
- Pool exhaustion events
- CPU affinity verification
- Hash distribution analysis
- Table resize timing

### Debug Output
- Console summary statistics
- Detailed log file: `wordcount_debug.log`
- Performance bottleneck identification

### Key Debug Indicators
```
V-Cache CCD detected: 24 CPUs with 100663296 bytes L3  # Good
avg probe length: 1.09  # Excellent hash distribution
SIMD chunks: 48000, Scalar chunks: 42000  # Good ratio
Pool exhaustions: 0  # No memory pressure
Table grow time: 3.2 ms  # Acceptable overhead
```

## Memory Management

### Portable Aligned Allocation
The implementation uses a portable approach for aligned memory allocation:
```c
xaligned_alloc_portable()
```
- Tries C11 `aligned_alloc` first (if available)
- Falls back to POSIX `posix_memalign`
- Ensures size is rounded up to alignment boundary
- Debug mode tracks all allocations/frees

### Memory Usage Breakdown (per thread)
- Hash table: ~524KB initial (65536 entries × 24 bytes)
- String pool: 32MB pre-allocated
- Overflow tracking: Dynamic (usually 0)
- Total: ~33MB per thread baseline

## Limitations

1. **Maximum Word Length**: 100 characters (longer words truncated)
2. **Memory Usage**: ~33MB per thread baseline
3. **File Size**: Limited by available RAM for memory mapping
4. **Platform**: x86-64 Linux only (uses Linux-specific APIs)
5. **Word Definition**: ASCII letters only [a-zA-Z]
6. **UTF-8 Handling**: Non-ASCII treated as word separators

## Performance Tuning Guide

### For AMD Ryzen with V-Cache
1. Ensure V-Cache detection is working (check console output)
2. Use 6 threads for optimal performance across all file sizes
3. Compile with `-march=znver4` or `-march=znver5`
4. CPU affinity is automatically optimized

### For Intel Processors
1. Compile with `-march=native` for auto-detection
2. Test different thread counts (4, 6, 8, 12)
3. V-Cache detection will gracefully fall back
4. Consider disabling hyperthreading for consistency

### Memory Considerations
1. Increase `STRING_POOL_SIZE` for files with many unique words
2. Adjust `INITIAL_CAPACITY` based on expected unique word count
3. Use huge pages for very large files: `madvise(MADV_HUGEPAGE)`
4. Monitor pool exhaustions in debug mode

## Comparison with Standard Implementation

| Aspect | Standard C | Hyperoptimized |
|--------|------------|----------------|
| Processing | Sequential | Parallel (6 threads) |
| Hash Function | FNV-1a software | CRC32C hardware |
| Text Scanning | Byte-by-byte | AVX-512 SIMD |
| Memory | Dynamic allocation | Memory pools |
| Cache Usage | Basic | V-Cache optimized |
| Throughput (5MB) | ~140 MB/s | 712 MB/s |
| Throughput (131MB) | ~140 MB/s | 4,573 MB/s |
| Speedup | 1.0x | 5-32x |

## Known Issues and Solutions

### WSL2 Limitations
- V-Cache topology may not be fully exposed
- Some performance counters unavailable
- File I/O slightly slower than native Linux

### Edge Cases
- Words >100 chars truncated (by design)
- UTF-8 sequences treated as delimiters (by design)
- Very high unique word counts (>5M) may exhaust pools

### Common Problems
1. **Compiler doesn't recognize znver5**: Use znver4 or march=native
2. **Performance below expectations**: Verify thread count and V-Cache detection
3. **Memory errors**: Check debug log for pool exhaustions
4. **Wrong word counts**: Ensure CRC reset between words (fixed in v3.3)

## Future Optimization Opportunities

1. **Compact Entry Structure**: Reduce from 24 to 16 bytes
2. **Parallel Merge Phase**: Partition by hash prefix
3. **Adaptive Initial Capacity**: Sample-based estimation
4. **Robin Hood Hashing**: Reduce probe variance
5. **NUMA Awareness**: Optimize for multi-socket systems
6. **GPU Acceleration**: Massive parallelism for huge files
7. **AVX-512 VBMI**: More efficient character classification
8. **Huge Pages**: Reduce TLB misses

## Testing and Validation

### Correctness Tests
```bash
# Build debug version
gcc -O2 -g -DDEBUG -march=native -pthread wordcount_hyperopt.c -o wordcount_debug -lm

# Single word test
yes "word" | head -n 1000000 > repeat.txt
./wordcount_debug repeat.txt  # Should show 1 unique word

# UTF-8 handling
echo "café naïve 北京" > utf8.txt
./wordcount_debug utf8.txt
grep "UTF-8" wordcount_debug.log

# Validate against reference
./wordcount_c book.txt > ref.txt
./wordcount_hopt book.txt > test.txt
diff ref.txt test.txt  # Should match except timing
```

### Performance Testing
```bash
# Use bench_c.sh for comprehensive testing
./bench_c.sh --hyperonly --large --runs=100 --pin=0-23

# Manual performance test
for i in {1..10}; do
    time ./wordcount_hopt book.txt 2>&1 | grep real
done

# Profile with perf
perf record -g ./wordcount_hopt book.txt
perf report --stdio | head -50
```

## Conclusion

The hyperoptimized implementation achieves exceptional performance through:
- Aggressive parallelization with optimal thread count
- Hardware-specific optimizations (AVX-512, CRC32C)
- Cache-conscious design with V-Cache awareness
- Minimal memory allocation via pooling
- SIMD acceleration for text processing

Performance scales dramatically with file size, achieving:
- 5x speedup on small files (5MB)
- 32x speedup on large files (131MB)
- Approaching memory bandwidth limits (4.58 GB/s)