# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Performance benchmark comparing word frequency counting across C, Rust, Go, C#, JavaScript, and PHP using byte-level text processing.

## Commands

### Benchmarks
```bash
# Install all dependencies (first time setup)
./install-deps.sh

# Run benchmark with default settings (3 runs per test)
./bench.sh

# Run benchmark with validation (verifies all implementations match)
./bench.sh --validate

# Run benchmark with custom iteration count
./bench.sh --runs=10

# Run benchmark with validation and custom runs
./bench.sh --validate --runs=5
```

### Building
```bash
# C
gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops wordcount.c -o wordcount_c

# Rust
rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 wordcount.rs -o wordcount_rust

# Go
go build -gcflags="-B" -ldflags="-s -w" -o wordcount_go wordcount.go

# C#
dotnet build -c Release
```

### Running
```bash
./wordcount_c book.txt
./wordcount_rust book.txt
GOGC=off ./wordcount_go book.txt
./bin/Release/net8.0/WordCount book.txt
node --max-old-space-size=4096 wordcount.js book.txt
php -d opcache.enable_cli=1 -d opcache.jit=tracing wordcount.php book.txt
```

### Testing
```bash
# Download test file
curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt

# Clean up
rm -f wordcount_rust wordcount_c wordcount_go
rm -rf bin obj WordCount.csproj
rm -f book_10mb.txt book_50mb.txt *_results.txt
```

## Architecture

### Core Algorithm
- Byte-level processing (raw bytes, not strings)
- Manual word extraction (ASCII letters [a-zA-Z])
- In-place lowercase conversion
- Hash tables optimized per language
- Single-pass file processing

### Language Specifics

**C**: FNV-1a hash, custom hash table, 8KB buffer
**Rust**: Custom FNV hasher, zero-copy strings, pre-sized HashMap
**Go**: 64KB buffer, buffer boundary handling, unsafe string conversions
**C#**: UTF-8 BOM handling, pre-sized Dictionary
**JavaScript**: Direct buffer processing, no regex
**PHP**: PCRE regex `\b[a-z]+\b`, array_count_values()

### Key Details

- Word definition: ASCII letters [a-zA-Z] (except PHP uses regex boundaries)
- C implementation is reference for validation
- `--validate` flag verifies consistency
- Each implementation outputs `*_results.txt`

## Development Notes

- C implementation is the reference
- Validate changes with `./bench.sh --validate`
- Use GOGC=off for Go benchmarks
- PHP word count differs due to regex boundaries (expected)

## Performance

Expected results (5.3MB file, ~900K words):
- Rust: ~20ms
- C: ~22ms  
- Go: ~30ms
- PHP: ~52ms
- JavaScript: ~90ms
- C#: ~90ms

All scale linearly O(n).