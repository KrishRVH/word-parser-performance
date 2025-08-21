# Word Frequency Counter Performance Benchmark

A comprehensive performance comparison of text processing across C, Rust, Go, C#, JavaScript, and PHP.

## Quick Start

```bash
./bench.sh              # Run benchmark
./bench.sh --validate   # Run with result validation
./bench.sh --runs=10    # Run with 10 iterations per test
```

## Results on AMD Ryzen 9 9950X3D

### Performance Rankings (1.2MB Moby Dick, 50-run average)
| Language | Time | vs Baseline | Consistency |
|----------|------|-------------|-------------|
| **Rust** | 0.001s | baseline | Perfect (no variance) |
| **C** | 0.002s | 2.0x slower | 90% stable, 10% spikes |
| **Go** | 0.010s | 10x slower | Perfect consistency |
| **PHP** | 0.020s | 20x slower | Very consistent |
| **JavaScript** | 0.043s | 43x slower | Moderate variance |
| **C#** | 0.051s | 51x slower | Small variance |

### Scaling Performance (50-run average)
| Language | 1.2MB | 12MB | 60MB | Scaling Factor |
|----------|-------|------|------|----------------|
| **C** | 0.002s | 0.052s | 0.261s | Linear (O(n)) |
| **Rust** | 0.001s | 0.050s | 0.262s | Linear (O(n)) |
| **Go** | 0.010s | 0.070s | 0.348s | Linear (O(n)) |
| **PHP** | 0.020s | 0.123s | 0.610s | Linear (O(n)) |
| **JavaScript** | 0.043s | 0.160s | 0.691s | Linear (O(n)) |
| **C#** | 0.051s | 0.180s | 0.715s | Linear (O(n)) |

## Key Performance Findings

### 🏆 Winners by Category

- **Absolute Speed**: Rust (0.262s for 60MB with zero variance)
- **Consistency**: Rust and Go (perfectly predictable timing)
- **Scaling**: All implementations scale linearly O(n)
- **Surprise**: PHP beats JavaScript/C# thanks to native C functions

### 📊 Performance Insights

1. **Rust edges out C** - Not only matches C's speed but with perfect consistency (no timing spikes)

2. **C has occasional spikes** - 10% of runs spike 10x (0.001s → 0.010s), likely from CPU scheduling

3. **Go's predictability** - Despite GC, shows zero variance with GOGC=off

4. **PHP leverages native code** - `array_count_values()` is implemented in C, beating pure interpreted languages

5. **All scale linearly** - 10x data = ~10x time across all implementations

## Word Count Discrepancies

### ⚠️ Important Discovery

Different implementations count words slightly differently for the same 60MB file:

| Language | Total Words | Unique | Difference | Likely Cause |
|----------|-------------|---------|------------|--------------|
| **C, Rust, JS** | 10,953,200 | 16,956 | Baseline | Byte-level processing |
| **Go** | 10,953,189 | 16,956 | -11 words | Buffer boundary handling? |
| **C#** | 10,952,100 | 16,959 | -1,100 words, +3 unique | Character encoding edge case? |
| **PHP** | 10,925,900 | 16,892 | -27,300 words | Regex `\b` boundaries |

### Why Do Counts Differ?

These differences (0.01% to 0.25%) could be:

1. **Implementation bugs** - Buffer boundaries, EOF handling
2. **Legitimate differences** - What counts as a "word"?
   - Contractions: Is "don't" one word or two?
   - Hyphenated: Is "well-known" one word or two?
   - Apostrophes: Is "'tis" one word or two?

The fact that C, Rust, and JavaScript agree perfectly suggests their byte-level approach is the "reference" implementation. Deviations might be bugs worth investigating.

## Implementation Details

### Approach by Language

| Language | Method | Hash Structure | Buffer Size | Word Definition |
|----------|--------|----------------|-------------|-----------------|
| **C** | Byte-level | Custom FNV-1a table | 8KB | Letters only |
| **Rust** | Byte-level | FNV HashMap | Full file | Letters only |
| **Go** | Byte-level | Built-in map | 64KB | Letters only |
| **JavaScript** | Byte-level | Built-in Map | Full file | Letters only |
| **C#** | Char streaming | Dictionary | 64KB | Letters only |
| **PHP** | Regex extraction | Native array | Full file | `\b[a-z]+\b` |

### Key Optimizations Applied

- **C/Rust**: FNV-1a hash (faster than SipHash for short strings)
- **Go**: Garbage collection disabled (GOGC=off)
- **JavaScript**: Direct buffer processing, no string conversion
- **C#**: Character-by-character processing, no full lowercase conversion
- **PHP**: Native C functions, JIT for large files

## Lessons Learned

### Implementation Quality > Language Choice
The difference between naive and optimized implementations within a language (5-10x) exceeds the difference between languages (2-3x).

### Byte-Level Processing Wins
All fast implementations process bytes/characters directly rather than using string manipulation or regex (except PHP which uses native C regex).

### Memory Allocation Matters
- Pre-allocate data structures (hash tables, buffers)
- Reuse buffers instead of creating new strings
- Process in-place when possible

### Native Functions Are Fast
PHP's `array_count_values()` is implemented in C, making PHP competitive despite being interpreted.

### Perfect Isn't Always Better
C is slightly faster on average but has timing spikes. Rust's consistency might be more valuable for latency-sensitive applications.

## When to Use Each Language

| Language | Best For | Why |
|----------|----------|-----|
| **Rust** | Systems requiring speed AND predictability | Zero variance, matches C performance |
| **C** | Absolute maximum throughput | Fastest average, despite occasional spikes |
| **Go** | Network services, concurrent systems | Good performance, simple code, predictable |
| **PHP** | Web backends with existing infrastructure | Fast enough, especially with native functions |
| **JavaScript** | Node.js services, full-stack web | Acceptable performance, huge ecosystem |
| **C#** | Windows native, enterprise, Unity | Good tooling, but slower on Linux/WSL2 |

## Conclusion

**The 51x performance gap sounds dramatic, but context matters:**

- Even the "slowest" language processes 60MB in 0.7 seconds
- Most applications aren't CPU-bound on text processing
- Developer productivity often outweighs performance differences
- **Implementation approach matters more than language**

**Real-world considerations:**
- Need consistent latency? → Rust
- Need maximum throughput? → C
- Need simple, maintainable code? → Go
- Already using a language? → Optimize within it first

The benchmark reveals that modern hardware is so fast that language performance differences rarely matter for typical workloads. A well-optimized solution in any language beats a naive implementation in a "fast" language.

---

*Benchmark conducted on AMD Ryzen 9 9950X3D (16-core) with 64GB RAM on WSL2 Ubuntu 24.04*

*Note: C# performance is impacted by WSL2 virtualization overhead. Native Windows or Linux performance would be significantly better (likely 2-3x faster).*

*Word count discrepancies between implementations may indicate bugs worth investigating or legitimate differences in word boundary definitions.*