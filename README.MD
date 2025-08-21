# Word Frequency Counter - Language Performance Benchmark

Real-world performance comparison of C, Rust, Go, C#, JavaScript, and PHP on text processing.

## Quick Start

```bash
# Install dependencies
./install-deps.sh
source ~/.zshrc  # or ~/.bashrc

# Run benchmark
./bench.sh
```

## Actual Results on AMD Ryzen 9 9950X3D

### Performance Rankings (1.2MB file)
| Rank | Language | Time | vs C |
|------|----------|------|------|
| 1 | **C** | 0.001s | baseline |
| 2 | **Go** | 0.010s | 10x slower |
| 3 | **Rust** | 0.010s | 10x slower |
| 4 | **PHP** | 0.013s | 13x slower |
| 5 | **C# (.NET)** | 0.040s | 40x slower |
| 6 | **JavaScript** | 0.040s | 40x slower |

### Scaling Performance (Time in seconds)
| Language | 1.2MB | 12MB | 60MB |
|----------|-------|------|------|
| **C** | 0.001 | 0.053 | 0.273 |
| **Go** | 0.010 | 0.070 | 0.343 |
| **Rust** | 0.010 | 0.110 | 0.570 |
| **PHP** | 0.013 | 0.123 | 0.593 |
| **C# (.NET)** | 0.040 | 0.170 | 0.647 |
| **JavaScript** | 0.040 | 0.157 | 0.670 |

## Key Findings

### 🏆 Winners by Category

**Absolute Performance**: C (273ms for 60MB)
- 10x faster startup than any other language
- Scales linearly with file size
- Minimal memory overhead

**Best Compiled Alternative**: Go (343ms for 60MB)
- Only 1.25x slower than C at scale
- Excellent garbage collector
- Simple, readable code

**Surprise Performance**: PHP (593ms for 60MB)
- Outperforms JavaScript and C#
- `array_count_values()` is native C code
- PHP 8.3's JIT compiler helps significantly

### 🔥 Performance Insights

1. **C dominates** at all file sizes with hand-tuned hash table
2. **Go scales best** - stays close to C performance as files grow
3. **PHP beats JavaScript** thanks to native C functions
4. **Rust has overhead** but remains predictable
5. **C# and JavaScript** perform similarly at scale

## Implementation Optimizations Applied

### C
- Custom FNV-1a hash table
- Power-of-2 bucket size for fast modulo
- Single-pass processing with 8KB buffer

### Go
- Byte-level processing (no regex)
- Pre-allocated map
- 64KB read buffer
- Direct byte comparisons

### Rust
- HashMap with pre-allocated capacity
- Zero-copy where possible
- `split_whitespace()` instead of regex

### JavaScript
- Byte-level processing like C/Go
- Direct buffer operations
- No intermediate arrays

### C#
- Character-by-character processing
- StreamReader with 64KB buffer
- Pre-sized dictionary

### PHP
- Uses `array_count_values()` - native C function
- Built-in `arsort()` for efficient sorting
- Essentially calling optimized C code

## System Specifications

- **CPU**: AMD Ryzen 9 9950X3D 16-Core Processor
- **OS**: WSL2 Ubuntu 24.04
- **Compilers**:
  - GCC 13.3.0
  - Rust 1.89.0
  - Go 1.24.5
  - .NET 8.0.119
  - Node.js 20.19.4
  - PHP 8.3.6

## Practical Takeaways

### When to Use Each Language

**C**: When every millisecond counts
- High-frequency trading systems
- Embedded systems
- Video game engines
- Operating systems

**Go**: Best balance of performance and productivity
- Web services
- Cloud infrastructure
- Network tools
- Concurrent systems

**Rust**: When safety and performance both matter
- Systems programming
- WebAssembly
- Cryptocurrency/blockchain
- Security-critical applications

**PHP**: Surprisingly viable for text processing
- Web applications
- Content management
- Quick scripts
- When you have existing PHP infrastructure

**JavaScript**: Good enough for most web scenarios
- Full-stack applications
- Real-time with optimization
- When team knows JS well
- Electron desktop apps

**C#**: Enterprise applications
- Windows development
- Game development (Unity)
- Enterprise backends
- Cross-platform with .NET Core

## The Bottom Line

- **10x-40x performance difference** between C and interpreted languages
- **Implementation quality matters** - good algorithms beat language choice
- **Native functions win** - PHP's C functions are extremely fast
- **Modern CPUs are fast** - even "slow" languages process 60MB in under a second

## Files

- `bench.sh` - Automated benchmark runner
- `install-deps.sh` - Dependency installer for WSL2/Ubuntu
- `wordcount.c` - C implementation
- `wordcount.go` - Go implementation  
- `wordcount.rs` - Rust implementation
- `WordCount.cs` - C# implementation
- `wordcount.js` - JavaScript implementation
- `wordcount.php` - PHP implementation

## Contributing

Run benchmarks on your hardware and share results! Different CPUs show different patterns:
- Intel vs AMD
- Laptop vs Desktop
- Linux vs macOS vs Windows native

---

*Benchmark conducted on AMD Ryzen 9 9950X3D - one of the fastest consumer CPUs available (2024)*