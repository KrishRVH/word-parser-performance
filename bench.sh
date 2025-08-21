#!/bin/bash
# benchmark.sh - Fair performance comparison of word frequency counters

echo "========================================="
echo "Word Frequency Counter - Language Benchmark"
echo "========================================="
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
# Fixed: Now properly setting variables without capturing echo output
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

# Compile Rust if available
if [ "$HAS_RUST" = "1" ]; then
    echo "Compiling Rust version with optimizations..."
    rustc -O wordcount.rs -o wordcount_rust 2>rust_error.log
    if [ $? -eq 0 ]; then
        echo "✓ Rust compilation successful"
        rm -f rust_error.log
    else
        echo "✗ Rust compilation failed - Error details:"
        cat rust_error.log | head -10
        echo ""
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
        echo "✗ C compilation failed"
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
        echo "✗ Go compilation failed"
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
    <TargetFramework>net8.0</TargetFramework>
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
                echo "✗ C# compilation failed"
                HAS_DOTNET=0
            fi
        else
            echo "✗ C# compilation failed"
            HAS_DOTNET=0
        fi
    fi
fi
echo ""

# Arrays to store results for ranking
declare -a LANG_NAMES
declare -a LANG_TIMES
declare -A ALL_RESULTS  # Store all results for summary table
RESULT_COUNT=0

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
        if command -v /usr/bin/time &> /dev/null; then
            # Linux with GNU time - use more precision
            local result=$(/usr/bin/time -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        elif command -v gtime &> /dev/null; then
            # macOS with GNU time
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
    
    local avg_time=$(echo "scale=6; $total_time / 3" | bc)
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
        run_benchmark "Go" "./wordcount_go" "$TEST_FILE"
    fi
    
    if [ "$HAS_DOTNET" = "1" ]; then
        if [ -f "bin/Release/net8.0/WordCount" ]; then
            run_benchmark "C# (.NET)" "./bin/Release/net8.0/WordCount" "$TEST_FILE"
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

# Summary with actual results
echo "========================================="
echo "Benchmark Complete!"
echo "========================================="
echo ""

# Sort and display actual performance ranking
if [ $RESULT_COUNT -gt 0 ]; then
    echo "Actual Performance Ranking (${TEST_FILES[0]}):"
    echo "----------------------------------------"
    
    # Create temporary file for sorting
    TEMP_RESULTS=$(mktemp)
    for ((i=0; i<RESULT_COUNT; i++)); do
        echo "${LANG_TIMES[$i]} ${LANG_NAMES[$i]}" >> "$TEMP_RESULTS"
    done
    
    # Sort by time and display with ranking
    RANK=1
    BASELINE_TIME=""
    FASTEST_LANG=""
    sort -n "$TEMP_RESULTS" | while read TIME LANG; do
        if [ -z "$BASELINE_TIME" ]; then
            BASELINE_TIME="$TIME"
            FASTEST_LANG="$LANG"
            printf "%d. %-20s %8.3fs (baseline)\n" "$RANK" "$LANG" "$TIME"
        else
            # Avoid divide by zero
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
            echo "- CPU: $(lscpu | grep "Model name" | cut -d: -f2 | xargs)"
        else
            echo "Key Insights:"
            echo "- Test system: $(uname -s) on $(uname -m)"
            echo "- CPU: $(lscpu | grep "Model name" | cut -d: -f2 | xargs)"
        fi
    fi
    
    # Clean up temp file
    rm -f "$TEMP_RESULTS"
else
    echo "No benchmark results collected."
fi
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