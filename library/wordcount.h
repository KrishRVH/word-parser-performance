/**
 * @file wordcount.h
 * @brief Hash-table–based word frequency counter (C99, portable).
 *
 * This library provides a small, self-contained, reentrant word-frequency
 * counter built around a dynamically-sized hash table.
 *
 * It is intended both as production-quality utility code and as a
 * didactic example of:
 *
 *   - clean C99 library design (no global state, opaque handles),
 *   - careful error handling and invariants,
 *   - portable character handling through <ctype.h>,
 *   - and disciplined memory management.
 *
 * The implementation is strictly ISO C99, with optional compiler
 * attributes enabled via macros when available. It does not depend
 * on POSIX or platform-specific APIs.
 *
 * -----------------------------------------------------------------------
 *  Word model
 * -----------------------------------------------------------------------
 *
 * A “word” is a maximal sequence of bytes for which isalpha() returns
 * non-zero in the current C locale. The wc_process_text() function:
 *
 *   - identifies words using isalpha(),
 *   - converts them to lowercase via tolower() (case-insensitive),
 *   - truncates long words to a configurable maximum length,
 *   - and inserts them into the table.
 *
 * All character classification and case-folding are performed via the
 * standard <ctype.h> functions, following STR37-C from the SEI CERT C
 * Coding Standard by casting to unsigned char before calling them.
 *
 * Important:
 *   - Behavior is locale-dependent. For portable ASCII-only behavior,
 *     ensure the "C" locale is active.
 *
 * -----------------------------------------------------------------------
 *  Thread-safety and reentrancy
 * -----------------------------------------------------------------------
 *
 * - Each wc_table instance is self-contained and uses no shared globals.
 * - The API is reentrant in the sense that different wc_table instances
 *   may be used concurrently by different threads with no interaction.
 * - A single wc_table is *not* internally synchronized. If you need to
 *   access the same table from multiple threads, protect it with a
 *   mutex or equivalent.
 *
 * -----------------------------------------------------------------------
 *  Error handling
 * -----------------------------------------------------------------------
 *
 * All fallible operations return wc_status:
 *
 *   - WC_OK                   – Success.
 *   - WC_ERR_INVALID_ARGUMENT – A precondition was violated (e.g. NULL).
 *   - WC_ERR_OUT_OF_MEMORY    – Dynamic allocation failed.
 *
 * Callers should always check the return value of functions marked
 * WC_NODISCARD.
 *
 * -----------------------------------------------------------------------
 *  Typical usage
 * -----------------------------------------------------------------------
 *
 * @code
 * #include "wordcount.h"
 * #include <stdio.h>
 * #include <string.h>
 *
 * void example(const char *buffer, size_t length) {
 *     wc_table *t = wc_create(NULL);
 *     if (!t) {
 *         // handle out-of-memory
 *         return;
 *     }
 *
 *     wc_status st = wc_process_text(t, buffer, length);
 *     if (st != WC_OK) {
 *         // handle error
 *         wc_destroy(t);
 *         return;
 *     }
 *
 *     wc_entry *entries = NULL;
 *     size_t    count   = 0;
 *
 *     st = wc_snapshot(t, &entries, &count);
 *     if (st == WC_OK) {
 *         for (size_t i = 0; i < count; ++i) {
 *             printf("%zu %s\n", entries[i].count, entries[i].word);
 *         }
 *     }
 *
 *     wc_free_snapshot(entries); // safe to call with NULL
 *     wc_destroy(t);
 * }
 * @endcode
 */

#ifndef WORDCOUNT_H_INCLUDED
#define WORDCOUNT_H_INCLUDED

/*===========================================================================
 * Version information (compile-time)
 *===========================================================================*/

/** @brief Library major version number. */
#define WC_VERSION_MAJOR 2

/** @brief Library minor version number. */
#define WC_VERSION_MINOR 1

/** @brief Library patch version number. */
#define WC_VERSION_PATCH 0

/** @brief Library version as a string literal "MAJOR.MINOR.PATCH". */
#define WC_VERSION_STRING "2.1.0"

/*===========================================================================
 * Feature-test / attribute macros
 *===========================================================================*/

/**
 * @def WC_NODISCARD
 * @brief Hint that the function's return value should not be ignored.
 *
 * Implemented using GCC/Clang attributes when available, and expands to
 * nothing on other compilers. This is purely advisory; the library's
 * behavior does not depend on it.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WC_NODISCARD __attribute__((warn_unused_result))
#else
#define WC_NODISCARD
#endif

/**
 * @def WC_PURE
 * @brief Hint that a function has no side effects except its return value.
 *
 * A WC_PURE function's result depends only on its arguments and on
 * global memory that is not modified during the call. This enables
 * better optimization by some compilers.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WC_PURE __attribute__((pure))
#else
#define WC_PURE
#endif

/*===========================================================================
 * Standard includes
 *===========================================================================*/

#include <stdbool.h> /* bool */
#include <stddef.h>  /* size_t, NULL */

/*===========================================================================
 * C++ compatibility
 *===========================================================================*/

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================
 * Public types
 *===========================================================================*/

/**
 * @brief Opaque handle to a word-count table.
 *
 * The internal structure is not exposed. All access goes through the
 * API functions declared in this header. This allows the implementation
 * to evolve without breaking callers.
 */
typedef struct wc_table wc_table;

/**
 * @brief A single word-count entry returned by wc_snapshot().
 *
 * The `word` pointer is owned by the table. It remains valid until the
 * owning table is destroyed with wc_destroy(). Do not free or modify
 * this pointer.
 */
typedef struct wc_entry
{
    const char *word; /**< NUL-terminated word string, owned by table. */
    size_t count;     /**< Number of occurrences of this word.        */
} wc_entry;

/**
 * @brief Status codes returned by all fallible API functions.
 */
typedef enum wc_status
{
    WC_OK = 0,               /**< Operation completed successfully.        */
    WC_ERR_INVALID_ARGUMENT, /**< A required argument was NULL or invalid. */
    WC_ERR_OUT_OF_MEMORY     /**< Dynamic allocation failed.               */
} wc_status;

/**
 * @brief Configuration options for wc_create().
 *
 * Pass NULL to wc_create() to use library defaults. Alternatively,
 * zero-initialize a wc_config instance and set only fields you wish
 * to override; zero values select sensible defaults.
 */
typedef struct wc_config
{
    /**
     * @brief Desired minimum number of hash buckets.
     *
     * The implementation rounds this up to the next power of two,
     * and enforces a minimum of 16,384 buckets.
     *
     * Values smaller than the default are treated as "use default".
     * Very large values are clamped to an implementation-defined
     * maximum to avoid pathological memory usage.
     *
     * A value of 0 selects the default (16,384 buckets).
     */
    size_t initial_capacity;

    /**
     * @brief Maximum stored word length in bytes, including the NUL
     *        terminator.
     *
     * When processing text via wc_process_text():
     *
     *   - Words longer than (max_word_length - 1) bytes are truncated
     *     to (max_word_length - 1) bytes.
     *   - The remainder of that word (still classified as isalpha())
     *     is skipped and not stored in any form.
     *   - Distinct long words that share the same leading
     *     (max_word_length - 1) bytes will be counted together.
     *
     * wc_add_word() does not impose this limit; it stores the word
     * as given.
     *
     * A value of 0 selects the default (64 bytes).
     * Values outside the range [8, 1024] are clamped to that range.
     */
    size_t max_word_length;
} wc_config;

/*===========================================================================
 * Lifecycle
 *===========================================================================*/

/**
 * @brief Create a new, empty word-count table.
 *
 * @param cfg  Optional configuration, or NULL for library defaults.
 *
 * @return New table handle on success, or NULL on allocation failure.
 *
 * The returned table must eventually be destroyed with wc_destroy()
 * to avoid memory leaks.
 *
 * Thread-safety:
 *   - Safe to call concurrently from multiple threads to create
 *     *different* tables.
 *   - The returned table itself is not thread-safe; guard it with
 *     external synchronization if you need concurrent access.
 */
WC_NODISCARD
wc_table *wc_create(const wc_config *cfg);

/**
 * @brief Destroy a table and release all associated resources.
 *
 * @param t  Table to destroy, or NULL.
 *
 * After this call, `t` must not be used again. Passing NULL is allowed
 * and has no effect.
 *
 * Thread-safety:
 *   - Do not destroy a table while it is being used by another thread.
 *   - Safe to destroy different tables concurrently.
 */
void wc_destroy(wc_table *t);

/*===========================================================================
 * Insertion / text processing
 *===========================================================================*/

/**
 * @brief Add a single, pre-processed word to the table.
 *
 * The word is copied into internal storage. If the word already
 * exists, its count is incremented; otherwise a new entry is created
 * with count 1.
 *
 * No normalization is performed. In particular:
 *   - No case-folding (comparison is case-sensitive).
 *   - No Unicode or locale conversion beyond what strcmp() provides.
 *
 * @param t     Table handle (must not be NULL).
 * @param word  NUL-terminated word string (must not be NULL).
 *
 * @return WC_OK on success, or an error status on failure.
 *
 * Special cases:
 *   - If `word` is the empty string "", the call succeeds and is a
 *     no-op (no entry is created).
 *
 * Complexity:
 *   - Amortized O(1) per insertion on average.
 *   - O(n) in the worst case of many hash collisions.
 */
WC_NODISCARD
wc_status wc_add_word(wc_table *t, const char *word);

/**
 * @brief Scan raw text and count all words.
 *
 * This function:
 *   - walks the input buffer once,
 *   - identifies maximal sequences for which isalpha() is non-zero,
 *   - lowercases those sequences with tolower(),
 *   - truncates them according to the table's max_word_length, and
 *   - inserts them via the same mechanism used by wc_add_word().
 *
 * @param t     Table handle (must not be NULL).
 * @param data  Pointer to input bytes, or NULL if @p len is 0.
 * @param len   Number of bytes in @p data.
 *
 * @return WC_OK on success, or an error status on failure.
 *
 * Error cases:
 *   - WC_ERR_INVALID_ARGUMENT if `t` is NULL, or `data` is NULL
 *     while `len` is non-zero.
 *   - WC_ERR_OUT_OF_MEMORY if allocation fails during processing.
 *
 * Notes:
 *   - The input buffer is never modified.
 *   - This function treats the buffer as a complete, standalone unit.
 *     It is *not* stream-aware: if words may span across chunk
 *     boundaries, you must handle that at a higher level (e.g. by
 *     buffering edges yourself or by passing full text blocks).
 */
WC_NODISCARD
wc_status wc_process_text(wc_table *t, const char *data, size_t len);

/*===========================================================================
 * Query functions
 *===========================================================================*/

/**
 * @brief Return the total number of word tokens processed.
 *
 * This is the sum of counts for all words. For example,
 * "the cat and the dog" has 5 total words.
 *
 * @param t  Table handle, or NULL.
 *
 * @return Total word count, or 0 if @p t is NULL.
 *
 * WC_PURE: the result depends only on the pointed-to table state.
 */
WC_PURE
size_t wc_total_words(const wc_table *t);

/**
 * @brief Return the number of distinct words in the table.
 *
 * For example, "the cat and the dog" has 4 unique words.
 *
 * @param t  Table handle, or NULL.
 *
 * @return Unique word count, or 0 if @p t is NULL.
 *
 * WC_PURE: the result depends only on the pointed-to table state.
 */
WC_PURE
size_t wc_unique_words(const wc_table *t);

/*===========================================================================
 * Snapshot / enumeration
 *===========================================================================*/

/**
 * @brief Take a sorted snapshot of all word-count entries.
 *
 * Allocates and returns an array of wc_entry structures containing a
 * view of all current words and their counts.
 *
 * The array is sorted by:
 *   1. Descending count (most frequent words first).
 *   2. Lexicographic order (ascending) for ties.
 *
 * @param t            Table handle (must not be NULL).
 * @param out_entries  Output: pointer to the allocated array, or NULL
 *                     on an empty table.
 * @param out_len      Output: number of entries in the array.
 *
 * @return WC_OK on success, or an error status on failure.
 *
 * Ownership and lifetime:
 *   - The array itself is owned by the caller and must be released
 *     with wc_free_snapshot().
 *   - Each wc_entry.word points into the table's internal storage and
 *     remains valid until wc_destroy() is called on that table.
 *   - Modifying or freeing wc_entry.word is undefined behavior.
 *
 * Empty tables:
 *   - On success when the table contains no words, *out_entries is
 *     set to NULL and *out_len is set to 0. This is not an error.
 */
WC_NODISCARD
wc_status
wc_snapshot(const wc_table *t, wc_entry **out_entries, size_t *out_len);

/**
 * @brief Free a snapshot array obtained from wc_snapshot().
 *
 * @param entries  Array returned by wc_snapshot(), or NULL.
 *
 * Passing NULL is allowed and has no effect. Callers must not use
 * wc_entry.word pointers after the underlying table is destroyed,
 * regardless of whether the snapshot array is still alive.
 */
void wc_free_snapshot(wc_entry *entries);

/*===========================================================================
 * Version information (runtime)
 *===========================================================================*/

/**
 * @brief Return the library version string at runtime.
 *
 * The returned pointer refers to a static NUL-terminated string in
 * "MAJOR.MINOR.PATCH" format. It is never NULL.
 *
 * This function is useful to verify that the compiled library matches
 * the headers used at build time.
 */
WC_PURE
const char *wc_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WORDCOUNT_H_INCLUDED */
