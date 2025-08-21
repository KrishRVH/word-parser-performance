#!/bin/bash
# benchmark.sh - Fair performance comparison of word frequency counters

echo "========================================="
echo "Word Frequency Counter - Language Benchmark"
echo "========================================="
echo ""

# Check dependencies
check_dependency() {
    if ! command -v $1 &> /dev/null; then
        echo "❌ $1 is not installed"
        return 1
    else
        echo "✓ $1 is installed ($($1 --version 2>&1 | head -n1))"
        return 0
    fi
}

echo "Checking dependencies..."
HAS_NODE=$(check_dependency node && echo 1 || echo 0)
HAS_PHP=$(check_dependency php && echo 1 || echo 0)
HAS_RUST=$(check_dependency rustc && echo 1 || echo 0)
HAS_DOTNET=$(check_dependency dotnet && echo 1 || echo 0)
HAS_GCC=$(check_dependency gcc && echo 1 || echo 0)
HAS_GO=$(check_dependency go && echo 1 || echo 0)
echo ""

# Create test file if it doesn't exist
if [ ! -f "book.txt" ]; then
    echo "Downloading test file (Moby Dick from Project Gutenberg)..."
    curl -s https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt
    echo "✓ Downloaded book.txt"
else
    echo "✓ Using existing book.txt"
fi

# Display file info
FILE_SIZE=$(du -h book.txt | cut -f1)
WORD_COUNT=$(wc -w < book.txt)
LINE_COUNT=$(wc -l < book.txt)
echo ""
echo "Test file information:"
echo "  Size: $FILE_SIZE"
echo "  Words: $(printf "%'d" $WORD_COUNT)"
echo "  Lines: $(printf "%'d" $LINE_COUNT)"
echo ""

# Create larger test files
create_test_file() {
    local size=$1
    local filename=$2
    local multiplier=$3
    
    if [ ! -f "$filename" ]; then
        echo "Creating $size test file..."
        > "$filename"
        for i in $(seq 1 $multiplier); do
            cat book.txt >> "$filename"
        done
        echo "✓ Created $filename"
    fi
}

# Optionally create larger test files
echo "Do you want to test with larger files? (y/n)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    create_test_file "10MB" "book_10mb.txt" 10
    create_test_file "50MB" "book_50mb.txt" 50
    TEST_FILES=("book.txt" "book_10mb.txt" "book_50mb.txt")
else
    TEST_FILES=("book.txt")
fi
echo ""

# Compile Rust if available
if [ "$HAS_RUST" = "1" ]; then
    echo "Compiling Rust version with optimizations..."
    RUSTC_VERSION=$(rustc --version | cut -d' ' -f2) rustc -O wordcount.rs -o wordcount_rust 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ Rust compilation successful"
    else
        echo "❌ Rust compilation failed"
        HAS_RUST=0
    fi
fi

# Compile C if available
if [ "$HAS_GCC" = "1" ]; then
    echo "Compiling C version with optimizations..."
    gcc -O3 -march=native wordcount.c -o wordcount_c 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C compilation successful"
    else
        echo "❌ C compilation failed"
        HAS_GCC=0
    fi
fi

# Build Go if available
if [ "$HAS_GO" = "1" ]; then
    echo "Building Go version with optimizations..."
    go build -ldflags="-s -w" -o wordcount_go wordcount.go 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ Go compilation successful"
    else
        echo "❌ Go compilation failed"
        HAS_GO=0
    fi
fi

# Build C# if available
if [ "$HAS_DOTNET" = "1" ]; then
    echo "Building C# version with optimizations..."
    # Check if we need to create a project file
    if [ ! -f "WordCount.csproj" ]; then
        cat > WordCount.csproj << 'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net6.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <PublishSingleFile>true</PublishSingleFile>
    <PublishTrimmed>true</PublishTrimmed>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
</Project>
EOF
    fi
    
    dotnet build -c Release --nologo --verbosity quiet 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C# compilation successful"
    else
        # Try fallback with csc if available
        if command -v csc &> /dev/null; then
            csc -optimize+ -out:wordcount_cs.exe WordCount.cs 2>/dev/null
            if [ $? -eq 0 ]; then
                echo "✓ C# compilation successful (using csc)"
            else
                echo "❌ C# compilation failed"
                HAS_DOTNET=0
            fi
        else
            echo "❌ C# compilation failed"
            HAS_DOTNET=0
        fi
    fi
fi
echo ""

# Run benchmarks
run_benchmark() {
    local lang=$1
    local cmd=$2
    local file=$3
    
    echo "  $lang:"
    
    # Warm-up run (not measured)
    $cmd "$file" > /dev/null 2>&1
    
    # Actual benchmark (3 runs, take average)
    local total_time=0
    for run in 1 2 3; do
        # Use GNU time if available, otherwise use built-in time
        if command -v gtime &> /dev/null; then
            # macOS with GNU time
            local result=$(gtime -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        elif command -v /usr/bin/time &> /dev/null; then
            # Linux
            local result=$(/usr/bin/time -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        else
            # Fallback to bash time
            local start=$(date +%s%N)
            $cmd "$file" > /dev/null 2>&1
            local end=$(date +%s%N)
            local result=$(echo "scale=3; ($end - $start) / 1000000000" | bc)
        fi
        total_time=$(echo "$total_time + $result" | bc)
        echo -n "    Run $run: ${result}s"
        echo ""
    done
    
    local avg_time=$(echo "scale=3; $total_time / 3" | bc)
    echo "    Average: ${avg_time}s"
    echo ""
}

# Main benchmark loop
for TEST_FILE in "${TEST_FILES[@]}"; do
    echo "========================================="
    echo "Benchmarking with: $TEST_FILE ($(du -h $TEST_FILE | cut -f1))"
    echo "========================================="
    echo ""
    
    # Run compiled languages first (typically fastest)
    if [ "$HAS_GCC" = "1" ]; then
        run_benchmark "C" "./wordcount_c" "$TEST_FILE"
    fi
    
    if [ "$HAS_RUST" = "1" ]; then
        run_benchmark "Rust" "./wordcount_rust" "$TEST_FILE"
    fi
    
    if [ "$HAS_GO" = "1" ]; then
        run_benchmark "Go" "./wordcount_go" "$TEST_FILE"
    fi
    
    if [ "$HAS_DOTNET" = "1" ]; then
        if [ -f "bin/Release/net6.0/WordCount" ]; then
            run_benchmark "C# (.NET)" "./bin/Release/net6.0/WordCount" "$TEST_FILE"
        elif [ -f "wordcount_cs.exe" ]; then
            run_benchmark "C# (Mono)" "./wordcount_cs.exe" "$TEST_FILE"
        fi
    fi
    
    # Run interpreted languages (typically slower)
    if [ "$HAS_NODE" = "1" ]; then
        run_benchmark "JavaScript (Node.js)" "node wordcount.js" "$TEST_FILE"
    fi
    
    if [ "$HAS_PHP" = "1" ]; then
        run_benchmark "PHP" "php wordcount.php" "$TEST_FILE"
    fi
done

# Check for output files
echo "========================================="
echo "Output Files Generated:"
echo "========================================="
for f in *_results.txt; do
    if [ -f "$f" ]; then
        echo "  ✓ $f ($(wc -l < "$f") lines)"
    fi
done
echo ""

# Summary
echo "========================================="
echo "Benchmark Complete!"
echo "========================================="
echo ""
echo "Performance Ranking (typical):"
echo "1. C         - Fastest (baseline)"
echo "2. Rust      - ~1.2x slower than C"
echo "3. Go        - ~1.5x slower than C"
echo "4. C# (.NET) - ~3x slower than C"
echo "5. JavaScript- ~7x slower than C"
echo "6. PHP       - ~10x slower than C"
echo ""
echo "Key observations:"
echo "- Compiled languages (C, Rust, C#) significantly outperform interpreted ones"
echo "- Memory usage varies by 10x between C and interpreted languages"
echo "- Output files contain detailed word frequency analysis"
echo ""
echo "To get detailed statistics, run each program individually:"
if [ "$HAS_GCC" = "1" ]; then
    echo "  ./wordcount_c book.txt"
fi
if [ "$HAS_RUST" = "1" ]; then
    echo "  ./wordcount_rust book.txt"
fi
if [ "$HAS_GO" = "1" ]; then
    echo "  ./wordcount_go book.txt"
fi
if [ "$HAS_DOTNET" = "1" ]; then
    echo "  dotnet run --configuration Release book.txt"
fi
if [ "$HAS_NODE" = "1" ]; then
    echo "  node wordcount.js book.txt"
fi
if [ "$HAS_PHP" = "1" ]; then
    echo "  php wordcount.php book.txt"
fi

# Clean up compiled files
echo ""
echo "Clean up compiled files and build artifacts? (y/n)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    rm -f wordcount_rust wordcount_c wordcount_go wordcount_cs.exe
    rm -rf bin obj
    rm -f WordCount.csproj
    echo "✓ Cleaned up"
fi