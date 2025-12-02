/**
 * @file wordcount.h
 * @brief Word frequency counting library - public API.
 *
 * This library provides a reentrant hash-table-based word frequency counter.
 * It is designed as a textbook example of idiomatic Modern C (C11 baseline,
 * C23-ready).
 *
 * ## Design Principles
 *
 * - **Pure ISO C11+**: No POSIX or platform-specific APIs in the public
 * interface.
 * - **Opaque types**: The `wc_table` structure is forward-declared; clients
 * cannot access its internals directly.
 * - **Explicit error handling**: All fallible operations return `wc_status`.
 * - **No global state**: Fully reentrant; safe for concurrent use as long as
 * each `wc_table` instance is accessed by at most one thread at a time, or
 * external synchronization is used. Different table instances may be used
 * concurrently by different threads without synchronization.
 * - **CERT C compliant**: Follows SEI CERT C Coding Standard guidelines.
 *
 * ## Word Definition
 *
 * A "word" is a maximal sequence of bytes for which `isalpha()` returns
 * non-zero in the current C locale. The `wc_process_text()` function case-folds
 * words to lowercase via `tolower()` before storage, making counting
 * case-insensitive.
 *
 * Note: Classification uses `isalpha()` and `tolower()` from `<ctype.h>` in the
 * current C locale. The library does *not* handle Unicode beyond what the
 * underlying C library provides. For portable ASCII-only behavior, ensure the
 * "C" locale is active.
 *
 * ## Example Usage
 *
 * ```c
 * wc_table *t = wc_create(NULL);
 * if (!t) { handle_oom(); }
 *
 * wc_status st = wc_process_text(t, buffer, length);
 * if (st != WC_OK) { handle_error(st); }
 *
 * wc_entry *entries = NULL;
 * size_t count = 0;
 * st = wc_snapshot(t, &entries, &count);
 * if (st == WC_OK) {
 *     for (size_t i = 0; i < count; ++i) {
 *         printf("%zu %s\n", entries[i].count, entries[i].word);
 *     }
 *     wc_free_snapshot(entries);
 * }
 *
 * wc_destroy(t);
 * ```
 *
 * @author Refactored for Modern C best practices
 * @version 2.1.0
 * @copyright Public domain / CC0
 */

#ifndef WORDCOUNT_H
#define WORDCOUNT_H

/*===========================================================================
 * Version information (compile-time)
 *===========================================================================*/

/** @brief Library major version number. */
#define WC_VERSION_MAJOR 2

/** @brief Library minor version number. */
#define WC_VERSION_MINOR 1

/** @brief Library patch version number. */
#define WC_VERSION_PATCH 0

/** @brief Library version as a string literal. */
#define WC_VERSION_STRING "2.1.0"

/*===========================================================================
 * Feature-test and compatibility macros
 *===========================================================================*/

/**
 * @def WC_NODISCARD
 * @brief Marks functions whose return value should not be discarded.
 *
 * Uses C23 `[[nodiscard]]` if available, falls back to GCC/Clang
 * `__attribute__((warn_unused_result))`, or expands to nothing.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define WC_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define WC_NODISCARD __attribute__((warn_unused_result))
#else
#define WC_NODISCARD
#endif

/**
 * @def WC_PURE
 * @brief Marks functions with no side effects except return value.
 *
 * The function result depends only on arguments and global memory that is
 * not modified during the call.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WC_PURE __attribute__((pure))
#else
#define WC_PURE
#endif

/*===========================================================================
 * Standard includes
 *===========================================================================*/

#include <stdbool.h> /* bool (C99+) */
#include <stddef.h>  /* size_t, NULL */

/*===========================================================================
 * C++ compatibility
 *===========================================================================*/

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================
 * Type definitions
 *===========================================================================*/

/**
 * @brief Opaque handle to a word-count table.
 *
 * The internal structure is not exposed to clients. All access is through
 * the public API functions. This allows the implementation to change
 * without breaking ABI compatibility.
 */
typedef struct wc_table wc_table;

/**
 * @brief A single word-count entry returned by wc_snapshot().
 *
 * @note The `word` pointer refers to memory owned by the table and remains
 *       valid only until `wc_destroy()` is called on that table. Clients
 *       must not free or modify this pointer.
 */
typedef struct wc_entry
{
    const char *word; /**< NUL-terminated word string (table-owned). */
    size_t count;     /**< Number of occurrences of this word. */
} wc_entry;

/**
 * @brief Status codes returned by public API functions.
 *
 * All fallible operations return a `wc_status` to indicate success or
 * the category of failure. Check for `WC_OK` to confirm success.
 */
typedef enum wc_status
{
    WC_OK = 0,               /**< Operation completed successfully. */
    WC_ERR_INVALID_ARGUMENT, /**< A required argument was NULL or invalid. */
    WC_ERR_OUT_OF_MEMORY     /**< Memory allocation failed. */
} wc_status;

/**
 * @brief Configuration options for wc_create().
 *
 * Pass NULL to `wc_create()` to accept library defaults. Alternatively,
 * zero-initialize this structure and set only the fields you wish to
 * customize; zero values select sensible defaults.
 *
 * Example:
 * ```c
 * wc_config cfg = { .initial_capacity = 65536 };
 * wc_table *t = wc_create(&cfg);
 * ```
 */
typedef struct wc_config
{
    /**
     * @brief Desired minimum number of hash buckets.
     *
     * The implementation rounds this up to the nearest power of two,
     * with a minimum of 16384 buckets (the default). Values smaller than
     * the default are treated as "use default". Larger values allow
     * pre-sizing for known large datasets to reduce rehashing.
     *
     * A value of 0 selects the default (16384 buckets).
     */
    size_t initial_capacity;

    /**
     * @brief Maximum stored word length, including terminating NUL.
     *
     * Words longer than (max_word_length - 1) bytes are truncated;
     * the remainder of the alphabetic sequence is skipped.
     *
     * A value of 0 selects a sensible default (64 bytes).
     * Valid range: 8 to 1024 bytes; values outside this range are clamped.
     */
    size_t max_word_length;
} wc_config;

/*===========================================================================
 * Lifecycle functions
 *===========================================================================*/

/**
 * @brief Create a new, empty word-count table.
 *
 * @param cfg  Configuration options, or NULL for defaults.
 * @return     A new table handle on success, or NULL if allocation failed.
 *
 * @note The returned table must be destroyed with `wc_destroy()` when
 *       no longer needed to avoid memory leaks.
 *
 * @par Thread Safety
 * Thread-safe: multiple threads may create independent tables concurrently.
 * However, the returned table instance is not internally synchronized;
 * concurrent access to the same table requires external synchronization.
 */
WC_NODISCARD
wc_table *wc_create(const wc_config *cfg);

/**
 * @brief Destroy a table and release all associated memory.
 *
 * After this call, the table handle is invalid and must not be used.
 * Passing NULL is safe and has no effect.
 *
 * @param t  Table to destroy, or NULL.
 *
 * @par Thread Safety
 * Not thread-safe for the same table instance. Ensure no other threads
 * are accessing the table during destruction. Safe to call concurrently
 * on different table instances.
 */
void wc_destroy(wc_table *t);

/*===========================================================================
 * Word insertion functions
 *===========================================================================*/

/**
 * @brief Add a single pre-processed word to the table.
 *
 * The word is copied internally. If the word already exists, its count
 * is incremented; otherwise, a new entry is created with count 1.
 *
 * @param t     Table handle (must not be NULL).
 * @param word  NUL-terminated word string (must not be NULL).
 * @return      WC_OK on success, or an error status.
 *
 * @retval WC_OK                   Word added successfully.
 * @retval WC_ERR_INVALID_ARGUMENT `t` or `word` was NULL.
 * @retval WC_ERR_OUT_OF_MEMORY    Allocation failed.
 *
 * @note This function does NOT perform any normalization. No case-folding,
 *       Unicode normalization, whitespace handling, or tokenization is
 *       applied; the string is inserted verbatim. Use `wc_process_text()`
 *       for raw text input with automatic tokenization and lowercasing.
 *
 * @par Thread Safety
 * Not thread-safe for the same table instance. Concurrent calls on the
 * same table require external synchronization.
 *
 * @par Complexity
 * Amortized O(1) average case; O(n) worst case if many collisions.
 */
WC_NODISCARD
wc_status wc_add_word(wc_table *t, const char *word);

/**
 * @brief Process raw text and count all words.
 *
 * Scans the input buffer for words (maximal sequences of `isalpha()`
 * characters), converts them to lowercase, and adds them to the table.
 * Words exceeding `max_word_length - 1` bytes are truncated.
 *
 * @param t     Table handle (must not be NULL).
 * @param data  Pointer to input text buffer, or NULL if `len` is 0.
 * @param len   Number of bytes in the input buffer.
 * @return      WC_OK on success, or an error status.
 *
 * @retval WC_OK                   Text processed successfully.
 * @retval WC_ERR_INVALID_ARGUMENT `t` was NULL, or `data` was NULL with
 * non-zero `len`.
 * @retval WC_ERR_OUT_OF_MEMORY    Allocation failed during processing.
 *
 * @note The input buffer is read-only and is not modified.
 *
 * @warning This function processes the buffer as a complete, independent unit.
 *          It is NOT suitable for streaming chunked input where words may be
 *          split across chunk boundaries. For streaming use cases, accumulate
 *          the complete text first, or implement boundary-aware chunking.
 *
 * @par Thread Safety
 * Not thread-safe for the same table instance. Concurrent calls on the
 * same table require external synchronization.
 *
 * @par Complexity
 * O(len) for scanning, plus O(unique_words) hash table operations.
 */
WC_NODISCARD
wc_status wc_process_text(wc_table *t, const char *data, size_t len);

/*===========================================================================
 * Query functions
 *===========================================================================*/

/**
 * @brief Get the total number of word tokens processed.
 *
 * This counts every word occurrence, including duplicates.
 * For example, "the cat and the dog" has 5 total words.
 *
 * @param t  Table handle, or NULL.
 * @return   Total word count, or 0 if `t` is NULL.
 */
WC_PURE
size_t wc_total_words(const wc_table *t);

/**
 * @brief Get the number of distinct (unique) words in the table.
 *
 * For example, "the cat and the dog" has 4 unique words.
 *
 * @param t  Table handle, or NULL.
 * @return   Unique word count, or 0 if `t` is NULL.
 */
WC_PURE
size_t wc_unique_words(const wc_table *t);

/*===========================================================================
 * Snapshot functions
 *===========================================================================*/

/**
 * @brief Take a sorted snapshot of all word-count entries.
 *
 * Allocates and populates an array of `wc_entry` structures containing
 * all words and their counts, sorted by:
 *   1. Descending count (most frequent first).
 *   2. Lexicographic order (ascending) for ties.
 *
 * @param t            Table handle (must not be NULL).
 * @param out_entries  Output: pointer to the allocated array, or NULL if empty.
 * @param out_len      Output: number of entries in the array.
 * @return             WC_OK on success, or an error status.
 *
 * @retval WC_OK                   Snapshot created successfully.
 * @retval WC_ERR_INVALID_ARGUMENT Any pointer argument was NULL.
 * @retval WC_ERR_OUT_OF_MEMORY    Allocation failed.
 *
 * @note On success with an empty table, `*out_entries` is set to NULL
 *       and `*out_len` is set to 0. This is not an error.
 *
 * @note The `wc_entry.word` pointers in the returned array refer to
 *       memory owned by the table. They remain valid until `wc_destroy()`
 *       is called. Do not free these pointers.
 *
 * @warning The caller owns the array itself and must release it by
 *          calling `wc_free_snapshot()`.
 */
WC_NODISCARD
wc_status
wc_snapshot(const wc_table *t, wc_entry **out_entries, size_t *out_len);

/**
 * @brief Free a snapshot array obtained from wc_snapshot().
 *
 * Passing NULL is safe and has no effect.
 *
 * @param entries  Array to free, or NULL.
 *
 * @note Do NOT call `free()` directly on the array; always use this
 *       function to ensure future compatibility.
 */
void wc_free_snapshot(wc_entry *entries);

/*===========================================================================
 * Version information
 *===========================================================================*/

/**
 * @brief Get the library version string at runtime.
 *
 * Equivalent to `WC_VERSION_STRING` but callable at runtime. Useful for
 * verifying header/library version consistency.
 *
 * @return Static string in "MAJOR.MINOR.PATCH" format, never NULL.
 */
WC_PURE
const char *wc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* WORDCOUNT_H */
