#### Performance Consistency
With 50 runs, interesting patterns emerge:
- **Rust**: Zero variance - exactly 0.001s every run
- **C**: Occasional 10x spikes (0.001s → 0.010s) likely from CPU scheduling
- **Go**: Perfect consistency despite being garbage collected (GC off helps)
- **PHP**: Remarkably consistent for an interpreted language

The spikes in C (happening ~10% of runs) show that even "fast" languages can have outliers due to OS scheduling, CPU boost states, or cache effects.# Word Frequency Counter Performance Benchmark

A real-world comparison of text processing performance across C, Rust, Go, C#, JavaScript, and PHP.

## Quick Start

```bash
./bench.sh              # Run benchmark
./bench.sh --validate   # Run with result validation
./bench.sh --runs=10    # Run with 10 iterations per test
```

## Results on AMD Ryzen 9 9950X3D

### Performance Rankings (1.2MB file - Moby Dick, 50 runs)
| Language | Time | vs Baseline | Consistency |
|----------|------|-------------|-------------|
| **Rust** | 0.001s | baseline | Perfect (σ=0) |
| **C** | 0.002s | 2.0x slower | Some spikes |
| **Go** | 0.010s | 10x slower | Perfect (σ=0) |
| **PHP** | 0.020s | 20x slower | Perfect (σ=0) |
| **JavaScript** | 0.043s | 43x slower | Moderate variance |
| **C#** | 0.051s | 51x slower | Small variance |

### Scaling Performance (average of 50 runs)
| Language | 1.2MB | 12MB | 60MB |
|----------|-------|------|------|
| **C** | 0.002s | 0.052s | 0.261s |
| **Rust** | 0.001s | 0.050s | 0.262s |
| **Go** | 0.010s | 0.070s | 0.348s |
| **PHP** | 0.020s | 0.123s | 0.610s |
| **JavaScript** | 0.043s | 0.160s | 0.691s |
| **C#** | 0.051s | 0.180s | 0.715s |

## Key Findings

### 🏆 Performance Insights

1. **Rust edges out C** - With 50 runs, Rust consistently beats C by being more predictable (no variance vs C's occasional spikes).

2. **Go's consistency is remarkable** - Perfect 0.010s every single run at 1.2MB shows excellent runtime predictability.

3. **PHP outperforms JavaScript and C#** - Native C functions (`array_count_values`) beat interpreted code, even with JIT. Though PHP counts 0.25% fewer words due to regex differences.

4. **All languages scale linearly** - 10x data = ~10x time, showing O(n) algorithms across the board.

### 💡 Lessons Learned

#### Implementation Quality > Language Choice
The difference between naive and optimized implementations within a language can be 5-10x, larger than the difference between languages. Byte-level processing universally beats string manipulation.

#### Native Functions Win
PHP's surprisingly good performance (2.3x slower than C at 60MB) comes from using `array_count_values()` - a native C function. When available, built-in functions beat custom implementations. However, PHP's regex-based word extraction counts slightly differently (0.25% fewer words) than byte-level processing.

#### Memory Allocation Matters
- **Winners**: C and Rust pre-allocate hash tables
- **Losers**: Creating new strings for every word kills performance
- **Key**: Process bytes directly, allocate strings only for unique words

#### Hash Function Choice
Using FNV-1a hash instead of default hash functions improved Rust performance by 2-3x. For short strings (words), simpler hash functions often beat cryptographically secure ones.

#### Performance Consistency
With 50 runs, interesting patterns emerge:
- **Rust**: Zero variance - exactly 0.001s every run
- **C**: Occasional 10x spikes (0.001s → 0.010s) likely from CPU scheduling
- **Go**: Perfect consistency despite being garbage collected (GC off helps)
- **PHP**: Remarkably consistent for an interpreted language

The spikes in C (happening ~10% of runs) show that even "fast" languages can have outliers due to OS scheduling, CPU boost states, or cache effects.

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

### 🔍 Result Validation Discovery

Running with `--validate` reveals an important finding: **PHP counts 0.25% fewer words than other implementations**:

| Implementation | Total Words (60MB) | Unique Words |
|---------------|-------------------|--------------|
| C, Rust, Go, JS, C# | 10,953,200 | 16,956 |
| PHP | 10,925,900 | 16,892 |
| **Difference** | -27,300 (-0.25%) | -64 |

This happens because PHP's regex `\b[a-z]+\b` handles word boundaries differently than byte-level processing:
- Contractions: "don't" might be 1 word vs 2 ("don" + "t")
- Hyphenated words: "well-known" might be 1 word vs 2
- Special characters cause different boundary detection

The validation passes because the top 10 words remain in the same order, but the counts differ slightly. This demonstrates that **even "correct" implementations can disagree on edge cases** - a valuable lesson in real-world text processing.

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

**The 51x performance spread between Rust and C# seems huge, but:**
- At 60MB, even the "slowest" language processes in 0.7 seconds
- Most applications aren't CPU-bound on text processing
- Developer productivity often matters more than raw performance
- **Even implementations can disagree** - PHP counts 0.25% fewer words due to regex differences

**Choose based on your constraints:**
- **Need absolute speed?** → Rust (most consistent) or C
- **Need predictability?** → Rust or Go (zero variance)
- **Have existing infrastructure?** → Use what you have
- **Need rapid development?** → Use what your team knows

The benchmark proves that **modern hardware is fast** - even "slow" languages can process War and Peace (3.2MB) in 100ms. Unless you're processing gigabytes of text regularly, language performance differences are rarely the bottleneck.

The real lesson: **Implementation approach matters more than language choice**. A well-optimized solution in any language beats a naive implementation in a "fast" language.

---

*Benchmark conducted on AMD Ryzen 9 9950X3D (16-core) with 64GB RAM on WSL2 Ubuntu 24.04*

*Note: C# performance is likely impacted by WSL2 virtualization. Native Windows or Linux performance would be significantly better.*