#!/bin/bash
# bench.sh - Performance benchmark
# Usage: ./bench.sh [--validate] [--runs=N]
# Notes:
#  - C hyperopt builds: 6-thread and 12-thread by default (best on WSL2)
#  - Uses GCC >=14 with -march=znver5/-mtune=znver5 if available; falls back to -march=native
#  - Stable timing/formatting under C locale

export LC_ALL=C LANG=C

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

check_dependency() {
    if ! command -v "$1" &> /dev/null; then
        echo "✗ $1 is not installed"
        return 1
    else
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

if [ ! -f "book.txt" ]; then
    echo "Downloading test file (Moby Dick from Project Gutenberg)..."
    curl -s https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt
    echo "✓ Downloaded book.txt"
else
    echo "✓ Using existing book.txt"
fi

# Show info for the primary test file
FILE_SIZE=$(du -h book.txt | cut -f1)
WORD_COUNT=$(wc -w < book.txt)
LINE_COUNT=$(wc -l < book.txt)
echo ""
echo "Test file information:"
echo "  Size: $FILE_SIZE"
echo "  Words: $(printf "%'d" $WORD_COUNT)"
echo "  Lines: $(printf "%'d" $LINE_COUNT)"
echo ""

# Create book2.txt (5x) and book3.txt (25x) if user opts in
create_multiplied_file() {
    local base=$1
    local out=$2
    local mult=$3
    if [ -f "$out" ]; then
        return
    fi
    echo "Creating $out as ${mult}x of $base..."
    : > "$out"
    for _ in $(seq 1 "$mult"); do
        cat "$base" >> "$out"
    done
    echo "✓ Created $out"
}

echo "Do you want to test with larger files? (y/n)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    create_multiplied_file "book.txt" "book2.txt" 5
    create_multiplied_file "book.txt" "book3.txt" 25
    TEST_FILES=("book.txt")
    [ -f "book2.txt" ] && TEST_FILES+=("book2.txt")
    [ -f "book3.txt" ] && TEST_FILES+=("book3.txt")
else
    TEST_FILES=("book.txt")
fi
echo ""

# Determine best arch flags for hyperopt
GCC_MAJ=""
if [ "$HAS_GCC" = "1" ]; then
    GCC_MAJ=$(gcc -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)
fi
AVX_FLAGS="-mavx512f -mavx512bw -mavx512vl -msse4.2"
if [ -n "$GCC_MAJ" ] && [ "$GCC_MAJ" -ge 14 ]; then
    MARCH_FLAGS="-march=znver5 -mtune=znver5 $AVX_FLAGS"
else
    MARCH_FLAGS="-march=native -mtune=native"
fi

if [ "$HAS_RUST" = "1" ]; then
    echo "Compiling Rust version..."
    rustc -C opt-level=3 -C target-cpu=native -C lto=fat -C codegen-units=1 \
        wordcount.rs -o wordcount_rust 2>rust_error.log
    if [ $? -eq 0 ]; then
        echo "✓ Rust compilation successful"
        rm -f rust_error.log
    else
        echo "✗ Rust compilation failed"
        head -10 rust_error.log
        echo ""
        HAS_RUST=0
    fi
fi

if [ "$HAS_GCC" = "1" ]; then
    echo "Compiling C reference..."
    gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops \
        wordcount.c -o wordcount_c 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C compilation successful"
    else
        echo "✗ C compilation failed"
        HAS_GCC=0
    fi

    echo "Compiling C hyperopt (6-thread and 12-thread)..."
    gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
        -DNUM_THREADS=6 wordcount_hyperopt.c -o wordcount_hopt_t6 -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C hyperopt 6-thread successful"
    else
        echo "✗ C hyperopt 6-thread failed"
    fi

    gcc -O3 $MARCH_FLAGS -flto -fomit-frame-pointer -funroll-loops -pthread \
        -DNUM_THREADS=12 wordcount_hyperopt.c -o wordcount_hopt_t12 -lm 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "✓ C hyperopt 12-thread successful"
    else
        echo "✗ C hyperopt 12-thread failed"
    fi
fi

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

extract_counts_from_output() {
    local impl=$1
    local test_file=$2
    local output=$($impl "$test_file" 2>/dev/null | grep -E "Total words:|Unique words:")
    local total=$(echo "$output" | grep "Total words:" | sed 's/[^0-9]//g')
    local unique=$(echo "$output" | grep "Unique words:" | sed 's/[^0-9]//g')
    echo "$total $unique"
}

validate_results() {
    local file=$1
    local base_name="${file%.txt}"

    echo "  Validating results for $file..."
    local c_counts=""
    local expected_total=0
    local expected_unique=0

    if [ "$HAS_GCC" = "1" ] && [ -f "wordcount_c" ]; then
        c_counts=$(extract_counts_from_output "./wordcount_c" "$file")
        expected_total=$(echo $c_counts | cut -d' ' -f1)
        expected_unique=$(echo $c_counts | cut -d' ' -f2)
        echo "    Using C as reference: $expected_total total, $expected_unique unique"
    else
        echo "    ⚠ C implementation not available for reference"
        return
    fi

    declare -a languages
    declare -a total_counts
    declare -a unique_counts

    languages+=("C (ref)")
    total_counts+=("$expected_total")
    unique_counts+=("$expected_unique")

    if [ "$HAS_GCC" = "1" ] && [ -f "wordcount_hopt_t6" ]; then
        counts=$(extract_counts_from_output "./wordcount_hopt_t6" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("C hyperopt (6-thread)")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi
    if [ "$HAS_GCC" = "1" ] && [ -f "wordcount_hopt_t12" ]; then
        counts=$(extract_counts_from_output "./wordcount_hopt_t12" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("C hyperopt (12-thread)")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    if [ "$HAS_RUST" = "1" ] && [ -f "wordcount_rust" ]; then
        counts=$(extract_counts_from_output "./wordcount_rust" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("Rust")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    if [ "$HAS_GO" = "1" ] && [ -f "wordcount_go" ]; then
        counts=$(extract_counts_from_output "./wordcount_go" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("Go")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    if [ "$HAS_DOTNET" = "1" ] && [ -f "bin/Release/net8.0/WordCount" ]; then
        counts=$(extract_counts_from_output "./bin/Release/net8.0/WordCount" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("C#")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    if [ "$HAS_NODE" = "1" ] && [ -f "wordcount.js" ]; then
        counts=$(extract_counts_from_output "node wordcount.js" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("JavaScript")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    if [ "$HAS_PHP" = "1" ] && [ -f "wordcount.php" ]; then
        counts=$(extract_counts_from_output "php wordcount.php" "$file")
        total=$(echo $counts | cut -d' ' -f1)
        unique=$(echo $counts | cut -d' ' -f2)
        languages+=("PHP")
        total_counts+=("$total")
        unique_counts+=("$unique")
    fi

    echo "    Word count comparison:"
    local all_match=1
    for i in "${!languages[@]}"; do
        if [ $i -eq 0 ]; then
            printf "      %-20s: %d total, %d unique (reference)\n" \
                "${languages[$i]}" "${total_counts[$i]}" "${unique_counts[$i]}"
        else
            local diff_total=$((${total_counts[$i]} - expected_total))
            local diff_unique=$((${unique_counts[$i]} - expected_unique))
            if [ "${total_counts[$i]}" = "$expected_total" ] && \
               [ "${unique_counts[$i]}" = "$expected_unique" ]; then
                printf "      %-20s: ✓ Exact match\n" "${languages[$i]}"
            else
                all_match=0
                if [ -n "${total_counts[$i]}" ] && [ -n "${unique_counts[$i]}" ]; then
                    printf "      %-20s: ⚠ Total: %d (%+d), Unique: %d (%+d)\n" \
                        "${languages[$i]}" "${total_counts[$i]}" "$diff_total" \
                        "${unique_counts[$i]}" "$diff_unique"
                else
                    printf "      %-20s: ✗ Failed to get counts\n" "${languages[$i]}"
                fi
            fi
        fi
    done

    echo "    Top words consistency check:"
    local temp_dir
    temp_dir=$(mktemp -d)
    declare -a result_files
    [ -f "${base_name}_c_results.txt" ] && result_files+=("${base_name}_c_results.txt")
    [ -f "${base_name}_c-hopt_results.txt" ] && result_files+=("${base_name}_c-hopt_results.txt")
    [ -f "${base_name}_rust_results.txt" ] && result_files+=("${base_name}_rust_results.txt")
    [ -f "${base_name}_go_results.txt" ] && result_files+=("${base_name}_go_results.txt")
    [ -f "${base_name}_csharp_results.txt" ] && result_files+=("${base_name}_csharp_results.txt")
    [ -f "${base_name}_javascript_results.txt" ] && result_files+=("${base_name}_javascript_results.txt")
    [ -f "${base_name}_php_results.txt" ] && result_files+=("${base_name}_php_results.txt")

    if [ ${#result_files[@]} -ge 2 ]; then
        for i in "${!result_files[@]}"; do
            grep -E '^\s*[0-9]+[\.\s]' "${result_files[$i]}" | \
                head -10 | sed 's/[,]//g' | \
                awk '{
                    word = ""; count = "";
                    for(i=1; i<=NF; i++) {
                        if ($i ~ /^[0-9]+$/ && i > 1) { count = $i }
                        else if ($i !~ /^[0-9]+\.?$/ && word == "") { word = $i }
                    }
                    if (word != "" && count != "") print word, count
                }' | sort > "$temp_dir/results_$i.txt"
        done
        local reference_file="$temp_dir/results_0.txt"
        local top_words_match=1
        for ((i=1; i<${#result_files[@]}; i++)); do
            if ! cmp -s "$reference_file" "$temp_dir/results_$i.txt"; then
                top_words_match=0; break
            fi
        done
        if [ $top_words_match -eq 1 ]; then
            echo "      ✓ All implementations have identical top 10 words"
        else
            echo "      ⚠ Top 10 words differ between implementations"
        fi
    else
        echo "      ⚠ Not enough result files to compare"
    fi
    rm -rf "$temp_dir"

    if [ $all_match -eq 1 ]; then
        echo "    ✓ Perfect validation: All implementations match C reference"
    else
        echo "    ⚠ Some implementations differ from C reference"
    fi
}

declare -a LANG_NAMES
declare -a LANG_TIMES
declare -A ALL_RESULTS
RESULT_COUNT=0

run_benchmark() {
    local lang=$1
    local cmd=$2
    local file=$3

    echo "  $lang:"
    # Warm up once
    $cmd "$file" > /dev/null 2>&1

    # Prefer single-line progress if a TTY
    local IS_TTY=0
    if [ -t 1 ]; then IS_TTY=1; fi

    # Accumulate in integer nanoseconds (robust and fast)
    local total_ns=0
    local best_ns=""

    for ((run=1; run<=NUM_RUNS; run++)); do
        # Monotonic timing in ns (Linux)
        local start_ns end_ns
        start_ns=$(date +%s%N)
        $cmd "$file" > /dev/null 2>&1
        end_ns=$(date +%s%N)

        # Duration in ns
        local dur_ns=$((end_ns - start_ns))
        # Guard against any weirdness
        if [ "$dur_ns" -lt 1000000 ]; then
            dur_ns=1000000  # clamp to 1ms to avoid zeros/div-by-zero downstream
        fi

        total_ns=$((total_ns + dur_ns))
        if [ -z "$best_ns" ] || [ "$dur_ns" -lt "$best_ns" ]; then
            best_ns=$dur_ns
        fi

        # Convert to seconds for display with awk (no sci-notation issues)
        local last_s best_s avg_s
        last_s=$(awk -v ns="$dur_ns" 'BEGIN{printf("%.3f", ns/1e9)}')
        best_s=$(awk -v ns="$best_ns" 'BEGIN{printf("%.3f", ns/1e9)}')
        avg_s=$(awk -v ns="$total_ns" -v n="$run" 'BEGIN{printf("%.3f", (ns/n)/1e9)}')

        if [ "$IS_TTY" -eq 1 ]; then
            printf "\r    Run %d/%d: last=%ss best=%ss avg=%ss\033[K" \
                "$run" "$NUM_RUNS" "$last_s" "$best_s" "$avg_s"
        else
            printf "    Run %d: %ss\n" "$run" "$last_s"
        fi
    done

    if [ "$IS_TTY" -eq 1 ]; then
        printf "\n"
    fi

    # Final average in seconds string
    local avg_final_s
    avg_final_s=$(awk -v ns="$total_ns" -v n="$NUM_RUNS" 'BEGIN{v=(ns/n)/1e9; printf((v<0.1)?"%.4f":"%.3f", v)}')
    echo "    Average: ${avg_final_s}s"
    echo ""

    # Record for rankings/summary (only for the first test file to keep baseline semantics)
    if [[ "$file" == "${TEST_FILES[0]}" ]]; then
        LANG_NAMES[RESULT_COUNT]="$lang"
        LANG_TIMES[RESULT_COUNT]="$avg_final_s"
        RESULT_COUNT=$((RESULT_COUNT + 1))
    fi

    ALL_RESULTS["${lang}:${file}"]="$avg_final_s"
}

for TEST_FILE in "${TEST_FILES[@]}"; do
    echo "========================================="
    echo "Benchmarking with: $TEST_FILE ($(du -h "$TEST_FILE" | cut -f1))"
    echo "========================================="
    echo ""

    if [ "$HAS_GCC" = "1" ]; then
        [ -f "wordcount_c" ] && run_benchmark "C" "./wordcount_c" "$TEST_FILE"
        [ -f "wordcount_hopt_t6" ] && run_benchmark "C hyperopt (6-thread)" "./wordcount_hopt_t6" "$TEST_FILE"
        [ -f "wordcount_hopt_t12" ] && run_benchmark "C hyperopt (12-thread)" "./wordcount_hopt_t12" "$TEST_FILE"
    fi
    if [ "$HAS_RUST" = "1" ] && [ -f "wordcount_rust" ]; then
        run_benchmark "Rust" "./wordcount_rust" "$TEST_FILE"
    fi
    if [ "$HAS_GO" = "1" ] && [ -f "wordcount_go" ]; then
        GOGC=off run_benchmark "Go" "./wordcount_go" "$TEST_FILE"
    fi
    if [ "$HAS_DOTNET" = "1" ]; then
        if [ -f "bin/Release/net8.0/WordCount" ]; then
            DOTNET_TieredCompilation=0 run_benchmark "C# (.NET)" "./bin/Release/net8.0/WordCount" "$TEST_FILE"
        elif [ -f "wordcount_cs.exe" ]; then
            run_benchmark "C# (Mono)" "./wordcount_cs.exe" "$TEST_FILE"
        fi
    fi
    if [ "$HAS_NODE" = "1" ]; then
        run_benchmark "JavaScript (Node.js)" \
            "node --max-old-space-size=4096 --optimize-for-size wordcount.js" \
            "$TEST_FILE"
    fi
    if [ "$HAS_PHP" = "1" ]; then
        FILE_SIZE_BYTES=$(stat -c%s "$TEST_FILE" 2>/dev/null || \
                          stat -f%z "$TEST_FILE" 2>/dev/null)
        if [ "$FILE_SIZE_BYTES" -gt 10485760 ]; then
            run_benchmark "PHP" \
              "php -d opcache.enable_cli=1 -d opcache.jit=tracing -d opcache.jit_buffer_size=128M wordcount.php" \
              "$TEST_FILE"
        else
            run_benchmark "PHP" "php wordcount.php" "$TEST_FILE"
        fi
    fi

    if [ $VALIDATE -eq 1 ]; then
        validate_results "$TEST_FILE"
        echo ""
    fi
done

echo "========================================="
echo "Output Files Generated:"
echo "========================================="
for f in *_results.txt; do
    if [ -f "$f" ]; then
        echo "  ✓ $f ($(wc -l < "$f") lines)"
    fi
done
echo ""

echo "========================================="
echo "Benchmark Complete!"
echo "========================================="
echo ""

if [ $RESULT_COUNT -gt 0 ]; then
    echo "Performance Ranking (${TEST_FILES[0]}):"
    echo "----------------------------------------"
    TEMP_RESULTS=$(mktemp)
    for ((i=0; i<RESULT_COUNT; i++)); do
        echo "${LANG_TIMES[$i]} ${LANG_NAMES[$i]}" >> "$TEMP_RESULTS"
    done
    RANK=1
    BASELINE_TIME=""
    sort -n "$TEMP_RESULTS" | while read TIME LANG; do
        if [ -z "$BASELINE_TIME" ]; then
            BASELINE_TIME="$TIME"
            printf "%d. %-24s %8.3fs (baseline)\n" "$RANK" "$LANG" "$TIME"
        else
            if (( $(echo "$BASELINE_TIME > 0" | bc -l) )); then
                SLOWDOWN=$(echo "scale=1; $TIME / $BASELINE_TIME" | bc)
                printf "%d. %-24s %8.3fs (%.1fx slower)\n" \
                    "$RANK" "$LANG" "$TIME" "$SLOWDOWN"
            else
                printf "%d. %-24s %8.3fs\n" "$RANK" "$LANG" "$TIME"
            fi
        fi
        RANK=$((RANK + 1))
    done
    rm -f "$TEMP_RESULTS"
    echo ""

    if [ ${#TEST_FILES[@]} -gt 1 ]; then
        echo "Performance Across File Sizes:"
        echo "----------------------------------------"
        printf "%-24s" "Language"
        for FILE in "${TEST_FILES[@]}"; do
            SIZE=$(du -h "$FILE" | cut -f1)
            printf " %10s" "$SIZE"
        done
        echo ""
        echo "----------------------------------------"
        for ((i=0; i<RESULT_COUNT; i++)); do
            LANG="${LANG_NAMES[$i]}"
            printf "%-24s" "$LANG"
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

    if [ $RESULT_COUNT -gt 1 ]; then
        FASTEST="${LANG_TIMES[0]}"; SLOWEST="${LANG_TIMES[0]}"
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
            if command -v lscpu &> /dev/null; then
                echo "- CPU: $(lscpu | grep -m1 'Model name' | cut -d: -f2 | xargs)"
            elif [ -f /proc/cpuinfo ]; then
                echo "- CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
            fi
            if [ $VALIDATE -eq 1 ]; then
                echo ""
                echo "- Validation: Using C implementation as reference"
            fi
        fi
    fi
else
    echo "No benchmark results collected."
fi
echo ""

echo "To run individual tests:"
if [ "$HAS_GCC" = "1" ]; then
    echo "  ./wordcount_c book.txt            # Reference C implementation"
    [ -f "wordcount_hopt_t6" ]  && echo "  ./wordcount_hopt_t6 book.txt     # C hyperopt (6-thread)"
    [ -f "wordcount_hopt_t12" ] && echo "  ./wordcount_hopt_t12 book.txt    # C hyperopt (12-thread)"
fi
[ "$HAS_RUST" = "1" ]   && echo "  ./wordcount_rust book.txt"
[ "$HAS_GO" = "1" ]     && echo "  GOGC=off ./wordcount_go book.txt"
[ "$HAS_DOTNET" = "1" ] && echo "  ./bin/Release/net8.0/WordCount book.txt"
[ "$HAS_NODE" = "1" ]   && echo "  node wordcount.js book.txt"
[ "$HAS_PHP" = "1" ]    && echo "  php wordcount.php book.txt"
echo ""

echo "Clean up compiled files, test files, and build artifacts? (y/n)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    rm -f wordcount_rust wordcount_c wordcount_go wordcount_cs.exe
    rm -f wordcount_hopt_t6 wordcount_hopt_t12
    rm -rf bin obj
    rm -f WordCount.csproj
    rm -f *_results.txt
    echo "✓ Cleaned up all files"
fi