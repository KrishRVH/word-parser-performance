cmake_minimum_required(VERSION 3.16)
project(word_parser_performance C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warnings globally
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

# =============================================================================
# Library (C99) - in library/
# =============================================================================
add_library(wordcount_lib STATIC library/wordcount.c)
target_include_directories(wordcount_lib PUBLIC library)
set_target_properties(wordcount_lib PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# CLI using library
add_executable(wc library/wc_main.c)
target_link_libraries(wc PRIVATE wordcount_lib)
set_target_properties(wc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# Tests
add_executable(wc_test library/wc_test.c)
target_link_libraries(wc_test PRIVATE wordcount_lib)
set_target_properties(wc_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

enable_testing()
add_test(NAME wc_unit_tests COMMAND wc_test)

# =============================================================================
# Standalone benchmark implementations (C11)
# =============================================================================

# Idiomatic mmap version
add_executable(wordcount_idiomatic wordcount.c)
set_target_properties(wordcount_idiomatic PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# Hyperopt version - needs pthreads and arch-specific flags
add_executable(wordcount_hyperopt wordcount_hyperopt.c)
target_link_libraries(wordcount_hyperopt PRIVATE pthread m)
set_target_properties(wordcount_hyperopt PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON  # Needs _GNU_SOURCE
)
target_compile_options(wordcount_hyperopt PRIVATE
    -O3
    -march=native
    -mtune=native
    -flto
    -fomit-frame-pointer
    -funroll-loops
)
target_compile_definitions(wordcount_hyperopt PRIVATE _GNU_SOURCE)