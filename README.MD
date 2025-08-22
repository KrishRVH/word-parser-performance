# Word Frequency Counter Performance Benchmark

A comprehensive performance comparison of text processing across C, Rust, Go, C#, JavaScript, and PHP.

## Quick Start

```bash
# Install dependencies (Ubuntu/WSL2)
./install-deps.sh

# Run benchmark
./bench.sh              # Run benchmark only
./bench.sh --validate   # Run with result validation
./bench.sh --runs=10    # Run with 10 iterations per test
```

## Performance Results

### Rankings (5.3MB test file, ~900K words, 200 runs average)
| Rank | Language | Time | vs Baseline | Word Count | Match |
|------|----------|------|-------------|------------|-------|
| 1 | **Rust** | 0.020s | baseline | 928,012 | Reference |
| 2 | **C** | 0.022s | 1.1x slower | 928,012 | Exact |
| 3 | **Go** | 0.030s | 1.5x slower | 928,012 | Exact |
| 4 | **PHP** | 0.051s | 2.5x slower | 927,930 | -82 words¹ |
| 5 | **C#** | 0.090s | 4.5x slower | 928,012 | Exact |
| 6 | **JavaScript** | 0.090s | 4.5x slower | 928,012 | Exact |

¹ PHP uses regex with `\b` word boundaries

### Performance Scaling Across File Sizes (200 runs average)
| Language | 5.3MB | 53MB | 261MB | Scaling |
|----------|-------|------|-------|---------|
| **Rust** | 0.020s | 0.221s | 1.176s | Linear |
| **C** | 0.022s | 0.225s | 1.169s | Linear |
| **Go** | 0.030s | 0.296s | 1.605s | Linear |
| **PHP** | 0.051s | 0.561s | 2.694s | Linear |
| **C#** | 0.090s | 0.517s | 2.290s | Linear |
| **JavaScript** | 0.090s | 0.641s | 3.092s | Linear |

### Key Findings

- **Rust leads consistently** across all file sizes (20ms for 5.3MB)
- **C competitive** with Rust, slightly faster at 261MB scale
- **Go maintains 1.5x overhead** consistently
- **PHP scales well** despite regex overhead (2.5x slower)
- **C# and JavaScript tied** at 90ms for 5.3MB
- **All implementations scale linearly** with file size
- **4.5x performance spread** between fastest and slowest

### Performance Consistency Analysis (200 runs)

| Language | Consistency | Standard Deviation | Notes |
|----------|-------------|-------------------|--------|
| **Rust** | Excellent | ~0.000s | Perfect consistency, no variance |
| **Go** | Excellent | ~0.000s | Extremely predictable performance |
| **JavaScript** | Excellent | ~0.000s | V8 JIT highly consistent after warmup |
| **C#** | Very Good | ~0.002s | 98% runs identical, rare JIT spikes |
| **C** | Good | ~0.004s | 85% consistent, OS scheduling affects |
| **PHP** | Moderate | ~0.005s | Most variable, interpreter overhead |

#### Consistency Insights

- **Compiled languages (Rust, Go)** demonstrate best consistency
- **JIT languages (JavaScript, C#)** achieve excellent consistency after warmup
- **Rust wins on both speed AND predictability** - ideal for latency-sensitive applications
- **C shows surprising variance** despite being native code (OS scheduling effects)
- **PHP has highest variance** due to interpreted nature

### Platform Performance Notes

- **C#/.NET 8**: Performs identically on Windows and Linux (~90ms)
- **No platform advantage**: Modern .NET has achieved platform parity
- **WSL2 overhead**: Minimal (~5%), essentially native Linux performance

## Implementation Approaches

| Language | Method | Word Definition | Optimizations |
|----------|--------|-----------------|---------------|
| **C** | Byte-level processing | ASCII letters only | FNV-1a hash, custom hash table |
| **Rust** | Byte-level processing | ASCII letters only | FNV HashMap, zero-copy strings |
| **Go** | Byte-level processing | ASCII letters only | 64KB buffer, GOGC=off |
| **JavaScript** | Byte-level processing | ASCII letters only | Direct buffer processing |
| **C#** | Byte-level processing | ASCII letters only | Pre-sized dictionary |
| **PHP** | Regex extraction | `\b[a-z]+\b` pattern | Native C regex, array_count_values |

## Validation

All implementations except PHP produce identical results:

- **928,012 words**: C, Rust, Go, JavaScript, C#
- **927,930 words**: PHP (-82 due to regex boundary handling)

Word definitions:
- Byte-level: ASCII letters [a-zA-Z]
- Regex: PCRE word boundaries


## Technical Details

### Algorithm Complexity
| Language | Time | Space | File Loading |
|----------|------|-------|--------------|
| C | O(n) | O(unique words) | Streaming with 8KB buffer |
| Rust | O(n) | O(n + unique words) | Full file into memory |
| Go | O(n) | O(unique words) | Streaming with 64KB buffer |
| JavaScript | O(n) | O(n + unique words) | Full file into memory |
| C# | O(n) | O(n + unique words) | Full file into memory |
| PHP | O(n) | O(n + unique words) | Full file into memory |

## Use Cases

| Language | Best For |
|----------|----------|
| **C/Rust** | Systems programming, maximum performance |
| **Go** | Network services, concurrent processing |
| **PHP** | Web backends, existing PHP infrastructure |
| **JavaScript** | Node.js services, full-stack JavaScript |
| **C#** | Windows development, enterprise applications |

## Building and Running

### Prerequisites
```bash
# Ubuntu/WSL2
./install-deps.sh
```

### Running Tests
```bash
# Compile
gcc -O3 -march=native wordcount.c -o wordcount_c
rustc -O wordcount.rs -o wordcount_rust
go build -o wordcount_go wordcount.go
dotnet build -c Release

# Run
./wordcount_c book.txt
./wordcount_rust book.txt
GOGC=off ./wordcount_go book.txt
./bin/Release/net8.0/WordCount book.txt
node wordcount.js book.txt
php wordcount.php book.txt
```

### Optimization Flags

- **C**: `-O3 -march=native -flto`
- **Rust**: `-C opt-level=3 -C target-cpu=native -C lto=fat`
- **Go**: `-ldflags="-s -w"` with `GOGC=off`
- **C#**: Release mode
- **JavaScript**: `--max-old-space-size=4096`
- **PHP**: `opcache.jit=tracing`

## Key Points

- Implementation quality matters more than language choice
- All implementations use byte-level processing except PHP (regex)
- Word definition: Byte-level vs regex boundaries affects counts
- All implementations are production-optimized

## Validation

The `--validate` flag:
- Uses C implementation as reference
- Compares word counts across implementations
- Verifies top 10 word consistency
- Reports exact differences

## Contributing

Potential improvements:
- Additional languages (Java, Python, Ruby)
- Parallel processing implementations
- Memory usage profiling
- Different architectures (ARM, Apple Silicon)

## License

MIT License - See LICENSE file for details

---

## Test Methodology

- **200 runs per test** for statistical accuracy
- **3 file sizes tested**: 5.3MB, 53MB, 261MB
- **Validation mode** ensures all implementations produce identical results
- **Benchmark system**: AMD Ryzen 9 9950X3D, 64GB RAM, WSL2 Ubuntu 24.04
- **Compiler flags**: Full optimizations enabled for all languages