#!/bin/bash
# bench_c.sh - C implementation comparison and optimization tool
# Usage:
#   ./bench_c.sh --hyperonly --large --runs=10 --pin=0-23
#   ./bench_c.sh --hyperonly --large --scan-threads=6,8,12,16 --runs=10 --pin=0-23
#   ./bench_c.sh -d --hyperonly --large --profile \
#     --events="cycles,instructions,cache-misses,LLC-loads,LLC-load-misses" \
#     --pin=0-23 --runs=3 --bundle

export LC_ALL=C LANG=C

NUM_RUNS=5
DEBUG_MODE=0
PROFILE_MODE=0
VALIDATE_MODE=0
THREAD_COUNT=""
NO_CLEANUP=0
HYPER_ONLY=0
INPUT_FILE="book.txt"
PERF_EVENTS="cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,dTLB-loads,dTLB-load-misses"
PIN_MASK=""
BUNDLE=0
SCAN_THREADS=""
LARGE_MODE=0

for arg in "$@"; do
    case $arg in
        --runs=*) NUM_RUNS="${arg#*=}"; shift ;;
        -d|--debug) DEBUG_MODE=1; shift ;;
        --profile) PROFILE_MODE=1; shift ;;
        --validate) VALIDATE_MODE=1; shift ;;
        --threads=*) THREAD_COUNT="${arg#*=}"; shift ;;
        --no-cleanup) NO_CLEANUP=1; shift ;;
        --hyperonly) HYPER_ONLY=1; shift ;;
        --file=*) INPUT_FILE="${arg#*=}"; shift ;;
        --events=*) PERf_EVENTS="${arg#*=}"; PERF_EVENTS="$PERf_EVENTS"; shift ;;
        --pin=*) PIN_MASK="${arg#*=}"; shift ;;
        --bundle) BUNDLE=1; shift ;;
        --scan-threads=*) SCAN_THREADS="${arg#*=}"; shift ;;
        --large) LARGE_MODE=1; shift ;;
        --help)
            cat <<EOF
Usage: $0 [--runs=N] [-d|--debug] [--profile] [--validate] [--threads=N] [--hyperonly]
          [--file=PATH] [--events=LIST] [--pin=MASK] [--bundle] [--scan-threads=CSV] [--large] [--no-cleanup]
  --runs=N        Number of runs per variant (default: 5)
  -d, --debug     Build DEBUG variant and collect detailed metrics
  --profile       perf stat on first debug run per variant
  --validate      Print word counts
  --threads=N     Custom NUM_THREADS build (adds to release set)
  --hyperonly     Skip reference C build/run
  --file=PATH     Primary input file (default: book.txt)
  --events=LIST   perf stat events (comma-separated)
  --pin=MASK      Pin runs via taskset (e.g., 0-23)
  --bundle        Write debug_bundle.tar.gz with logs and sysinfo
  --scan-threads=CSV  Build/run multiple thread counts (e.g. 6,8,12,16)
  --large         Also test book2.txt and book3.txt if present
EOF
            exit 0
            ;;
    esac
done

echo "========================================="
echo "C Implementation Comparison Tool"
echo "========================================="
echo ""

if [ $HYPER_ONLY -eq 1 ]; then
    echo "HYPER-ONLY MODE ENABLED"
    [ -n "$THREAD_COUNT" ] && echo "  - Testing with $THREAD_COUNT threads"
    echo ""
fi

if [ $DEBUG_MODE -eq 1 ]; then
    echo "DEBUG MODE ENABLED"
    echo "  - Full instrumentation"
    echo "  - Debug logs will be collected"
    echo "  - Performance analysis will be detailed"
    [ -n "$THREAD_COUNT" ] && echo "  - Testing with $THREAD_COUNT threads"
    echo ""
fi

echo "Comparing:"
if [ $HYPER_ONLY -eq 0 ]; then echo "  - wordcount.c (reference implementation)"; fi
echo "  - wordcount_hyperopt.c (hyperoptimized version)"
echo ""

if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc is not installed"
    exit 1
fi

# pick march flags
GCC_VER=$(gcc -dumpfullversion -dumpversion | cut -d. -f1)
AVX_FLAGS="-mavx512f -mavx512bw -mavx512vl -msse4.2"
if [ -n "$GCC_VER" ] && [ "$GCC_VER" -ge 14 ]; then
    MARCH_FLAGS="-march=znver5 -mtune=znver5 $AVX_FLAGS"
else
    # fallback for older compilers
    if lscpu 2>/dev/null | grep -qi "znver4"; then
        MARCH_FLAGS="-march=znver4 -mtune=znver4 $AVX_FLAGS"
    else
        MARCH_FLAGS="-march=native -mtune=native"
    fi
fi

# Select files
INPUT_FILES=("$INPUT_FILE")
if [ $LARGE_MODE -eq 1 ]; then
    INPUT_FILES+=("book2.txt" "book3.txt")
fi

# Ensure primary exists
if [ ! -f "$INPUT_FILE" ]; then
    echo "Downloading test file..."
    curl -s https://www.gutenberg.org/files/2701/2701-0.txt -o "$INPUT_FILE"
    echo "✓ Downloaded $INPUT_FILE"
fi

echo "Test files:"
for f in "${INPUT_FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "  - $f (missing, will skip)"
        continue
    fi
    FS_BYTES=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
    FS_H=$(du -h "$f" | cut -f1)
    WC=$(wc -w < "$f")
    printf "  - %s (%s, %'d words)\n" "$f" "$FS_H" "$WC"
done
[ -n "$PIN_MASK" ] && echo "CPU pin mask: $PIN_MASK"
echo ""

rm -f wordcount_debug.log wordcount_debug_*.log

# Build reference
echo "Building reference C implementation..."
if [ $DEBUG_MODE -eq 1 ] && [ $HYPER_ONLY -eq 0 ]; then
    gcc -O2 -g -fno-omit-frame-pointer -march=native \
        wordcount.c -o wordcount_c_ref 2>/dev/null
else
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
        wordcount.c -o wordcount_c_ref 2>/dev/null
fi
if [ $HYPER_ONLY -eq 0 ]; then
  if [ $? -eq 0 ]; then
      echo "✓ Reference build successful"
  else
      echo "✗ Reference build failed"
  fi
fi

declare -a BUILD_VARIANTS
declare -a BUILD_NAMES

if [ $DEBUG_MODE -eq 1 ]; then
    echo ""
    echo "Building debug variants..."
    echo "  Building optimized debug version (O2 + DEBUG) ..."
    if lscpu 2>/dev/null | grep -q "Zen 5\|9950X3D\|znver5"; then
        gcc -O2 -g -DDEBUG -fno-omit-frame-pointer $MARCH_FLAGS -pthread \
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

    if [ -n "$THREAD_COUNT" ]; then
        echo "  Building debug with $THREAD_COUNT threads..."
        gcc -O2 -g -DDEBUG -fno-omit-frame-pointer $MARCH_FLAGS -pthread \
            -DNUM_THREADS=$THREAD_COUNT wordcount_hyperopt.c -o wordcount_debug_t$THREAD_COUNT -lm 2>/dev/null
        if [ $? -eq 0 ]; then
            BUILD_VARIANTS+=("./wordcount_debug_t$THREAD_COUNT")
            BUILD_NAMES+=("Debug ${THREAD_COUNT}-thread")
            echo "    ✓ ${THREAD_COUNT}-thread debug build successful"
        fi
    fi

    echo "  Building optimized (O3) for comparison..."
    gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
        wordcount_hyperopt.c -o wordcount_hopt_opt -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_hopt_opt")
        BUILD_NAMES+=("Optimized (O3)")
        echo "    ✓ Optimized build successful"
    fi
else
    echo "Building hyperopt release set (6-thread, 12-thread)..."
    gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
        -DNUM_THREADS=6 wordcount_hyperopt.c -o wordcount_hopt_t6 -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_hopt_t6")
        BUILD_NAMES+=("C Hyperopt (6-thread)")
        echo "✓ 6-thread build successful"
    else
        echo "✗ 6-thread build failed"; exit 1
    fi
    gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
        -DNUM_THREADS=12 wordcount_hyperopt.c -o wordcount_hopt_t12 -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        BUILD_VARIANTS+=("./wordcount_hopt_t12")
        BUILD_NAMES+=("C Hyperopt (12-thread)")
        echo "✓ 12-thread build successful"
    else
        echo "✗ 12-thread build failed"; exit 1
    fi

    # Additional scan if requested
    if [ -n "$SCAN_THREADS" ]; then
        OLDIFS="$IFS"; IFS=','; read -ra TLIST <<< "$SCAN_THREADS"; IFS="$OLDIFS"
        for t in "${TLIST[@]}"; do
            ttrim=$(echo "$t" | tr -d ' ')
            if [ -n "$ttrim" ] && [ "$ttrim" != "6" ] && [ "$ttrim" != "12" ]; then
                echo "Building hyperopt with ${ttrim} threads..."
                gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
                    -DNUM_THREADS=$ttrim wordcount_hyperopt.c -o wordcount_hopt_t$ttrim -lm 2>/dev/null
                if [ $? -eq 0 ]; then
                    BUILD_VARIANTS+=("./wordcount_hopt_t$ttrim")
                    BUILD_NAMES+=("C Hyperopt (${ttrim}-thread)")
                    echo "✓ ${ttrim}-thread build successful"
                fi
            fi
        done
    fi

    if [ -n "$THREAD_COUNT" ] && [ "$THREAD_COUNT" != "6" ] && [ "$THREAD_COUNT" != "12" ]; then
        echo "Building hyperopt with custom $THREAD_COUNT threads..."
        gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
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
for f in "${INPUT_FILES[@]}"; do
    [ -f "$f" ] && cat "$f" > /dev/null
done
if [ $HYPER_ONLY -eq 0 ] && [ -f "$INPUT_FILE" ]; then
    ./wordcount_c_ref "$INPUT_FILE" > /dev/null 2>&1
fi
if [ ${#BUILD_VARIANTS[@]} -gt 0 ] && [ -f "$INPUT_FILE" ]; then
    ${BUILD_VARIANTS[0]} "$INPUT_FILE" > /dev/null 2>&1
fi
echo "✓ Cache warmed"
echo ""

analyze_debug_log() {
    local log_file=$1
    local variant_name=$2

    if [ ! -f "$log_file" ]; then
        echo "    ⚠ No debug log found"
        return
    fi

    echo "    Debug Analysis for $variant_name:"
    echo "    ────────────────────────────────"
    echo "    Initialization:"
    grep -E "V-Cache CCD|PID:|mmap:|init:" "$log_file" | head -5 | sed 's/^/      /'

    echo ""
    echo "    Thread Performance:"
    grep -E "^.*T[0-9]+:" "$log_file" | head -8 | sed 's/^/      /'

    echo ""
    echo "    Hash Table Statistics:"
    grep -E "avg probe|max_probe|Hash resizes" "$log_file" | head -3 | sed 's/^/      /'

    echo ""
    echo "    SIMD Efficiency:"
    local simd=$(grep -oE "SIMD chunks: *[0-9]+" "$log_file" | awk '{print $3}' | head -1)
    local scalar=$(grep -oE "Scalar chunks: *[0-9]+" "$log_file" | awk '{print $3}' | head -1)
    if [ -n "$simd" ] && [ -n "$scalar" ] && [ "$scalar" -gt 0 ] 2>/dev/null; then
        local ratio=$(echo "scale=2; $simd / $scalar" | bc)
        echo "      SIMD chunks: $simd"
        echo "      Scalar chunks: $scalar"
        echo "      Ratio: ${ratio}:1"
    else
        grep -E "SIMD chunks|Scalar chunks" "$log_file" | sed 's/^/      /' | head -2
    fi

    echo ""
    echo "    Insert Path:"
    grep -E "Insert: hits=|Table grow time:" "$log_file" | sed 's/^/      /'

    echo ""
    echo "    Run-length Histogram:"
    grep -E "Run-length histogram|Word-length histogram" "$log_file" | sed 's/^/      /'

    echo ""
    echo "    Thread/CPU:"
    grep -E "ThreadCPU T|Worker T" "$log_file" | sed 's/^/      /'

    echo ""
    echo "    Memory Statistics:"
    grep -E "Memory:|Pool exhaustions:|leaked" "$log_file" | head -3 | sed 's/^/      /'

    echo ""
    echo "    Timing Breakdown:"
    grep -E "processing:|merge:|top-k:" "$log_file" | sed 's/^/      /'

    echo ""
    echo "    Issues:"
    grep -E "FATAL|ERROR|exhaustion|truncated" "$log_file" | head -5 | sed 's/^/      /'
}

run_bench() {
    local name="$1"
    local cmd="$2"
    local file="$3"
    local is_debug="$4"

    if [ ! -f "$file" ]; then
        echo "Skipping $name: file '$file' not found"
        return
    fi

    local shortf
    shortf=$(basename "$file")
    [ -z "$name" ] && name="$(basename "$cmd")"

    echo "Testing $name ($shortf):"

    if [ "$is_debug" = "1" ]; then
        rm -f wordcount_debug.log
    fi

    local runner=""
    if [ -n "$PIN_MASK" ] && command -v taskset >/dev/null 2>&1; then
        runner="taskset -c $PIN_MASK"
    fi

    if [ "$is_debug" = "1" ] && [ $PROFILE_MODE -eq 1 ] && command -v perf >/dev/null 2>&1; then
        echo "  Run 1: (with perf stat)"
        timeout 60 perf stat -e "$PERF_EVENTS" $runner $cmd "$file" 2>&1 | sed 's/^/    /' | tee /tmp/perf_out_$$.txt >/dev/null
    fi

    local FS_BYTES
    FS_BYTES=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null)
    local FS_MB
    FS_MB=$(echo "scale=6; $FS_BYTES / 1048576" | bc)

    local times=()
    for ((i=1; i<=NUM_RUNS; i++)); do
        local start_ns end_ns
        start_ns=$(date +%s%N)
        $runner $cmd "$file" > /dev/null 2>&1
        end_ns=$(date +%s%N)

        local result="0.000000"
        if [ -n "$start_ns" ] && [ -n "$end_ns" ] && [ "$end_ns" -gt "$start_ns" ] 2>/dev/null; then
            result=$(echo "scale=6; ($end_ns - $start_ns) / 1000000000" | bc)
        fi

        times+=("$result")
        printf "  Run %d: %.3fs\n" "$i" "$result"
    done

    local sum=0 min=${times[0]:-0}
    for t in "${times[@]}"; do
        sum=$(echo "$sum + $t" | bc)
        if (( $(echo "$t < $min" | bc -l) )); then min=$t; fi
    done
    local avg=$(echo "scale=6; $sum / $NUM_RUNS" | bc)

    local sorted=($(printf "%s\n" "${times[@]}" | sort -n))
    local p50=${sorted[$((NUM_RUNS*50/100))]}
    local p95=${sorted[$((NUM_RUNS*95/100))]}
    local p99=${sorted[$((NUM_RUNS*99/100))]}

    local gsum=0
    for t in "${times[@]}"; do
        if (( $(echo "$t <= 0" | bc -l) )); then t="0.000001"; fi
        gsum=$(echo "$gsum + l($t)" | bc -l)
    done
    local gmean=$(echo "e($gsum / $NUM_RUNS)" | bc -l 2>/dev/null | awk '{printf("%.6f",$0)}')

    local throughput="N/A"
    if (( $(echo "$min > 0" | bc -l) )); then
        throughput=$(echo "scale=2; $FS_MB / $min" | bc)
    fi

    printf "  Average: %.3fs\n" "$avg"
    printf "  Best:    %.3fs\n" "$min"
    printf "  p50:     %.3fs  p95: %.3fs  p99: %.3fs  gmean: %.6fs\n" "$p50" "$p95" "$p99" "$gmean"
    if [[ "$throughput" =~ ^[0-9.]+$ ]]; then
        printf "  Throughput: %.2f MB/s\n" "$throughput"
    else
        echo "  Throughput: $throughput"
    fi

    if [ "$is_debug" = "1" ] && [ -f "wordcount_debug.log" ]; then
        cp wordcount_debug.log "/tmp/${name// /_}_debug.log" 2>/dev/null
        echo ""
        analyze_debug_log "wordcount_debug.log" "$name ($shortf)"
    fi

    echo ""

    local label="$name [$shortf]"
    if [ "$name" = "C Reference" ] && [ "$shortf" = "$(basename "$INPUT_FILE")" ]; then
        label="$name"
    fi
    echo "$label|$avg|$min|$throughput" >> /tmp/bench_c_results_$$.txt
    if [ -f /tmp/perf_out_$$.txt ]; then
        mv /tmp/perf_out_$$.txt "/tmp/perf_${label// /_}_$$.txt"
    fi
}

bundle_results() {
    local dir="debug_bundle"
    rm -rf "$dir" >/dev/null 2>&1
    mkdir -p "$dir"
    cp -f wordcount_debug.log "$dir"/ 2>/dev/null
    cp -f /tmp/bench_c_results_$$.txt "$dir"/results.txt 2>/dev/null
    {
        echo "===== uname -a ====="
        uname -a
        echo -e "\n===== lscpu ====="
        lscpu 2>&1
        echo -e "\n===== gcc -v ====="
        gcc -v 2>&1
        echo -e "\n===== /etc/os-release ====="
        cat /etc/os-release 2>/dev/null
    } > "$dir/sysinfo.txt"
    tar -czf debug_bundle.tar.gz "$dir"
    echo "✓ Debug bundle written: debug_bundle.tar.gz"
}

echo "========================================="
echo "Running Benchmarks ($NUM_RUNS runs each)"
echo "========================================="
echo ""

rm -f /tmp/bench_c_results_$$.txt

if [ $HYPER_ONLY -eq 0 ]; then
    run_bench "C Reference" "./wordcount_c_ref" "$INPUT_FILE" "0"
fi

for i in "${!BUILD_VARIANTS[@]}"; do
    is_debug="0"
    [[ "${BUILD_NAMES[$i]}" == *"Debug"* ]] && is_debug="1"
    for f in "${INPUT_FILES[@]}"; do
        run_bench "${BUILD_NAMES[$i]}" "${BUILD_VARIANTS[$i]}" "$f" "$is_debug"
    done
done

if [ $DEBUG_MODE -eq 1 ]; then
    echo "========================================="
    echo "Debug Mode Analysis Summary"
    echo "========================================="
    echo ""
    [ -f "wordcount_debug.log" ] && { echo "Debug Log Output (wordcount_debug.log):"; echo "────────────────────────────────────────"; cat wordcount_debug.log; echo ""; }
fi

echo ""
echo "========================================="
echo "Performance Summary"
echo "========================================="
echo ""

echo "Implementation               Average    Best     Throughput   vs Reference"
echo "──────────────────────────────────────────────────────────────────────────"

if [ $HYPER_ONLY -eq 0 ]; then
  ref_avg=$(grep "^C Reference|" /tmp/bench_c_results_$$.txt | cut -d'|' -f2 | head -1)
else
  ref_avg=""
fi

while IFS='|' read -r name avg min throughput; do
    if [ "$name" = "C Reference" ]; then
        if [[ "$throughput" =~ ^[0-9.]+$ ]]; then
            printf "%-28s %8.3fs %8.3fs %8.2f MB/s   baseline\n" "$name" "$avg" "$min" "$throughput"
        else
            printf "%-28s %8.3fs %8.3fs %8s\n" "$name" "$avg" "$min" "$throughput"
        fi
    else
        if [ -n "$ref_avg" ]; then
          speedup=$(echo "scale=2; $ref_avg / $avg" | bc)
          if [[ "$throughput" =~ ^[0-9.]+$ ]]; then
              printf "%-28s %8.3fs %8.3fs %8.2f MB/s   %.2fx\n" "$name" "$avg" "$min" "$throughput" "$speedup"
          else
              printf "%-28s %8.3fs %8.3fs %8s   %.2fx\n" "$name" "$avg" "$min" "$throughput" "$speedup"
          fi
        else
          if [[ "$throughput" =~ ^[0-9.]+$ ]]; then
              printf "%-28s %8.3fs %8.3fs %8.2f MB/s\n" "$name" "$avg" "$min" "$throughput"
          else
              printf "%-28s %8.3fs %8.3fs %8s\n" "$name" "$avg" "$min" "$throughput"
          fi
        fi
    fi
done < /tmp/bench_c_results_$$.txt

echo ""
echo "========================================="
echo "Performance Analysis"
echo "========================================="
echo ""

best_throughput=0
best_impl=""
while IFS='|' read -r name avg min throughput; do
    if [[ "$throughput" =~ ^[0-9.]+$ ]] && (( $(echo "$throughput > $best_throughput" | bc -l) )); then
        best_throughput=$throughput
        best_impl=$name
    fi
done < /tmp/bench_c_results_$$.txt

echo "Best performer: $best_impl"
echo "Peak throughput: ${best_throughput} MB/s"
echo ""

target_throughput=3000
if [[ "$best_throughput" =~ ^[0-9.]+$ ]]; then
    throughput_pct=$(echo "scale=1; $best_throughput * 100 / $target_throughput" | bc)
else
    throughput_pct="N/A"
fi
echo "Target Analysis:"
echo "  Target throughput: 3.0 GB/s"
echo "  Achieved: ${best_throughput} MB/s (${throughput_pct}% of target)"
echo ""

echo ""
if [ "$NO_CLEANUP" -eq 1 ]; then
    response="n"
    echo "Skipping cleanup (--no-cleanup flag set)"
elif [ -t 0 ]; then
    echo "Clean up test binaries, logs, and output files? (y/n)"
    read -r response
else
    read -r response 2>/dev/null || response="n"
fi

[ "$BUNDLE" -eq 1 ] && bundle_results

if [[ "$response" =~ ^[Yy]$ ]]; then
    rm -f wordcount_c_ref wordcount_hopt* wordcount_debug*
    rm -f wordcount_asan
    rm -f /tmp/bench_c_*_$$.txt /tmp/*_debug.log
    rm -f perf.data perf.data.old
    rm -f wordcount_debug.log
    rm -f *.gcda *.gcno
    rm -f book_c-hopt_results.txt
    rm -f book_c_results.txt
    rm -rf debug_bundle
    rm -f debug_bundle.tar.gz
    echo "✓ Cleaned up all files"
else
    echo "✓ Keeping all files"
fi