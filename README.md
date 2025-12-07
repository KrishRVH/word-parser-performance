# Word Frequency Counter Performance Benchmark

A comprehensive performance comparison of text processing across C, Rust, Go, C#, JavaScript, and PHP.

## Quick Start

```bash
# Install dependencies (Ubuntu/WSL2)
./install-deps.sh

bench_c.sh (release):
./bench_c.sh --hyperonly --large --runs=10 --pin=0-23

bench_c.sh (full debug + perf + bundle):
./bench_c.sh -d --hyperonly --large --profile --events="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses" --pin=0-23 --runs=3 --bundle

bench.sh (release):
taskset -c 0-23 ./bench.sh --runs=10

bench.sh (debug/validation):
taskset -c 0-23 ./bench.sh --validate --runs=3
```

## Benchmark Word Definition and Validation Spec

This benchmark does **not** use `wc -w`'s idea of a "word".
All implementations are required to follow the definition below.

### 1. Input and Encoding Assumptions

- Input files are treated as **raw byte streams**.
- The benchmark makes **no attempt** to interpret character encodings or locales.
- All logic is defined in terms of **single bytes**, not Unicode scalar values.

Implementations may *internally* treat the input as UTF‑8 for convenience, but the word definition itself is **ASCII‑only and byte-based**.

---

### 2. Word Definition (Benchmark Spec)

A **word** is defined as a **maximal run of ASCII letters**. Formally:

- Let `is_letter(b)` be:

  ```text
  is_letter(b) := ('A' <= b <= 'Z') OR ('a' <= b <= 'z')
  ```

  where `b` is a single byte (0–255), **not** a Unicode code point.

- Scan the byte stream from start to end:

  - Any contiguous sequence of bytes where `is_letter(b)` is true forms **one word**.
  - Any byte where `is_letter(b)` is false (digits, punctuation, whitespace, non‑ASCII, etc.) acts as a **separator** and is **not part of any word**.

Examples:

- `"foo-bar"` → words: `"foo"`, `"bar"` (the `-` is a separator)
- `"123abc456"` → word: `"abc"` (digits are separators)
- `"it's"` → words: `"it"`, `"s"` (apostrophe is a separator)
- Bytes ≥ 0x80 (non‑ASCII) are **always separators**.

---

### 3. Case Handling and Canonical Form

Words are treated **case-insensitively**:

- Each word is lowercased **per byte** for ASCII letters:

  ```text
  if ('A' <= b <= 'Z') then b := b + 32
  ```

- `'Apple'`, `'APPLE'`, and `'apple'` are all the same word `"apple"`.

Non‑ASCII bytes (≥ 0x80) are never part of words, so they are never lowercased.

---

### 4. Maximum Word Length and Truncation

Implementations are allowed to impose an internal maximum on stored word length (e.g. 64 or 100 bytes) for performance reasons, with the following constraint:

- **Counting semantics are unaffected by truncation.**

Concretely:

- When scanning a run of letters, the implementation may:
  - Store at most `MAX_WORD_LEN` bytes of the word for hashing / comparison.
  - **Skip the remaining letters of that run**, but **must still count exactly one word** for that entire run.
- The remainder of an overlong word **must not** be treated as a separate word.

So a 200‑byte ASCII letter run is **1 word**, not 2 or more, regardless of any internal truncation.

For the provided implementations:

- C reference, C hyperopt, Rust, Go, JS, C# all satisfy this property for the benchmark corpus.

---

### 5. Counts and Statistics

For each input file, every implementation must compute:

- **Total words**
  = total number of word tokens encountered in the file, as defined above.

- **Unique words**
  = number of distinct lowercased word strings.

Sorting / presentation:

- When listing "top N" words, entries are sorted by:
  1. **Descending count**, then
  2. **Ascending lexicographic order** of the lowercased ASCII word.

Percentages are computed as:

```text
percentage(word) = (count(word) * 100.0) / total_words
```

---

### 6. Validation Rules (Cross-Language Consistency)

The **C reference implementation** (`wordcount_c`) is the canonical implementation for this spec.

Validation proceeds as follows:

1. Run the C reference on a given file, capturing:
   - `Total words: T_ref`
   - `Unique words: U_ref`

2. For each other implementation (`C hyperopt`, `Rust`, `Go`, `C#`, `JavaScript`, `PHP`, etc.):

   - Run on the same file and extract:
     - `Total words: T_impl`
     - `Unique words: U_impl`

   - An implementation is considered a **correct match** if:

     ```text
     T_impl == T_ref AND U_impl == U_ref
     ```

3. Additionally, for each implementation that matches `(T_ref, U_ref)`, we verify the **top 10** words:

   - Extract the top 10 `(word, count)` pairs as written in that implementation's `*_results.txt`.
   - Normalize (strip commas, etc.) and sort into a canonical representation.
   - All normalized top‑10 lists must be **bit‑for‑bit identical** across implementations that matched `(T_ref, U_ref)`.

PHP is currently known to differ slightly in counts due to its use of a regex word definition (`\b[a-z]+\b`) and is treated as non-reference in validation.

---

### 7. Relationship to `wc -w`

The GNU `wc -w` tool uses a *different* definition:

- A word is a sequence of **non-whitespace characters** delimited by whitespace, not by non-letters.
- It does **not** split on punctuation or digits; `'foo-bar'`, `'123abc'`, and `"it's"` each count as a single word.
- Behavior is locale‑sensitive.

As a result, for the same `book.txt` test file:

- `wc -w` reports: **901,325** words (whitespace-delimited tokens).
- Benchmark spec (ASCII-letter tokens) reports: **928,012** words.

For this benchmark:

- **`wc -w` is not the ground truth.**
- The only ground truth is: *"Does this implementation match the C reference implementation under the ASCII-letters-only spec above?"*

---

## Performance Results

### Rankings (5.3MB test file, ~900K words)
| Rank | Language | Time | vs Baseline | Word Count | Match |
|------|----------|------|-------------|------------|-------|
| 1 | **C Hyperopt** (6-thread)² | 0.008s | baseline | 928,012 | Reference |
| 2 | **Rust** | 0.020s | 2.9x slower | 928,012 | Exact |
| 3 | **C** | 0.022s | 3.1x slower | 928,012 | Exact |
| 4 | **Go** | 0.030s | 4.3x slower | 928,012 | Exact |
| 5 | **PHP** | 0.051s | 7.3x slower | 927,930 | -82 words¹ |
| 6 | **C#** | 0.090s | 12.9x slower | 928,012 | Exact |
| 7 | **JavaScript** | 0.090s | 12.9x slower | 928,012 | Exact |

¹ PHP uses regex with `\b` word boundaries  
² Hyperoptimized for AMD Ryzen 9 9950X3D with AVX-512, CRC32C hashing, 6 threads (100 runs average)

### Performance Scaling Across File Sizes (200 runs average)
| Language | 5.3MB | 53MB | 261MB | Scaling |
|----------|-------|------|-------|---------|
| **C Hyperopt** | 0.008s | 0.030s | 0.030s | Linear |
| **Rust** | 0.020s | 0.221s | 1.176s | Linear |
| **C** | 0.022s | 0.225s | 1.169s | Linear |
| **Go** | 0.030s | 0.296s | 1.605s | Linear |
| **PHP** | 0.051s | 0.561s | 2.694s | Linear |
| **C#** | 0.090s | 0.517s | 2.290s | Linear |
| **JavaScript** | 0.090s | 0.641s | 3.092s | Linear |

### Key Findings

- **C Hyperopt achieves 4.67 GB/s** throughput on large files (12-thread version, exceeds 3.0 GB/s target)
- **Hardware-specific optimization matters**: 6 threads optimal for small files (685 MB/s), 12 threads for large (4.67 GB/s)
- **Rust leads standard implementations** at 20ms for 5.3MB
- **Standard C competitive** with Rust, slightly faster at 261MB scale
- **Go maintains consistent overhead** (1.5x slower than Rust/C)
- **PHP scales well** despite regex overhead
- **C# and JavaScript tied** at 90ms for 5.3MB
- **All implementations scale linearly** with file size
- **11.3x performance spread** between fastest (C Hyperopt) and slowest

### Performance Consistency Analysis

| Language | Consistency | Standard Deviation | Notes |
|----------|-------------|-------------------|--------|
| **C Hyperopt** | Excellent | ~0.000s | Extremely consistent with parallel processing |
| **Rust** | Excellent | ~0.000s | Perfect consistency, no variance |
| **Go** | Excellent | ~0.000s | Extremely predictable performance |
| **JavaScript** | Excellent | ~0.000s | V8 JIT highly consistent after warmup |
| **C#** | Very Good | ~0.002s | 98% runs identical, rare JIT spikes |
| **C** | Good | ~0.004s | 85% consistent, OS scheduling affects |
| **PHP** | Moderate | ~0.005s | Most variable, interpreter overhead |

#### Consistency Insights

- **C Hyperopt shows excellent consistency** despite multi-threading complexity
- **Compiled languages (Rust, Go)** demonstrate best consistency
- **JIT languages (JavaScript, C#)** achieve excellent consistency after warmup
- **Hardware optimization pays off**: C Hyperopt is both fastest AND consistent
- **Standard C shows surprising variance** despite being native code (OS scheduling effects)
- **PHP has highest variance** due to interpreted nature

### Platform Performance Notes

- **C Reference**: 192-241 MB/s throughput, consistent across file sizes
- **C Hyperopt**: Exceeds 3.0 GB/s target with 4.67 GB/s on large files
- **WSL2 overhead**: Minimal (~5%), essentially native Linux performance

## Implementation Approaches

| Language | Method | Word Definition | Optimizations |
|----------|--------|-----------------|---------------|
| **C Hyperopt** | Parallel SIMD processing | ASCII letters only | AVX-512, CRC32C hash, 6-12 threads, open addressing |
| **C** | Byte-level processing | ASCII letters only | FNV-1a hash, custom hash table |
| **Rust** | Byte-level processing | ASCII letters only | FNV HashMap, zero-copy strings |
| **Go** | Byte-level processing | ASCII letters only | 64KB buffer, GOGC=off |
| **JavaScript** | Byte-level processing | ASCII letters only | Direct buffer processing |
| **C#** | Byte-level processing | ASCII letters only | Pre-sized dictionary |
| **PHP** | Regex extraction | `\b[a-z]+\b` pattern | Native C regex, array_count_values |

## Validation Summary

All implementations except PHP produce identical results:

- **928,012 words**: C Hyperopt, C, Rust, Go, JavaScript, C#
- **927,930 words**: PHP (-82 due to regex boundary handling)
- **901,325 words**: `wc -w` (different definition - whitespace-delimited)

See the **[Benchmark Word Definition and Validation Spec](#benchmark-word-definition-and-validation-spec)** section above for the complete specification and explanation of why these numbers differ.


## Technical Details

### Algorithm Complexity
| Language | Time | Space | File Loading |
|----------|------|-------|--------------|
| C Hyperopt | O(n/p) | O(unique words) | Memory-mapped, parallel chunks |
| C | O(n) | O(unique words) | Streaming with 8KB buffer |
| Rust | O(n) | O(n + unique words) | Full file into memory |
| Go | O(n) | O(unique words) | Streaming with 64KB buffer |
| JavaScript | O(n) | O(n + unique words) | Full file into memory |
| C# | O(n) | O(n + unique words) | Full file into memory |
| PHP | O(n) | O(n + unique words) | Full file into memory |

## Use Cases

| Language | Best For |
|----------|----------|
| **C Hyperopt** | Ultra-high performance requirements, hardware-specific optimization |
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
gcc -O3 -march=native -pthread wordcount_hyperopt.c -o wordcount_hopt -lm
rustc -O wordcount.rs -o wordcount_rust
go build -o wordcount_go wordcount.go
dotnet build -c Release

# Run
./wordcount_c book.txt
./wordcount_hopt book.txt
./wordcount_rust book.txt
GOGC=off ./wordcount_go book.txt
./bin/Release/net8.0/WordCount book.txt
node wordcount.js book.txt
php wordcount.php book.txt
```

### Optimization Flags

- **C Hyperopt**: `-O3 -march=native -pthread` (AVX-512, CRC32C)
- **C**: `-O3 -march=native -flto`
- **Rust**: `-C opt-level=3 -C target-cpu=native -C lto=fat`
- **Go**: `-ldflags="-s -w"` with `GOGC=off`
- **C#**: Release mode
- **JavaScript**: `--max-old-space-size=4096`
- **PHP**: `opcache.jit=tracing`

## Key Points

- **Hardware-specific optimization achieves up to 19.6x speedup** (C Hyperopt 4.67 GB/s vs standard C 241 MB/s on large files)
- Implementation quality matters more than language choice
- All implementations use byte-level processing except PHP (regex)
- Word definition: Byte-level vs regex boundaries affects counts
- All implementations are production-optimized
- Thread count tuning critical for multi-core CPUs (6 threads optimal for small files, 12 threads for large files)

## Validation Mode

The `--validate` flag runs cross-language validation:
- Uses C reference implementation as ground truth
- Compares total and unique word counts across all implementations
- Verifies top 10 word consistency (exact match required)
- Reports any differences in counts or rankings

See the **[Benchmark Word Definition and Validation Spec](#benchmark-word-definition-and-validation-spec)** section for the complete validation specification.

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

- **Standard tests**: 200 runs per test for statistical accuracy
- **C Hyperopt**: 100 runs for precise measurement
- **3 file sizes tested**: 5.3MB, 53MB, 261MB
- **Validation mode** ensures all implementations produce identical results
- **Benchmark system**: AMD Ryzen 9 9950X3D, 64GB RAM, WSL2 Ubuntu 24.04
- **Compiler flags**: Full optimizations enabled for all languages
- **Hardware features utilized**: AVX-512, CRC32C, V-Cache optimization