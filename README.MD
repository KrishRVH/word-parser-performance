# Word Frequency Counter Performance Benchmark

A real-world comparison of text processing performance across C, Rust, Go, C#, JavaScript, and PHP.

## Quick Start

```bash
./bench.sh              # Run benchmark
./bench.sh --validate   # Run with result validation
./bench.sh --runs=10    # Run with 10 iterations per test
```

## Results on AMD Ryzen 9 9950X3D

### Performance Rankings (1.2MB file - Moby Dick)
| Language | Time | vs Baseline |
|----------|------|-------------|
| **C** | 0.001s | baseline |
| **Rust** | 0.001s | 1.0x |
| **Go** | 0.010s | 10x slower |
| **PHP** | 0.013s | 13x slower |
| **JavaScript** | 0.040s | 40x slower |
| **C#** | 0.050s | 50x slower |

### Scaling Performance
| Language | 1.2MB | 12MB | 60MB |
|----------|-------|------|------|
| **C** | 0.001s | 0.057s | 0.260s |
| **Rust** | 0.001s | 0.050s | 0.260s |
| **Go** | 0.010s | 0.070s | 0.360s |
| **PHP** | 0.013s | 0.130s | 0.613s |
| **JavaScript** | 0.040s | 0.160s | 0.673s |
| **C#** | 0.050s | 0.180s | 0.707s |

## Key Findings

### 🏆 Performance Insights

1. **Rust matches C performance** - Both achieve ~260ms for 60MB files through byte-level processing and efficient hash tables.

2. **Go scales best among managed languages** - Only 1.4x slower than C at 60MB despite being garbage collected.

3. **PHP outperforms JavaScript and C#** - Native C functions (`array_count_values`) beat interpreted code, even with JIT.

4. **All languages scale linearly** - 10x data = ~10x time, showing O(n) algorithms across the board.

### 💡 Lessons Learned

#### Implementation Quality > Language Choice
The difference between naive and optimized implementations within a language can be 5-10x, larger than the difference between languages. Byte-level processing universally beats string manipulation.

#### Native Functions Win
PHP's surprisingly good performance (2.4x slower than C at 60MB) comes from using `array_count_values()` - a native C function. When available, built-in functions beat custom implementations.

#### Memory Allocation Matters
- **Winners**: C and Rust pre-allocate hash tables
- **Losers**: Creating new strings for every word kills performance
- **Key**: Process bytes directly, allocate strings only for unique words

#### Hash Function Choice
Using FNV-1a hash instead of default hash functions improved Rust performance by 2-3x. For short strings (words), simpler hash functions often beat cryptographically secure ones.

#### Startup Overhead
At 1.2MB, startup time dominates:
- Compiled languages: ~1ms startup
- JIT languages: 40-50ms startup
- At 60MB, the gap narrows as processing time dominates

### 📊 When to Use Each Language

| Language | Best For | Why |
|----------|----------|-----|
| **C** | Systems programming, embedded | Absolute control, minimal overhead |
| **Rust** | Safety-critical systems | C-level performance with memory safety |
| **Go** | Network services, CLI tools | Great performance with simple code |
| **PHP** | Web backends, scripting | Fast enough with existing infrastructure |
| **JavaScript** | Full-stack web, Node services | Acceptable performance, huge ecosystem |
| **C#** | Enterprise, Windows, games | Good tooling, decent performance |

### 🔍 Validation Surprise

Running with `--validate` revealed that PHP counts words slightly differently:
- PHP: "a" appears 4,741 times
- Others: "a" appears 4,747 times

This 0.1% difference comes from regex word boundary handling (`\b`) vs byte-level processing, showing that **implementation details affect correctness, not just performance**.

## Technical Details

### What We're Measuring
- Reading a text file
- Extracting and counting word frequencies  
- Sorting by frequency
- Writing results

### How Implementations Differ
| Language | Approach | Hash Function | Buffer Size |
|----------|----------|---------------|-------------|
| C | Byte-level, custom hash table | FNV-1a | 8KB |
| Rust | Byte-level, HashMap | FNV-1a | Full file |
| Go | Byte-level, built-in map | Built-in | 64KB |
| PHP | Regex + native functions | N/A (array) | Full file |
| JavaScript | Byte-level, Map | Built-in | Full file |
| C# | Character streaming | Built-in | 64KB |

## Conclusion

**The 50x performance spread between C and C# seems huge, but:**
- At 60MB, even the "slowest" language processes in 0.7 seconds
- Most applications aren't CPU-bound on text processing
- Developer productivity often matters more than raw performance

**Choose based on your constraints:**
- **Need absolute speed?** → C or Rust
- **Need safety and speed?** → Rust or Go  
- **Have existing infrastructure?** → Use what you have
- **Need rapid development?** → Use what your team knows

The benchmark proves that **modern hardware is fast** - even "slow" languages can process War and Peace (3.2MB) in 100ms. Unless you're processing gigabytes of text regularly, language performance differences are rarely the bottleneck.

---

*Benchmark conducted on AMD Ryzen 9 9950X3D (16-core) with 64GB RAM on WSL2 Ubuntu 24.04*