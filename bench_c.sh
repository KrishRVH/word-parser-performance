#!/bin/bash
# bench_c.sh - C word counter benchmark tool
# Compares: reference (wordcount.c), new hyperopt, and old hyperopt
#
# Usage:
#   ./bench_c.sh                           # Full comparison
#   ./bench_c.sh --hyperonly               # Skip reference
#   ./bench_c.sh --large --runs=10         # Test with larger files
#   ./bench_c.sh --scan-threads=4,6,8,12   # Test different thread counts
#   ./bench_c.sh --pin=0-5                 # Pin to specific CPUs

export LC_ALL=C LANG=C

# Defaults
NUM_RUNS=5
HYPER_ONLY=0
INPUT_FILE="book.txt"
PIN_MASK=""
SCAN_THREADS=""
LARGE_MODE=0
NO_CLEANUP=0
VALIDATE_MODE=0

# File names
REF_FILE="wordcount.c"
HYPEROPT_FILE="wordcount_hyperopt.c"
HYPEROPT_OLD_FILE="wordcount_hyperopt_old.c"

# Parse arguments
for arg in "$@"; do
    case $arg in
        --runs=*)       NUM_RUNS="${arg#*=}" ;;
        --hyperonly)    HYPER_ONLY=1 ;;
        --file=*)       INPUT_FILE="${arg#*=}" ;;
        --pin=*)        PIN_MASK="${arg#*=}" ;;
        --scan-threads=*) SCAN_THREADS="${arg#*=}" ;;
        --large)        LARGE_MODE=1 ;;
        --no-cleanup)   NO_CLEANUP=1 ;;
        --validate)     VALIDATE_MODE=1 ;;
        --help)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --runs=N            Number of benchmark runs (default: 5)
  --hyperonly         Skip reference implementation
  --file=PATH         Input file (default: book.txt)
  --pin=MASK          CPU affinity mask (e.g., 0-5, 0,2,4)
  --scan-threads=CSV  Test multiple thread counts (e.g., 4,6,8,12)
  --large             Create and test 5x and 25x larger files
  --validate          Show word count output
  --no-cleanup        Keep binaries after run

Files tested:
  wordcount.c              - Reference parallel implementation
  wordcount_hyperopt.c     - New optimized version (cleaned up)
  wordcount_hyperopt_old.c - Original LLM-generated hyperopt

Examples:
  $0 --large --runs=10
  $0 --hyperonly --scan-threads=4,6,8,12 --pin=0-5
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $arg (use --help)"
            exit 1
            ;;
    esac
done

echo "========================================="
echo "C Word Counter Benchmark"
echo "========================================="
echo ""

# Check prerequisites
if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc is not installed"
    exit 1
fi

if ! command -v bc >/dev/null 2>&1; then
    echo "Error: bc is not installed"
    exit 1
fi

# ============================================================================
# Detect CPU architecture and build flags (matches original bench_c.sh)
# ============================================================================

GCC_VER=$(gcc -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)
AVX_FLAGS="-mavx512f -mavx512bw -mavx512vl -msse4.2"

if [ -n "$GCC_VER" ] && [ "$GCC_VER" -ge 14 ]; then
    MARCH_FLAGS="-march=znver5 -mtune=znver5 $AVX_FLAGS"
elif [ -n "$GCC_VER" ] && [ "$GCC_VER" -ge 12 ]; then
    # Let GCC detect the arch but force AVX flags
    MARCH_FLAGS="-march=native -mtune=native $AVX_FLAGS"
else
    MARCH_FLAGS="-march=native -mtune=native"
fi

echo "GCC version: $GCC_VER"
echo "Build flags: $MARCH_FLAGS"
echo ""

# ============================================================================
# Download primary test file if missing
# ============================================================================

if [ ! -f "$INPUT_FILE" ]; then
    echo "Downloading test file..."
    curl -sL "https://www.gutenberg.org/files/2701/2701-0.txt" -o "$INPUT_FILE"
    echo "✓ Downloaded $INPUT_FILE"
fi

# Show primary file info
FILE_SIZE=$(du -h "$INPUT_FILE" | cut -f1)
WORD_COUNT=$(wc -w < "$INPUT_FILE")
echo "Primary test file:"
printf "  %-20s %6s  %'d words\n" "$INPUT_FILE" "$FILE_SIZE" "$WORD_COUNT"
echo ""

# ============================================================================
# Large file creation (like original bench.sh)
# ============================================================================

create_multiplied_file() {
    local base=$1
    local out=$2
    local mult=$3
    
    if [ -f "$out" ]; then
        echo "✓ Using existing $out"
        return
    fi
    
    echo "Creating $out (${mult}x of $base)..."
    : > "$out"
    for _ in $(seq 1 "$mult"); do
        cat "$base" >> "$out"
    done
    local size
    size=$(du -h "$out" | cut -f1)
    echo "✓ Created $out ($size)"
}

# Build input file list
INPUT_FILES=("$INPUT_FILE")

if [ $LARGE_MODE -eq 1 ]; then
    echo "Creating larger test files..."
    create_multiplied_file "$INPUT_FILE" "book2.txt" 5
    create_multiplied_file "$INPUT_FILE" "book3.txt" 25
    INPUT_FILES+=("book2.txt" "book3.txt")
    echo ""
fi

# Show all test files
echo "Test files:"
for f in "${INPUT_FILES[@]}"; do
    if [ -f "$f" ]; then
        size=$(du -h "$f" | cut -f1)
        words=$(wc -w < "$f")
        printf "  %-20s %6s  %'d words\n" "$f" "$size" "$words"
    fi
done
[ -n "$PIN_MASK" ] && echo "CPU pin mask: $PIN_MASK"
echo ""

# Results file
RESULTS_FILE="/tmp/bench_c_results_$$.txt"
rm -f "$RESULTS_FILE"

# ============================================================================
# Build Functions
# ============================================================================

build_reference() {
    echo "Building reference ($REF_FILE)..."
    if gcc -O3 -std=c11 -march=native -mtune=native -flto -pthread \
           "$REF_FILE" -o wordcount_ref 2>/dev/null; then
        echo "✓ Reference build successful"
        return 0
    else
        echo "✗ Reference build failed"
        gcc -O3 -std=c11 -march=native -pthread "$REF_FILE" -o wordcount_ref 2>&1 | head -5
        return 1
    fi
}

build_hyperopt_new() {
    local threads=$1
    local output="wordcount_hopt_t${threads}"
    
    echo "Building $HYPEROPT_FILE (${threads} threads)..."
    if gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
           -DNUM_THREADS="$threads" \
           "$HYPEROPT_FILE" -o "$output" 2>/dev/null; then
        echo "✓ New hyperopt ${threads}-thread build successful"
        return 0
    else
        echo "✗ New hyperopt ${threads}-thread build failed"
        gcc -O3 $MARCH_FLAGS -pthread -DNUM_THREADS="$threads" \
            "$HYPEROPT_FILE" -o "$output" 2>&1 | head -5
        return 1
    fi
}

build_hyperopt_old() {
    local threads=$1
    local output="wordcount_hopt_old_t${threads}"
    
    echo "Building $HYPEROPT_OLD_FILE (${threads} threads)..."
    # Old version needs -lm for math functions
    if gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
           -DNUM_THREADS="$threads" \
           "$HYPEROPT_OLD_FILE" -o "$output" -lm 2>/dev/null; then
        echo "✓ Old hyperopt ${threads}-thread build successful"
        return 0
    else
        echo "✗ Old hyperopt ${threads}-thread build failed"
        gcc -O3 $MARCH_FLAGS -pthread -DNUM_THREADS="$threads" \
            "$HYPEROPT_OLD_FILE" -o "$output" -lm 2>&1 | head -5
        return 1
    fi
}

# ============================================================================
# Benchmark Function
# ============================================================================

run_bench() {
    local name="$1"
    local cmd="$2"
    local file="$3"
    
    if [ ! -f "$file" ]; then
        echo "Skipping: $file not found"
        return
    fi
    
    if [ ! -x "$cmd" ]; then
        echo "Skipping: $cmd not executable"
        return
    fi
    
    local shortf
    shortf=$(basename "$file")
    local file_size
    file_size=$(du -h "$file" | cut -f1)
    
    echo "Testing: $name ($shortf - $file_size)"
    
    # Setup taskset if requested
    local runner=""
    if [ -n "$PIN_MASK" ] && command -v taskset >/dev/null 2>&1; then
        runner="taskset -c $PIN_MASK"
    fi
    
    # Get file size in bytes
    local fs_bytes
    fs_bytes=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null)
    local fs_mb
    fs_mb=$(echo "scale=6; $fs_bytes / 1048576" | bc)
    
    # Run benchmarks
    local times=()
    local sum="0"
    local min=""
    local is_tty=0
    [ -t 1 ] && is_tty=1
    
    local run
    for ((run = 1; run <= NUM_RUNS; run++)); do
        local start_ns end_ns elapsed
        start_ns=$(date +%s%N)
        $runner "$cmd" "$file" > /dev/null 2>&1
        end_ns=$(date +%s%N)
        
        elapsed=$(echo "scale=6; ($end_ns - $start_ns) / 1000000000" | bc)
        times+=("$elapsed")
        sum=$(echo "scale=6; $sum + $elapsed" | bc)
        
        if [ -z "$min" ] || (( $(echo "$elapsed < $min" | bc -l) )); then
            min="$elapsed"
        fi
        
        local avg_so_far
        avg_so_far=$(echo "scale=3; $sum / $run" | bc)
        
        if [ $is_tty -eq 1 ]; then
            printf "\r  Run %d/%d: %.3fs (best: %.3fs, avg: %.3fs)\033[K" \
                "$run" "$NUM_RUNS" "$elapsed" "$min" "$avg_so_far"
        fi
    done
    
    [ $is_tty -eq 1 ] && printf "\n"
    
    # Calculate statistics
    local avg
    avg=$(echo "scale=6; $sum / $NUM_RUNS" | bc)
    
    # Sort for percentiles
    local sorted
    sorted=$(printf "%s\n" "${times[@]}" | sort -n)
    local p50 p95
    p50=$(echo "$sorted" | sed -n "$((NUM_RUNS * 50 / 100 + 1))p")
    p95=$(echo "$sorted" | sed -n "$((NUM_RUNS * 95 / 100 + 1))p")
    
    # Throughput based on best time
    local throughput="N/A"
    if (( $(echo "$min > 0" | bc -l) )); then
        throughput=$(echo "scale=2; $fs_mb / $min" | bc)
    fi
    
    printf "  Average: %.3fs | Best: %.3fs | p50: %.3fs | p95: %.3fs\n" \
        "$avg" "$min" "$p50" "$p95"
    printf "  Throughput: %.2f MB/s\n" "$throughput"
    echo ""
    
    # Record results
    echo "${name}[${shortf}]|$avg|$min|$throughput" >> "$RESULTS_FILE"
}

# ============================================================================
# Main Build Phase
# ============================================================================

echo "========================================="
echo "Building"
echo "========================================="
echo ""

declare -a BUILDS=()
declare -a BUILD_NAMES=()

# Build reference if not hyper-only
if [ $HYPER_ONLY -eq 0 ] && [ -f "$REF_FILE" ]; then
    if build_reference; then
        BUILDS+=("./wordcount_ref")
        BUILD_NAMES+=("Reference")
    fi
fi

# Determine thread counts to test
THREAD_COUNTS=(6 12)  # Default

if [ -n "$SCAN_THREADS" ]; then
    IFS=',' read -ra THREAD_COUNTS <<< "$SCAN_THREADS"
fi

# Build NEW hyperopt variants
if [ -f "$HYPEROPT_FILE" ]; then
    echo ""
    for t in "${THREAD_COUNTS[@]}"; do
        t=$(echo "$t" | tr -d ' ')
        if build_hyperopt_new "$t"; then
            BUILDS+=("./wordcount_hopt_t${t}")
            BUILD_NAMES+=("New Hyperopt (${t}T)")
        fi
    done
else
    echo "Note: $HYPEROPT_FILE not found"
fi

# Build OLD hyperopt variants
if [ -f "$HYPEROPT_OLD_FILE" ]; then
    echo ""
    echo "Found $HYPEROPT_OLD_FILE - building for comparison..."
    for t in "${THREAD_COUNTS[@]}"; do
        t=$(echo "$t" | tr -d ' ')
        if build_hyperopt_old "$t"; then
            BUILDS+=("./wordcount_hopt_old_t${t}")
            BUILD_NAMES+=("Old Hyperopt (${t}T)")
        fi
    done
else
    echo "Note: $HYPEROPT_OLD_FILE not found (skipping old version comparison)"
fi

if [ ${#BUILDS[@]} -eq 0 ]; then
    echo "Error: No implementations built successfully"
    exit 1
fi

echo ""
echo "========================================="
echo "Warming Up"
echo "========================================="

# Warm filesystem cache
for f in "${INPUT_FILES[@]}"; do
    [ -f "$f" ] && cat "$f" > /dev/null
done

# Warm CPU with first build
if [ -f "$INPUT_FILE" ]; then
    "${BUILDS[0]}" "$INPUT_FILE" > /dev/null 2>&1 || true
fi
echo "✓ Cache warmed"
echo ""

echo "========================================="
echo "Benchmarking ($NUM_RUNS runs each)"
echo "========================================="
echo ""

# Run all benchmarks
for idx in "${!BUILDS[@]}"; do
    for f in "${INPUT_FILES[@]}"; do
        run_bench "${BUILD_NAMES[$idx]}" "${BUILDS[$idx]}" "$f"
    done
done

# Show validation output if requested
if [ $VALIDATE_MODE -eq 1 ]; then
    echo "========================================="
    echo "Validation Output"
    echo "========================================="
    echo ""
    
    # Show output from each implementation type (first of each)
    for idx in "${!BUILD_NAMES[@]}"; do
        name="${BUILD_NAMES[$idx]}"
        # Only show first occurrence of each type
        case "$name" in
            "Reference")
                echo "--- Reference ---"
                "${BUILDS[$idx]}" "$INPUT_FILE" 2>&1 | head -20
                echo ""
                ;;
            "New Hyperopt (6T)")
                echo "--- New Hyperopt ---"
                "${BUILDS[$idx]}" "$INPUT_FILE" 2>&1 | head -20
                echo ""
                ;;
            "Old Hyperopt (6T)")
                echo "--- Old Hyperopt ---"
                "${BUILDS[$idx]}" "$INPUT_FILE" 2>&1 | head -20
                echo ""
                ;;
        esac
    done
fi

echo "========================================="
echo "Summary"
echo "========================================="
echo ""

printf "%-28s %10s %10s %12s %8s\n" "Implementation" "Average" "Best" "Throughput" "vs Ref"
printf "%-28s %10s %10s %12s %8s\n" "----------------------------" "----------" "----------" "------------" "--------"

# Get reference time for comparison (per file)
declare -A REF_AVG
while IFS='|' read -r name avg best throughput; do
    if [[ "$name" == Reference* ]]; then
        REF_AVG["$name"]="$avg"
    fi
done < "$RESULTS_FILE"

# Print results
while IFS='|' read -r name avg best throughput; do
    speedup="-"
    
    # Extract file from name like "New Hyperopt (6T)[book2.txt]"
    file_tag=$(echo "$name" | grep -oP '\[.*\]' || echo "[${INPUT_FILE}]")
    ref_key="Reference${file_tag}"
    ref_avg="${REF_AVG[$ref_key]:-}"
    
    if [[ "$name" == Reference* ]]; then
        speedup="baseline"
    elif [ -n "$ref_avg" ] && (( $(echo "$avg > 0" | bc -l) )); then
        speedup=$(echo "scale=2; $ref_avg / $avg" | bc)
        speedup="${speedup}x"
    fi
    
    printf "%-28s %9.3fs %9.3fs %10.2f MB/s %8s\n" \
        "$name" "$avg" "$best" "$throughput" "$speedup"
done < "$RESULTS_FILE"

echo ""

# ============================================================================
# Performance Analysis
# ============================================================================

echo "========================================="
echo "Performance Analysis"
echo "========================================="
echo ""

# Find best performer per file
echo "Best performers by file:"
for f in "${INPUT_FILES[@]}"; do
    shortf=$(basename "$f")
    best_tp=0
    best_name=""
    while IFS='|' read -r name _ _ tp; do
        if [[ "$name" == *"[$shortf]"* ]] && [[ "$tp" =~ ^[0-9.]+$ ]]; then
            if (( $(echo "$tp > $best_tp" | bc -l) )); then
                best_tp=$tp
                best_name=$name
            fi
        fi
    done < "$RESULTS_FILE"
    if [ -n "$best_name" ]; then
        printf "  %-12s: %-28s @ %.2f MB/s\n" "$shortf" "$best_name" "$best_tp"
    fi
done
echo ""

# Overall best
best_throughput=0
best_impl=""
while IFS='|' read -r name avg min throughput; do
    if [[ "$throughput" =~ ^[0-9.]+$ ]] && \
       (( $(echo "$throughput > $best_throughput" | bc -l) )); then
        best_throughput=$throughput
        best_impl=$name
    fi
done < "$RESULTS_FILE"

echo "Overall best: $best_impl"
echo "Peak throughput: ${best_throughput} MB/s"
echo ""

# Compare new vs old hyperopt if both present
new_best=0
old_best=0
while IFS='|' read -r name _ _ tp; do
    if [[ "$name" == "New Hyperopt"* ]] && [[ "$tp" =~ ^[0-9.]+$ ]]; then
        if (( $(echo "$tp > $new_best" | bc -l) )); then
            new_best=$tp
        fi
    fi
    if [[ "$name" == "Old Hyperopt"* ]] && [[ "$tp" =~ ^[0-9.]+$ ]]; then
        if (( $(echo "$tp > $old_best" | bc -l) )); then
            old_best=$tp
        fi
    fi
done < "$RESULTS_FILE"

if (( $(echo "$new_best > 0 && $old_best > 0" | bc -l) )); then
    echo "New vs Old Hyperopt comparison:"
    printf "  New best: %.2f MB/s\n" "$new_best"
    printf "  Old best: %.2f MB/s\n" "$old_best"
    if (( $(echo "$new_best > $old_best" | bc -l) )); then
        diff=$(echo "scale=1; ($new_best - $old_best) * 100 / $old_best" | bc)
        echo "  → New is ${diff}% faster"
    else
        diff=$(echo "scale=1; ($old_best - $new_best) * 100 / $new_best" | bc)
        echo "  → Old is ${diff}% faster"
    fi
    echo ""
fi

# ============================================================================
# Cleanup
# ============================================================================

if [ $NO_CLEANUP -eq 0 ]; then
    if [ -t 0 ]; then
        read -rp "Clean up binaries? (y/n) " response
    else
        response="n"
    fi
    
    if [[ "$response" =~ ^[Yy]$ ]]; then
        rm -f wordcount_ref wordcount_hopt_t* wordcount_hopt_old_t* "$RESULTS_FILE"
        rm -f *_c-hopt_results.txt  # Old hyperopt writes these
        
        if [ $LARGE_MODE -eq 1 ]; then
            read -rp "Also remove book2.txt and book3.txt? (y/n) " response2
            if [[ "$response2" =~ ^[Yy]$ ]]; then
                rm -f book2.txt book3.txt
            fi
        fi
        echo "✓ Cleaned up"
    fi
else
    echo "Binaries kept (--no-cleanup)"
fi