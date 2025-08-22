#!/bin/bash
# bench_c.sh - C implementation comparison and optimization tool
# Usage: ./bench_c.sh [--runs=N] [-d|--debug] [--profile] [--validate] [--threads=N]

NUM_RUNS=5
DEBUG_MODE=0
PROFILE_MODE=0
VALIDATE_MODE=0
THREAD_COUNT=""
NO_CLEANUP=0

for arg in "$@"; do
    case $arg in
        --runs=*)
            NUM_RUNS="${arg#*=}"
            shift
            ;;
        -d|--debug)
            DEBUG_MODE=1
            shift
            ;;
        --profile)
            PROFILE_MODE=1
            shift
            ;;
        --validate)
            VALIDATE_MODE=1
            shift
            ;;
        --threads=*)
            THREAD_COUNT="${arg#*=}"
            shift
            ;;
        --no-cleanup)
            NO_CLEANUP=1
            shift
            ;;
        --help)
            echo "Usage: $0 [--runs=N] [-d|--debug] [--profile] [--validate] [--threads=N] [--no-cleanup]"
            echo "  --runs=N      Number of benchmark runs (default: 5)"
            echo "  -d, --debug   Build with debug symbols and collect detailed metrics"
            echo "  --profile     Run with perf profiling"
            echo "  --validate    Compare outputs between implementations"
            echo "  --threads=N   Test with custom thread count (default: 8)"
            echo "  --no-cleanup  Don't prompt for cleanup at the end"
            exit 0
            ;;
    esac
done

echo "========================================="
echo "C Implementation Comparison Tool"
echo "========================================="
echo ""

if [ $DEBUG_MODE -eq 1 ]; then
    echo "DEBUG MODE ENABLED"
    echo "  - Full instrumentation"
    echo "  - Debug logs will be collected"
    echo "  - Performance analysis will be detailed"
    [ -n "$THREAD_COUNT" ] && echo "  - Testing with $THREAD_COUNT threads"
    echo ""
fi

echo "Comparing:"
echo "  - wordcount.c (reference implementation)"
echo "  - wordcount_hyperopt.c (hyperoptimized version)"
echo ""

# Check dependencies
if ! command -v gcc &> /dev/null; then
    echo "Error: gcc is not installed"
    exit 1
fi

# Check for test file
if [ ! -f "book.txt" ]; then
    echo "Downloading test file..."
    curl -s https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt
    echo "✓ Downloaded book.txt"
fi

FILE_SIZE=$(du -h book.txt | cut -f1)
FILE_SIZE_BYTES=$(stat -c%s book.txt 2>/dev/null || stat -f%z book.txt 2>/dev/null)
FILE_SIZE_MB=$(echo "scale=2; $FILE_SIZE_BYTES / 1048576" | bc)
WORD_COUNT=$(wc -w < book.txt)
echo "Test file: book.txt ($FILE_SIZE, $(printf "%'d" $WORD_COUNT) words)"
echo ""

# Clean up old debug logs
rm -f wordcount_debug.log wordcount_debug_*.log

# Compile reference C version
echo "Building reference C implementation..."
if [ $DEBUG_MODE -eq 1 ]; then
    gcc -O2 -g -fno-omit-frame-pointer -march=native \
        wordcount.c -o wordcount_c_ref 2>/dev/null
else
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
        wordcount.c -o wordcount_c_ref 2>/dev/null
fi

if [ $? -eq 0 ]; then
    echo "✓ Reference build successful"
else
    echo "✗ Reference build failed"
    exit 1
fi

# Build variants array
declare -a BUILD_VARIANTS
declare -a BUILD_NAMES

if [ $DEBUG_MODE -eq 1 ]; then
    echo ""
    echo "Building debug variants..."
    
    # Standard debug build - optimized for meaningful metrics
    echo "  Building optimized debug version (O2 + DEBUG)..."
    # Check if we're on Zen 5 for explicit AVX-512 flags
    if lscpu 2>/dev/null | grep -q "Zen 5\|9950X3D\|znver5"; then
        echo "    Detected Zen 5 - using explicit AVX-512 flags"
        gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=znver4 -mtune=znver4 \
            -mavx512f -mavx512bw -mavx512vl -msse4.2 -pthread \
            wordcount_hyperopt.c -o wordcount_debug -lm 2>/dev/null
    else
        gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=native -pthread \
            wordcount_hyperopt.c -o wordcount_debug -lm 2>/dev/null
    fi
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_debug")
        BUILD_NAMES+=("Debug Optimized")
        echo "    ✓ Optimized debug build successful"
    fi
    
    
    # Custom thread count build
    if [ -n "$THREAD_COUNT" ]; then
        echo "  Building optimized debug version with $THREAD_COUNT threads..."
        gcc -O2 -g -DDEBUG -fno-omit-frame-pointer -march=native -pthread \
            -DNUM_THREADS=$THREAD_COUNT wordcount_hyperopt.c -o wordcount_debug_t$THREAD_COUNT -lm 2>/dev/null
        if [ $? -eq 0 ]; then
            BUILD_VARIANTS+=("./wordcount_debug_t$THREAD_COUNT")
            BUILD_NAMES+=("Debug ${THREAD_COUNT}-thread")
            echo "    ✓ ${THREAD_COUNT}-thread debug build successful"
        fi
    fi
    
    # Also build optimized for comparison
    echo "  Building optimized version for comparison..."
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread \
        wordcount_hyperopt.c -o wordcount_hopt_opt -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_hopt_opt")
        BUILD_NAMES+=("Optimized (O3)")
        echo "    ✓ Optimized build successful"
    fi
else
    # Regular optimized build - now defaults to 8 threads
    echo "Building hyperopt C implementation (8 threads by default)..."
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread \
        wordcount_hyperopt.c -o wordcount_hopt -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_hopt")
        BUILD_NAMES+=("C Hyperopt (8-thread)")
        echo "✓ Hyperopt build successful"
    else
        echo "✗ Hyperopt build failed"
        exit 1
    fi
    
    # Custom thread count build only if explicitly requested
    if [ -n "$THREAD_COUNT" ] && [ "$THREAD_COUNT" != "8" ]; then
        echo "Building hyperopt with custom $THREAD_COUNT threads..."
        gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread \
            -DNUM_THREADS=$THREAD_COUNT wordcount_hyperopt.c -o wordcount_hopt_t$THREAD_COUNT -lm 2>/dev/null
        if [ $? -eq 0 ]; then
            BUILD_VARIANTS+=("./wordcount_hopt_t$THREAD_COUNT")
            BUILD_NAMES+=("C Hyperopt ${THREAD_COUNT}-thread")
            echo "✓ ${THREAD_COUNT}-thread build successful"
        fi
    fi
fi

echo ""
echo "========================================="
echo "Warming up..."
echo "========================================="
cat book.txt > /dev/null
./wordcount_c_ref book.txt > /dev/null 2>&1
if [ ${#BUILD_VARIANTS[@]} -gt 0 ]; then
    ${BUILD_VARIANTS[0]} book.txt > /dev/null 2>&1
fi
echo "✓ Cache warmed"
echo ""

# Function to analyze debug log
analyze_debug_log() {
    local log_file=$1
    local variant_name=$2
    
    if [ ! -f "$log_file" ]; then
        echo "    ⚠ No debug log found"
        return
    fi
    
    echo "    Debug Analysis for $variant_name:"
    echo "    ────────────────────────────────"
    
    # Key initialization info
    echo "    Initialization:"
    grep -E "V-Cache CCD|PID:|mmap:|init:" "$log_file" | head -5 | sed 's/^/      /'
    
    # Thread performance
    echo ""
    echo "    Thread Performance:"
    grep -E "^.*T[0-9]+:" "$log_file" | head -8 | sed 's/^/      /'
    
    # Hash table health
    echo ""
    echo "    Hash Table Statistics:"
    grep -E "avg probe|max_probe|Hash resizes" "$log_file" | head -3 | sed 's/^/      /'
    
    # SIMD efficiency
    echo ""
    echo "    SIMD Efficiency:"
    simd=$(grep "SIMD chunks:" "$log_file" | grep -oE "[0-9]+" | head -1)
    scalar=$(grep "Scalar chunks:" "$log_file" | grep -oE "[0-9]+" | tail -1)
    if [ -n "$simd" ] && [ -n "$scalar" ] && [ "$scalar" -gt 0 ]; then
        ratio=$(echo "scale=2; $simd / $scalar" | bc)
        echo "      SIMD chunks: $simd"
        echo "      Scalar chunks: $scalar"
        echo "      Ratio: ${ratio}:1"
    else
        grep -E "SIMD chunks|Scalar chunks" "$log_file" | head -2 | sed 's/^/      /'
    fi
    
    # Memory statistics
    echo ""
    echo "    Memory Statistics:"
    grep -E "Memory:|Pool exhaustions:|leaked" "$log_file" | head -3 | sed 's/^/      /'
    
    # Performance timing
    echo ""
    echo "    Timing Breakdown:"
    grep -E "processing:|merge:|top-k:" "$log_file" | sed 's/^/      /'
    
    # Critical errors or warnings
    echo ""
    echo "    Issues:"
    grep -E "FATAL|ERROR|exhaustion|truncated" "$log_file" | head -5 | sed 's/^/      /'
    if [ $? -ne 0 ]; then
        echo "      ✓ No critical issues found"
    fi
    
    echo ""
}

# Function to run benchmark
run_bench() {
    local name=$1
    local cmd=$2
    local file=$3
    local is_debug=$4
    
    echo "Testing $name:"
    
    # Clear debug log before run if in debug mode
    if [ "$is_debug" = "1" ]; then
        rm -f wordcount_debug.log
    fi
    
    # Get output for validation
    if [ $VALIDATE_MODE -eq 1 ]; then
        $cmd "$file" > /tmp/bench_c_output_$$.txt 2>&1
        local total=$(grep "Total words:" /tmp/bench_c_output_$$.txt | sed 's/[^0-9]//g')
        local unique=$(grep "Unique words:" /tmp/bench_c_output_$$.txt | sed 's/[^0-9]//g')
        echo "  Words: $total total, $unique unique"
    fi
    
    local times=()
    for ((i=1; i<=NUM_RUNS; i++)); do
        if [ "$is_debug" = "1" ] && [ $i -eq 1 ] && [ $PROFILE_MODE -eq 1 ] && command -v perf &> /dev/null; then
            # Run with perf stat on first debug run only in profile mode
            echo "  Run $i: (with perf stat)"
            timeout 10 perf stat -d $cmd "$file" 2>&1 | grep -E "seconds time|cache|instructions|branches|LLC" | sed 's/^/    /'
            local result=$(/usr/bin/time -f "%e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
        elif command -v /usr/bin/time &> /dev/null; then
            # Try to use high-precision timing from the program itself if available
            local exec_time=$($cmd "$file" 2>&1 | grep "Execution time:" | awk '{print $3}')
            if [ -n "$exec_time" ]; then
                # Convert ms to seconds for consistency
                local result=$(echo "scale=6; $exec_time / 1000" | bc)
            else
                # Fall back to /usr/bin/time but with more precision
                local result=$(/usr/bin/time -f "%.3e" $cmd "$file" 2>&1 1>/dev/null | tail -n1)
            fi
        else
            # Use high-precision nanosecond timing
            local start=$(date +%s%N)
            $cmd "$file" > /dev/null 2>&1
            local end=$(date +%s%N)
            local result=$(echo "scale=6; ($end - $start) / 1000000000" | bc)
        fi
        
        times+=($result)
        if [ "$is_debug" != "1" ] || [ $i -ne 1 ]; then
            printf "  Run %d: %.3fs\n" "$i" "$result"
        fi
    done
    
    # Calculate average and min
    local sum=0
    local min=${times[0]}
    for t in "${times[@]}"; do
        sum=$(echo "$sum + $t" | bc)
        if (( $(echo "$t < $min" | bc -l) )); then
            min=$t
        fi
    done
    local avg=$(echo "scale=3; $sum / $NUM_RUNS" | bc)
    
    # Calculate throughput (handle very small times)
    if [ $(echo "$min > 0" | bc) -eq 1 ]; then
        local throughput=$(echo "scale=2; $FILE_SIZE_MB / $min" | bc)
    else
        local throughput="N/A"
    fi
    
    printf "  Average: %.3fs\n" "$avg"
    printf "  Best:    %.3fs\n" "$min"
    if [ "$throughput" = "N/A" ]; then
        echo "  Throughput: N/A (time too small to measure)"
    else
        printf "  Throughput: %.2f MB/s\n" "$throughput"
    fi
    
    # Analyze debug log if in debug mode
    if [ "$is_debug" = "1" ] && [ -f "wordcount_debug.log" ]; then
        # Save the log with variant name
        cp wordcount_debug.log "/tmp/${name// /_}_debug.log"
        echo ""
        analyze_debug_log "wordcount_debug.log" "$name"
    fi
    
    echo ""
    
    echo "$name|$avg|$min|$throughput" >> /tmp/bench_c_results_$$.txt
}

# Run benchmarks
echo "========================================="
echo "Running Benchmarks ($NUM_RUNS runs each)"
echo "========================================="
echo ""

rm -f /tmp/bench_c_results_$$.txt

# Always run reference
run_bench "C Reference" "./wordcount_c_ref" "book.txt" "0"

# Run all built variants
for i in "${!BUILD_VARIANTS[@]}"; do
    is_debug="0"
    if [[ "${BUILD_NAMES[$i]}" == *"Debug"* ]]; then
        is_debug="1"
    fi
    run_bench "${BUILD_NAMES[$i]}" "${BUILD_VARIANTS[$i]}" "book.txt" "$is_debug"
done

# Additional debug analysis if in debug mode
if [ $DEBUG_MODE -eq 1 ]; then
    echo "========================================="
    echo "Debug Mode Analysis Summary"
    echo "========================================="
    echo ""
    
    # Show the debug log if it exists
    if [ -f "wordcount_debug.log" ]; then
        echo "Debug Log Output (wordcount_debug.log):"
        echo "────────────────────────────────────────"
        cat wordcount_debug.log
        echo ""
    fi
    
    # Compare debug logs if multiple variants
    if [ ${#BUILD_VARIANTS[@]} -gt 1 ]; then
        echo "Comparative Analysis:"
        echo "────────────────────"
        
        # Extract key metrics from all debug logs
        for name in "${BUILD_NAMES[@]}"; do
            log_file="/tmp/${name// /_}_debug.log"
            if [ -f "$log_file" ]; then
                echo ""
                echo "$name:"
                
                # Processing time
                proc_time=$(grep "processing:" "$log_file" | grep -oE "[0-9]+\.[0-9]+" | head -1)
                merge_time=$(grep "merge:" "$log_file" | grep -oE "[0-9]+\.[0-9]+" | head -1)
                
                if [ -n "$proc_time" ] && [ -n "$merge_time" ]; then
                    echo "  Processing: ${proc_time}ms"
                    echo "  Merge: ${merge_time}ms"
                    ratio=$(echo "scale=2; $proc_time / $merge_time" | bc 2>/dev/null || echo "N/A")
                    [ "$ratio" != "N/A" ] && echo "  Proc/Merge ratio: ${ratio}:1"
                fi
                
                # Thread imbalance
                max_thread_time=$(grep "^.*T[0-9]+:" "$log_file" | grep -oE "[0-9]+\.[0-9]+ms" | sed 's/ms//' | sort -rn | head -1)
                min_thread_time=$(grep "^.*T[0-9]+:" "$log_file" | grep -oE "[0-9]+\.[0-9]+ms" | sed 's/ms//' | sort -n | head -1)
                
                if [ -n "$max_thread_time" ] && [ -n "$min_thread_time" ] && [ "$min_thread_time" != "0" ]; then
                    imbalance=$(echo "scale=2; $max_thread_time / $min_thread_time" | bc 2>/dev/null || echo "N/A")
                    [ "$imbalance" != "N/A" ] && echo "  Thread imbalance: ${imbalance}x"
                fi
                
                # Hash efficiency
                avg_probe=$(grep "Overall avg probe" "$log_file" | grep -oE "[0-9]+\.[0-9]+" | head -1)
                [ -n "$avg_probe" ] && echo "  Avg probe length: $avg_probe"
            fi
        done
    fi
    
    echo ""
    echo "Performance Bottleneck Analysis:"
    echo "────────────────────────────────"
    
    # Analyze the best performing debug variant
    best_debug_log="/tmp/Debug_Standard_debug.log"
    if [ -f "$best_debug_log" ]; then
        proc_time=$(grep "processing:" "$best_debug_log" | grep -oE "[0-9]+\.[0-9]+" | head -1)
        merge_time=$(grep "merge:" "$best_debug_log" | grep -oE "[0-9]+\.[0-9]+" | head -1)
        
        if [ -n "$proc_time" ] && [ -n "$merge_time" ]; then
            total_time=$(echo "$proc_time + $merge_time" | bc)
            proc_pct=$(echo "scale=1; $proc_time * 100 / $total_time" | bc)
            merge_pct=$(echo "scale=1; $merge_time * 100 / $total_time" | bc)
            
            echo "  Processing phase: ${proc_pct}% of time"
            echo "  Merge phase: ${merge_pct}% of time"
            
            if (( $(echo "$merge_pct > 30" | bc -l) )); then
                echo "  → Merge phase is a bottleneck (>30%)"
                echo "    Consider parallel merge implementation"
            fi
            
            if (( $(echo "$proc_pct > 80" | bc -l) )); then
                echo "  → Processing dominates (>80%)"
                echo "    Focus on SIMD and threading optimizations"
            fi
        fi
    fi
fi

# Results summary
echo ""
echo "========================================="
echo "Performance Summary"
echo "========================================="
echo ""

# Parse and display results
echo "Implementation         Average    Best     Throughput   vs Reference"
echo "─────────────────────────────────────────────────────────────────────"

# Get reference time
ref_avg=$(grep "C Reference" /tmp/bench_c_results_$$.txt | cut -d'|' -f2)
ref_min=$(grep "C Reference" /tmp/bench_c_results_$$.txt | cut -d'|' -f3)

while IFS='|' read -r name avg min throughput; do
    if [ "$name" = "C Reference" ]; then
        printf "%-20s %8.3fs %8.3fs %8.2f MB/s   baseline\n" "$name" "$avg" "$min" "$throughput"
    else
        speedup=$(echo "scale=2; $ref_avg / $avg" | bc)
        printf "%-20s %8.3fs %8.3fs %8.2f MB/s   %.2fx\n" "$name" "$avg" "$min" "$throughput" "$speedup"
    fi
done < /tmp/bench_c_results_$$.txt

echo ""

# Identify performance issues
echo "========================================="
echo "Performance Analysis"
echo "========================================="
echo ""

# Get best throughput
best_throughput=0
best_impl=""
while IFS='|' read -r name avg min throughput; do
    if (( $(echo "$throughput > $best_throughput" | bc -l) )); then
        best_throughput=$throughput
        best_impl=$name
    fi
done < /tmp/bench_c_results_$$.txt

echo "Best performer: $best_impl"
echo "Peak throughput: ${best_throughput} MB/s"
echo ""

# Analyze throughput vs target
target_throughput=3000  # 3 GB/s target
throughput_pct=$(echo "scale=1; $best_throughput * 100 / $target_throughput" | bc)

echo "Target Analysis:"
echo "  Target throughput: 3.0 GB/s"
echo "  Achieved: ${best_throughput} MB/s (${throughput_pct}% of target)"
echo ""

if (( $(echo "$best_throughput < 1000" | bc -l) )); then
    echo "Performance Issues Detected:"
    echo "  ✗ Throughput is significantly below target (<1 GB/s)"
    echo ""
    echo "Potential causes:"
    echo "  1. Thread synchronization overhead"
    echo "  2. Memory bandwidth bottleneck"
    echo "  3. Hash table collisions/resizing"
    echo "  4. Inefficient SIMD usage"
    echo "  5. Cache misses"
    echo ""
    echo "Recommendations:"
    echo "  - Profile cache behavior with --profile"
    echo "  - Analyze debug logs with -d flag"
    echo "  - Consider memory prefetching patterns"
elif (( $(echo "$best_throughput < 2000" | bc -l) )); then
    echo "Performance is moderate (1-2 GB/s)"
    echo "Optimization opportunities:"
    echo "  - Fine-tune thread count"
    echo "  - Optimize merge phase"
    echo "  - Improve memory locality"
else
    echo "Good performance (>2 GB/s)"
    echo "Minor optimizations may help reach 3 GB/s target"
fi

# CPU feature check
echo ""
echo "CPU Capabilities:"
if grep -q avx512 /proc/cpuinfo 2>/dev/null; then
    echo "  ✓ AVX-512 available"
else
    echo "  ✗ AVX-512 not available (may limit performance)"
fi

if grep -q sse4_2 /proc/cpuinfo 2>/dev/null; then
    echo "  ✓ SSE4.2 (CRC32C) available"
else
    echo "  ✗ SSE4.2 not available"
fi

# Check for Zen 5 specific features
if lscpu 2>/dev/null | grep -q "Zen 5\|9950X3D"; then
    echo "  ✓ AMD Zen 5 architecture detected (optimized for 8 threads)"
fi

# Cleanup
echo ""
# Skip prompt if --no-cleanup flag is set
if [ "$NO_CLEANUP" -eq 1 ]; then
    response="n"
    echo "Skipping cleanup (--no-cleanup flag set)"
elif [ -t 0 ]; then
    # Interactive mode - prompt user
    echo "Clean up test binaries, logs, and output files? (y/n)"
    read -r response
else
    # Non-interactive mode - try to read response from stdin
    read -r response 2>/dev/null || response="n"
fi

if [[ "$response" =~ ^[Yy]$ ]]; then
    # Remove compiled binaries
    rm -f wordcount_c_ref wordcount_hopt wordcount_hopt_t*
    rm -f wordcount_debug wordcount_debug_t* wordcount_hopt_opt
    rm -f wordcount_asan
    
    # Remove temporary files and logs
    rm -f /tmp/bench_c_*_$$.txt /tmp/*_debug.log
    rm -f perf.data perf.data.old
    rm -f wordcount_debug.log
    rm -f *.gcda *.gcno
    
    # Remove result files generated during testing
    rm -f book_c-hopt_results.txt
    rm -f book_c_results.txt
    
    # Remove any debug bundle if created
    rm -rf debug_bundle
    rm -f debug_bundle.b64
    rm -f debug_bundle_part_*
    
    echo "✓ Cleaned up all files"
else
    echo "✓ Keeping all files"
fi