#!/bin/bash
# benchmark.sh - Performance comparison of word frequency counters
# Usage: ./bench.sh [--validate] [--runs=N]

# Parse command line arguments
VALIDATE=0
NUM_RUNS=3
for arg in "$@"; do
    case $arg in
        --validate)
            VALIDATE=1
            shift
            ;;
        --runs=*)
            NUM_RUNS="${arg#*=}"
            shift
            ;;
        --help)
            echo "Usage: $0 [--validate] [--runs=N]"
            echo "  --validate  Verify all implementations produce the same results"
            echo "  --runs=N    Number of benchmark runs (default: 3)"
            exit 0
            ;;
    esac
done

echo "========================================="
echo "Word Frequency Counter - Language Benchmark"
echo "========================================="
echo ""

if [ $VALIDATE -eq 1 ]; then
    echo "Mode: Benchmark with validation"
else
    echo "Mode: Benchmark only"
fi
echo "Runs per test: $NUM_RUNS"
echo ""

# Check dependencies
check_dependency() {
    if ! command -v $1 &> /dev/null; then
        echo "✗ $1 is not installed"
        return 1
    else
        # Special handling for go which doesn't accept --version
        if [ "$1" = "go" ]; then
            echo "✓ $1 is installed ($(go version 2>&1))"
        else
            echo "✓ $1 is installed ($($1 --version 2>&1 | head -n1))"
        fi
        return 0
    fi
}

echo "Checking dependencies..."
check_dependency node
HAS_NODE=$?; HAS_NODE=$((1-HAS_NODE))

check_dependency php
HAS_PHP=$?; HAS_PHP=$((1-HAS_PHP))

check_dependency rustc
HAS_RUST=$?; HAS_RUST=$((1-HAS_RUST))

check_dependency dotnet
HAS_DOTNET=$?; HAS_DOTNET=$((1-HAS_DOTNET))

check_dependency gcc
HAS_GCC=$?; HAS_GCC=$((1-HAS_GCC))

check_dependency go
HAS_GO=$?; HAS_GO=$((1-HAS_GO))

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

# Compile Rust
if [ "$HAS_RUST" = "1" ]; then
    echo "Compiling Rust version..."
    rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 wordcount.rs -o wordcount_rust 2>rust_error.log
    if [ $? -eq 0 ]; then
        echo "✓ Rust compilation successful"
        rm -f rust_error.log
    else
        echo "✗ Rust compilation failed"
        cat rust_error.log | head -10
        echo ""
        HAS_RUST=0
    fi
fi

# Compile C
if [ "$HAS_GCC" = "1" ]; then
    echo "Compiling C version..."
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops wordcount.c -o wordcount_c 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C compilation successful"
    else
        echo "✗ C compilation failed"
        HAS_GCC=0
    fi
fi

# Build Go
if [ "$HAS_GO" = "1" ]; then
    echo "Building Go version..."
    go build -gcflags="-B" -ldflags="-s -w" -o wordcount_go wordcount.go 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ Go build successful"
    else
        echo "✗ Go build failed"
        HAS_GO=0
    fi
fi

# Build C#
if [ "$HAS_DOTNET" = "1" ]; then
    echo "Building C# version..."
    if [ ! -f "WordCount.csproj" ]; then
        cat > WordCount.csproj << 'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <PublishSingleFile>true</PublishSingleFile>
    <PublishTrimmed>true</PublishTrimmed>
    <InvariantGlobalization>true</InvariantGlobalization>
    <TieredCompilation>false</TieredCompilation>
    <PublishReadyToRun>true</PublishReadyToRun>
  </PropertyGroup>
</Project>
EOF
    fi
    
    dotnet build -c Release --nologo --verbosity quiet 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C# build successful"
    else
        echo "✗ C# build failed"
        HAS_DOTNET=0
    fi
fi
echo ""

# Validation function
validate_results() {
    local file=$1
    local base_name="${file%.txt}"
    
    echo "  Validating results for $file..."
    
    # Collect all result files for this input
    declare -a result_files
    declare -a languages
    
    if [ -f "${base_name}_c_results.txt" ]; then
        result_files+=("${base_name}_c_results.txt")
        languages+=("C")
    fi
    if [ -f "${base_name}_rust_optimized_results.txt" ]; then
        result_files+=("${base_name}_rust_optimized_results.txt")
        languages+=("Rust")
    fi
    if [ -f "${base_name}_go_results.txt" ]; then
        result_files+=("${base_name}_go_results.txt")
        languages+=("Go")
    fi
    if [ -f "${base_name}_csharp_results.txt" ]; then
        result_files+=("${base_name}_csharp_results.txt")
        languages+=("C#")
    fi
    if [ -f "${base_name}_javascript_results.txt" ]; then
        result_files+=("${base_name}_javascript_results.txt")
        languages+=("JavaScript")
    fi
    if [ -f "${base_name}_php_results.txt" ]; then
        result_files+=("${base_name}_php_results.txt")
        languages+=("PHP")
    fi
    
    if [ ${#result_files[@]} -lt 2 ]; then
        echo "    ⚠ Less than 2 result files found, skipping validation"
        return
    fi
    
    # Extract top 10 words and counts from each file
    # More robust extraction that handles different formats
    local temp_dir=$(mktemp -d)
    local has_differences=0
    declare -A differences
    
    for i in "${!result_files[@]}"; do
        # Extract lines that look like rankings
        # Handle formats like "1. word 1,234" or "1  word  1234"
        grep -E '^\s*[0-9]+[\.\s]' "${result_files[$i]}" | \
            head -10 | \
            sed 's/[,]//g' | \
            awk '{
                # Find the word (second non-numeric field) and count (last numeric field)
                word = ""
                count = ""
                for(i=1; i<=NF; i++) {
                    if ($i ~ /^[0-9]+$/ && i > 1) {
                        count = $i
                    } else if ($i !~ /^[0-9]+\.?$/ && word == "") {
                        word = $i
                    }
                }
                if (word != "" && count != "") print word, count
            }' | sort > "$temp_dir/results_$i.txt"
    done
    
    # Check if all files have the same content
    local reference_file="$temp_dir/results_0.txt"
    local all_match=1
    
    for ((i=1; i<${#result_files[@]}; i++)); do
        if ! cmp -s "$reference_file" "$temp_dir/results_$i.txt"; then
            all_match=0
            differences["${languages[$i]}"]=1
        fi
    done
    
    if [ $all_match -eq 1 ]; then
        echo "    ✓ All implementations produce identical results"
    else
        echo "    ⚠ Results differ between implementations:"
        
        # Show which implementations differ
        local matching_with_c=""
        local different_from_c=""
        
        for ((i=1; i<${#languages[@]}; i++)); do
            if cmp -s "$reference_file" "$temp_dir/results_$i.txt"; then
                matching_with_c="$matching_with_c ${languages[$i]}"
            else
                different_from_c="$different_from_c ${languages[$i]}"
            fi
        done
        
        if [ -n "$matching_with_c" ]; then
            echo "      Matching: C${matching_with_c}"
        fi
        if [ -n "$different_from_c" ]; then
            echo "      Different:${different_from_c}"
        fi
        
        echo "    Top 3 words from each implementation:"
        for i in "${!languages[@]}"; do
            local top3=$(head -3 "$temp_dir/results_$i.txt" | awk '{printf "%s(%s) ", $1, $2}')
            printf "      %-15s %s\n" "${languages[$i]}:" "$top3"
        done
    fi
    
    rm -rf "$temp_dir"
}

# Arrays to store results for ranking
declare -a LANG_NAMES
declare -a LANG_TIMES
declare -A ALL_RESULTS
RESULT_COUNT=0

# Run benchmarks
run_benchmark() {
    local lang=$1
    local cmd=$2
    local file=$3
    
    echo "  $lang:"
    
    # Warm-up run (not measured)
    $cmd "$file" > /dev/null 2>&1
    
    # Actual benchmark (NUM_RUNS runs, take average)
    local total_time=0
    for ((run=1; run<=NUM_RUNS; run++)); do
        # Use GNU time if available, otherwise use built-in time
        if command -v /usr/bin/time &> /dev/null; then
            local result=$(/usr/bin/time -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        elif command -v gtime &> /dev/null; then
            local result=$(gtime -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        else
            # Fallback to bash time with nanosecond precision
            local start=$(date +%s%N)
            $cmd "$file" > /dev/null 2>&1
            local end=$(date +%s%N)
            local result=$(echo "scale=6; ($end - $start) / 1000000000" | bc)
        fi
        
        # Ensure minimum value to avoid zero
        if (( $(echo "$result < 0.001" | bc -l) )); then
            result="0.001"
        fi
        
        total_time=$(echo "scale=6; $total_time + $result" | bc)
        printf "    Run %d: %.3fs\n" "$run" "$result"
    done
    
    local avg_time=$(echo "scale=6; $total_time / $NUM_RUNS" | bc)
    # Format to 3 decimal places for display
    avg_time=$(printf "%.3f" "$avg_time")
    echo "    Average: ${avg_time}s"
    echo ""
    
    # Store results for ranking (only for first test file)
    if [[ "$file" == "${TEST_FILES[0]}" ]]; then
        LANG_NAMES[RESULT_COUNT]="$lang"
        LANG_TIMES[RESULT_COUNT]="$avg_time"
        RESULT_COUNT=$((RESULT_COUNT + 1))
    fi
    
    # Store all results for summary table
    ALL_RESULTS["${lang}:${file}"]="$avg_time"
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
    
    if [ "$HAS_RUST" = "1" ] && [ -f "wordcount_rust" ]; then
        run_benchmark "Rust" "./wordcount_rust" "$TEST_FILE"
    fi
    
    if [ "$HAS_GO" = "1" ] && [ -f "wordcount_go" ]; then
        export GOGC=off
        run_benchmark "Go" "./wordcount_go" "$TEST_FILE"
        unset GOGC
    fi
    
    if [ "$HAS_DOTNET" = "1" ]; then
        if [ -f "bin/Release/net8.0/WordCount" ]; then
            DOTNET_TieredCompilation=0 run_benchmark "C# (.NET)" "./bin/Release/net8.0/WordCount" "$TEST_FILE"
        elif [ -f "wordcount_cs.exe" ]; then
            run_benchmark "C# (Mono)" "./wordcount_cs.exe" "$TEST_FILE"
        fi
    fi
    
    # Run interpreted languages
    if [ "$HAS_NODE" = "1" ]; then
        run_benchmark "JavaScript (Node.js)" "node --max-old-space-size=4096 --optimize-for-size wordcount.js" "$TEST_FILE"
    fi
    
    if [ "$HAS_PHP" = "1" ]; then
        # Get file size in bytes
        FILE_SIZE_BYTES=$(stat -c%s "$TEST_FILE" 2>/dev/null || stat -f%z "$TEST_FILE" 2>/dev/null)
        
        # PHP strategy: Use JIT for files > 10MB
        if [ "$FILE_SIZE_BYTES" -gt 10485760 ]; then
            # Large file: JIT helps
            run_benchmark "PHP" "php -d opcache.enable_cli=1 -d opcache.jit=tracing -d opcache.jit_buffer_size=128M wordcount.php" "$TEST_FILE"
        else
            # Small file: JIT adds overhead
            run_benchmark "PHP" "php wordcount.php" "$TEST_FILE"
        fi
    fi
    
    # Validate results if requested
    if [ $VALIDATE -eq 1 ]; then
        validate_results "$TEST_FILE"
        echo ""
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

# Summary with actual results
echo "========================================="
echo "Benchmark Complete!"
echo "========================================="
echo ""

# Sort and display actual performance ranking
if [ $RESULT_COUNT -gt 0 ]; then
    echo "Performance Ranking (${TEST_FILES[0]}):"
    echo "----------------------------------------"
    
    # Create temporary file for sorting
    TEMP_RESULTS=$(mktemp)
    for ((i=0; i<RESULT_COUNT; i++)); do
        echo "${LANG_TIMES[$i]} ${LANG_NAMES[$i]}" >> "$TEMP_RESULTS"
    done
    
    # Sort by time and display with ranking
    RANK=1
    BASELINE_TIME=""
    sort -n "$TEMP_RESULTS" | while read TIME LANG; do
        if [ -z "$BASELINE_TIME" ]; then
            BASELINE_TIME="$TIME"
            printf "%d. %-20s %8.3fs (baseline)\n" "$RANK" "$LANG" "$TIME"
        else
            if (( $(echo "$BASELINE_TIME > 0" | bc -l) )); then
                SLOWDOWN=$(echo "scale=1; $TIME / $BASELINE_TIME" | bc)
                printf "%d. %-20s %8.3fs (%.1fx slower)\n" "$RANK" "$LANG" "$TIME" "$SLOWDOWN"
            else
                printf "%d. %-20s %8.3fs\n" "$RANK" "$LANG" "$TIME"
            fi
        fi
        RANK=$((RANK + 1))
    done
    
    echo ""
    
    # Show comparison table if multiple files were tested
    if [ ${#TEST_FILES[@]} -gt 1 ]; then
        echo "Performance Across File Sizes:"
        echo "----------------------------------------"
        printf "%-20s" "Language"
        for FILE in "${TEST_FILES[@]}"; do
            SIZE=$(du -h "$FILE" | cut -f1)
            printf " %10s" "$SIZE"
        done
        echo ""
        echo "----------------------------------------"
        
        # Display results for each language
        for ((i=0; i<RESULT_COUNT; i++)); do
            LANG="${LANG_NAMES[$i]}"
            printf "%-20s" "$LANG"
            for FILE in "${TEST_FILES[@]}"; do
                TIME="${ALL_RESULTS["${LANG}:${FILE}"]}"
                if [ -n "$TIME" ]; then
                    printf " %10.3fs" "$TIME"
                else
                    printf " %10s" "N/A"
                fi
            done
            echo ""
        done
        echo ""
    fi
    
    # Calculate performance spread
    if [ $RESULT_COUNT -gt 1 ]; then
        FASTEST="${LANG_TIMES[0]}"
        SLOWEST="${LANG_TIMES[0]}"
        for ((i=1; i<RESULT_COUNT; i++)); do
            if (( $(echo "${LANG_TIMES[$i]} < $FASTEST" | bc -l) )); then
                FASTEST="${LANG_TIMES[$i]}"
            fi
            if (( $(echo "${LANG_TIMES[$i]} > $SLOWEST" | bc -l) )); then
                SLOWEST="${LANG_TIMES[$i]}"
            fi
        done
        
        if (( $(echo "$FASTEST > 0" | bc -l) )); then
            SPREAD=$(echo "scale=1; $SLOWEST / $FASTEST" | bc)
            echo "Key Insights:"
            echo "- Performance spread: ${SPREAD}x between fastest and slowest"
            echo "- Test system: $(uname -s) on $(uname -m)"
            
            # Try to get CPU info
            if command -v lscpu &> /dev/null; then
                echo "- CPU: $(lscpu | grep "Model name" | cut -d: -f2 | xargs)"
            elif [ -f /proc/cpuinfo ]; then
                echo "- CPU: $(grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
            fi
        fi
    fi
    
    # Clean up temp file
    rm -f "$TEMP_RESULTS"
else
    echo "No benchmark results collected."
fi
echo ""

echo "To run individual tests:"
if [ "$HAS_GCC" = "1" ]; then
    echo "  ./wordcount_c book.txt"
fi
if [ "$HAS_RUST" = "1" ]; then
    echo "  ./wordcount_rust book.txt"
fi
if [ "$HAS_GO" = "1" ]; then
    echo "  GOGC=off ./wordcount_go book.txt"
fi
if [ "$HAS_DOTNET" = "1" ]; then
    echo "  ./bin/Release/net8.0/WordCount book.txt"
fi
if [ "$HAS_NODE" = "1" ]; then
    echo "  node wordcount.js book.txt"
fi
if [ "$HAS_PHP" = "1" ]; then
    echo "  php wordcount.php book.txt"
fi

# Clean up compiled files and test files
echo ""
echo "Clean up compiled files, test files, and build artifacts? (y/n)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    # Remove compiled binaries
    rm -f wordcount_rust wordcount_c wordcount_go wordcount_cs.exe
    rm -rf bin obj
    rm -f WordCount.csproj
    
    # Remove test files
    rm -f book.txt book_10mb.txt book_50mb.txt
    
    # Remove result files
    rm -f *_results.txt
    
    echo "✓ Cleaned up all files"
fi