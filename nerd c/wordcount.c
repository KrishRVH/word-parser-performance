/**
 * @file wordcount.c
 * @brief Word frequency counting library - implementation.
 *
 * Implementation notes:
 *
 * - Hash table uses separate chaining with linked lists.
 * - FNV-1a 64-bit hash for good distribution on modern 64-bit systems.
 * - Automatic table growth when load factor exceeds 1.0.
 * - CERT C compliant memory management (MEM00-C, MEM04-C, etc.).
 * - Character handling follows STR37-C (cast to unsigned char).
 *
 * @see wordcount.h for public API documentation.
 */

#include "wordcount.h"

#include <assert.h> /* assert */
#include <ctype.h>  /* isalpha, tolower */
#include <stdint.h> /* uint32_t, uint64_t, UINT64_C */
#include <stdlib.h> /* malloc, calloc, free, qsort */
#include <string.h> /* memcpy, strcmp, strlen */

/*===========================================================================
 * Version (must match header)
 *===========================================================================*/

#if WC_VERSION_MAJOR != 2 || WC_VERSION_MINOR != 1 || WC_VERSION_PATCH != 0
#error "Version mismatch between wordcount.h and wordcount.c"
#endif

/*===========================================================================
 * Internal configuration constants
 *===========================================================================*/

/**
 * @brief Internal limits and defaults.
 *
 * Using an anonymous enum for integral constants is idiomatic C and
 * ensures the values are true compile-time constants (unlike const variables).
 */
enum {
    /** Default hash table size: 2^14 = 16,384 buckets. */
    WC_DEFAULT_HASH_BITS = 14,

    /** Maximum hash table size: 2^24 = 16,777,216 buckets. */
    WC_MAX_HASH_BITS = 24,

    /** Default maximum word length (including NUL terminator). */
    WC_DEFAULT_MAX_WORD = 64,

    /** Minimum allowed max_word_length. */
    WC_MIN_MAX_WORD = 8,

    /** Maximum allowed max_word_length. */
    WC_MAX_MAX_WORD = 1024
};

/*===========================================================================
 * FNV-1a hash constants
 *===========================================================================*/

/**
 * @brief FNV-1a 64-bit offset basis.
 *
 * This is the standard FNV offset basis for 64-bit hashes.
 * @see http://www.isthe.com/chongo/tech/comp/fnv/
 */
static const uint64_t FNV64_OFFSET_BASIS = UINT64_C(14695981039346656037);

/**
 * @brief FNV-1a 64-bit prime.
 *
 * This is the standard FNV prime for 64-bit hashes.
 */
static const uint64_t FNV64_PRIME = UINT64_C(1099511628211);

/*===========================================================================
 * Internal data structures
 *===========================================================================*/

/**
 * @brief A single node in the hash table chain.
 */
typedef struct wc_node {
    struct wc_node* next; /**< Next node in chain, or NULL. */
    size_t count;         /**< Occurrence count for this word. */
    char* word;           /**< Heap-allocated, NUL-terminated word. */
} wc_node;

/**
 * @brief The word-count hash table.
 */
struct wc_table {
    wc_node** buckets;   /**< Array of bucket chain heads. */
    size_t bucket_count; /**< Number of buckets (always power of 2). */
    size_t total_words;  /**< Total tokens processed. */
    size_t unique_words; /**< Number of distinct entries. */
    size_t max_word_len; /**< Max word length including NUL. */
};

/*===========================================================================
 * Internal helper functions
 *===========================================================================*/

/**
 * @brief Clamp a value to a range [min_val, max_val].
 *
 * @param value    Value to clamp.
 * @param min_val  Minimum allowed value.
 * @param max_val  Maximum allowed value.
 * @return         Clamped value.
 */
static inline size_t clamp_size(size_t value, size_t min_val, size_t max_val)
{
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

/**
 * @brief Compute FNV-1a 64-bit hash over a byte buffer.
 *
 * FNV-1a is a fast, non-cryptographic hash with good distribution
 * properties for hash tables. The 64-bit variant is preferred on
 * modern 64-bit systems.
 *
 * @param data  Pointer to data bytes.
 * @param len   Number of bytes to hash.
 * @return      64-bit hash value.
 *
 * @note This function handles the empty case (len == 0) correctly,
 *       returning the offset basis.
 */
static uint64_t fnv1a_hash(const void* data, size_t len)
{
    const unsigned char* bytes = (const unsigned char*)data;
    uint64_t hash = FNV64_OFFSET_BASIS;

    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= FNV64_PRIME;
    }

    return hash;
}

/**
 * @brief Compute hash of a NUL-terminated string.
 *
 * @param str  NUL-terminated string.
 * @return     64-bit hash value.
 */
static uint64_t fnv1a_str(const char* str)
{
    return fnv1a_hash(str, strlen(str));
}

/**
 * @brief Duplicate a string of known length.
 *
 * This is a portable replacement for `strndup()` which may not be
 * available in strict ISO C environments.
 *
 * @param str  Source string (need not be NUL-terminated).
 * @param len  Number of characters to copy.
 * @return     Newly allocated NUL-terminated copy, or NULL on failure.
 *
 * @note Caller is responsible for freeing the returned memory.
 */
static char* dup_string_n(const char* str, size_t len)
{
    /*
     * MEM04-C: Beware of zero-length allocations.
     * malloc(0) behavior is implementation-defined; we always allocate
     * at least 1 byte for the NUL terminator.
     */
    char* copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }

    if (len > 0) {
        memcpy(copy, str, len);
    }
    copy[len] = '\0';

    return copy;
}

/**
 * @brief Compute the bucket index for a hash value.
 *
 * Uses bitwise AND since bucket_count is always a power of 2.
 * This is faster than modulo.
 *
 * @param hash          64-bit hash value.
 * @param bucket_count  Number of buckets (must be power of 2).
 * @return              Bucket index in [0, bucket_count - 1].
 */
static inline size_t bucket_index(uint64_t hash, size_t bucket_count)
{
    return (size_t)(hash & (bucket_count - 1));
}

/**
 * @brief Grow the hash table by doubling its size.
 *
 * Rehashes all existing entries into the new, larger bucket array.
 *
 * @param t  Table to grow (must not be NULL).
 * @return   WC_OK on success, WC_ERR_OUT_OF_MEMORY on failure.
 *
 * @note On failure, the original table is unchanged.
 */
static wc_status grow_table(wc_table* t)
{
    /* Don't grow beyond the maximum. */
    const size_t max_buckets = (size_t)1u << WC_MAX_HASH_BITS;
    if (t->bucket_count >= max_buckets) {
        return WC_OK;
    }

    const size_t new_count = t->bucket_count * 2;

    /*
     * calloc initializes to NULL pointers, which is what we need
     * for empty bucket chains.
     */
    wc_node** new_buckets = calloc(new_count, sizeof(*new_buckets));
    if (new_buckets == NULL) {
        return WC_ERR_OUT_OF_MEMORY;
    }

    /* Rehash all existing entries. */
    for (size_t i = 0; i < t->bucket_count; ++i) {
        wc_node* node = t->buckets[i];
        while (node != NULL) {
            wc_node* next = node->next;

            const uint64_t hash = fnv1a_str(node->word);
            const size_t idx = bucket_index(hash, new_count);

            node->next = new_buckets[idx];
            new_buckets[idx] = node;

            node = next;
        }
    }

    free(t->buckets);
    t->buckets = new_buckets;
    t->bucket_count = new_count;

    return WC_OK;
}

/**
 * @brief Add a word of known length to the table.
 *
 * Internal function that does not validate arguments (caller must).
 * The word is assumed to already be normalized (e.g., lowercase).
 *
 * @param t    Table handle.
 * @param word Word string.
 * @param len  Length of word (excluding NUL terminator).
 * @return     WC_OK on success, WC_ERR_OUT_OF_MEMORY on failure.
 */
static wc_status add_word_internal(wc_table* t, const char* word, size_t len)
{
    /* Empty words are silently ignored. */
    if (len == 0) {
        return WC_OK;
    }

    /*
     * Load factor control: grow when unique_words exceeds bucket_count.
     * This keeps the average chain length at or below 1.0.
     */
    if (t->unique_words > t->bucket_count) {
        const wc_status st = grow_table(t);
        if (st != WC_OK) {
            return st;
        }
    }

    const uint64_t hash = fnv1a_hash(word, len);
    const size_t idx = bucket_index(hash, t->bucket_count);

    /* Search for existing entry. */
    for (wc_node* node = t->buckets[idx]; node != NULL; node = node->next) {
        if (strcmp(node->word, word) == 0) {
            node->count++;
            t->total_words++;
            return WC_OK;
        }
    }

    /* Create new entry. */
    char* word_copy = dup_string_n(word, len);
    if (word_copy == NULL) {
        return WC_ERR_OUT_OF_MEMORY;
    }

    wc_node* new_node = malloc(sizeof(*new_node));
    if (new_node == NULL) {
        free(word_copy);
        return WC_ERR_OUT_OF_MEMORY;
    }

    new_node->word = word_copy;
    new_node->count = 1;
    new_node->next = t->buckets[idx];
    t->buckets[idx] = new_node;

    t->total_words++;
    t->unique_words++;

    return WC_OK;
}

/**
 * @brief Extract the next word from a text buffer.
 *
 * Scans forward from the current position to find the next alphabetic word,
 * converts it to lowercase, and writes it to the output buffer.
 *
 * @param pos       Current position in input (updated on return).
 * @param end       One past the last byte of input.
 * @param out       Output buffer for the word.
 * @param out_size  Size of output buffer (must be > 0).
 * @param out_len   Output: length of extracted word (may be 0).
 * @return          true if a word was found, false if end of input reached.
 *
 * @note Words longer than (out_size - 1) are truncated; the remaining
 *       alphabetic characters are skipped.
 *
 * @internal This is an internal function. Callers must ensure:
 *           - pos, out, out_len are non-NULL
 *           - out_size > 0
 *           These preconditions are asserted in debug builds.
 */
static bool next_word(const char** pos,
                      const char* end,
                      char* out,
                      size_t out_size,
                      size_t* out_len)
{
    /* Precondition assertions (active in debug builds). */
    assert(pos != NULL);
    assert(out != NULL);
    assert(out_size > 0);
    assert(out_len != NULL);

    const unsigned char* p = (const unsigned char*)*pos;
    const unsigned char* e = (const unsigned char*)end;

    /* Skip non-alphabetic characters. */
    while (p < e && !isalpha(*p)) {
        ++p;
    }

    if (p >= e) {
        out[0] = '\0';
        *out_len = 0;
        *pos = (const char*)p;
        return false;
    }

    /* Extract and lowercase the word. */
    size_t len = 0;
    const size_t max_len = out_size - 1;

    while (p < e && isalpha(*p)) {
        if (len < max_len) {
            /*
             * STR37-C: Arguments to character-handling functions must be
             * representable as an unsigned char. The pointer `p` is already
             * unsigned char*, so *p is in the correct range.
             */
            out[len++] = (char)tolower(*p);
        }
        ++p;
    }

    out[len] = '\0';
    *out_len = len;
    *pos = (const char*)p;

    return true;
}

/**
 * @brief Comparison function for qsort: descending count, then ascending word.
 *
 * @param a  Pointer to first wc_entry.
 * @param b  Pointer to second wc_entry.
 * @return   Negative if a < b, positive if a > b, zero if equal.
 */
static int compare_entries(const void* a, const void* b)
{
    const wc_entry* ea = (const wc_entry*)a;
    const wc_entry* eb = (const wc_entry*)b;

    /* Descending by count. */
    if (ea->count > eb->count) {
        return -1;
    }
    if (ea->count < eb->count) {
        return 1;
    }

    /* Ascending by word for ties. */
    return strcmp(ea->word, eb->word);
}

/*===========================================================================
 * Public API implementation
 *===========================================================================*/

wc_table* wc_create(const wc_config* cfg)
{
    unsigned hash_bits = WC_DEFAULT_HASH_BITS;
    size_t max_word = WC_DEFAULT_MAX_WORD;

    if (cfg != NULL) {
        /*
         * Process initial_capacity: clamp to at least default, then round
         * up to the nearest power of two.
         */
        if (cfg->initial_capacity > 0) {
            const size_t default_cap = (size_t)1u << WC_DEFAULT_HASH_BITS;
            size_t cap = cfg->initial_capacity;

            /* Clamp to at least the default to avoid accidentally tiny tables.
             */
            if (cap < default_cap) {
                cap = default_cap;
            }

            unsigned bits = WC_DEFAULT_HASH_BITS;
            size_t size = default_cap;

            while (size < cap && bits < WC_MAX_HASH_BITS) {
                size <<= 1;
                ++bits;
            }

            hash_bits = bits;
        }

        /* Process max_word_length. */
        if (cfg->max_word_length > 0) {
            max_word = clamp_size(
                    cfg->max_word_length, WC_MIN_MAX_WORD, WC_MAX_MAX_WORD);
        }
    }

    const size_t bucket_count = (size_t)1u << hash_bits;

    /*
     * Allocate table structure first, then buckets.
     * Use goto-cleanup pattern (MEM12-C) for error handling.
     */
    wc_table* t = malloc(sizeof(*t));
    if (t == NULL) {
        return NULL;
    }

    t->buckets = calloc(bucket_count, sizeof(*t->buckets));
    if (t->buckets == NULL) {
        free(t);
        return NULL;
    }

    t->bucket_count = bucket_count;
    t->total_words = 0;
    t->unique_words = 0;
    t->max_word_len = max_word;

    return t;
}

void wc_destroy(wc_table* t)
{
    if (t == NULL) {
        return;
    }

    /* Free all nodes in all bucket chains. */
    for (size_t i = 0; i < t->bucket_count; ++i) {
        wc_node* node = t->buckets[i];
        while (node != NULL) {
            wc_node* next = node->next;
            free(node->word);
            free(node);
            node = next;
        }
    }

    free(t->buckets);
    free(t);
}

wc_status wc_add_word(wc_table* t, const char* word)
{
    if (t == NULL || word == NULL) {
        return WC_ERR_INVALID_ARGUMENT;
    }

    return add_word_internal(t, word, strlen(word));
}

wc_status wc_process_text(wc_table* t, const char* data, size_t len)
{
    if (t == NULL) {
        return WC_ERR_INVALID_ARGUMENT;
    }
    if (data == NULL && len != 0) {
        return WC_ERR_INVALID_ARGUMENT;
    }
    if (len == 0) {
        return WC_OK;
    }

    /*
     * Allocate word buffer. Using malloc instead of VLA for:
     * 1. Explicit error handling (VLAs can silently overflow the stack).
     * 2. Configurable max_word_len that could be large.
     */
    char* buffer = malloc(t->max_word_len);
    if (buffer == NULL) {
        return WC_ERR_OUT_OF_MEMORY;
    }

    const char* pos = data;
    const char* end = data + len;
    size_t word_len;
    wc_status status = WC_OK;

    while (next_word(&pos, end, buffer, t->max_word_len, &word_len)) {
        if (word_len > 0) {
            status = add_word_internal(t, buffer, word_len);
            if (status != WC_OK) {
                break;
            }
        }
    }

    free(buffer);
    return status;
}

size_t wc_total_words(const wc_table* t)
{
    return (t != NULL) ? t->total_words : 0;
}

size_t wc_unique_words(const wc_table* t)
{
    return (t != NULL) ? t->unique_words : 0;
}

wc_status
wc_snapshot(const wc_table* t, wc_entry** out_entries, size_t* out_len)
{
    if (t == NULL || out_entries == NULL || out_len == NULL) {
        return WC_ERR_INVALID_ARGUMENT;
    }

    /* Handle empty table case. */
    if (t->unique_words == 0) {
        *out_entries = NULL;
        *out_len = 0;
        return WC_OK;
    }

    /*
     * MEM04-C: Don't allocate zero bytes.
     * We've already handled unique_words == 0 above.
     */
    wc_entry* entries = malloc(t->unique_words * sizeof(*entries));
    if (entries == NULL) {
        return WC_ERR_OUT_OF_MEMORY;
    }

    /* Collect all entries. */
    size_t idx = 0;
    for (size_t i = 0; i < t->bucket_count; ++i) {
        for (wc_node* node = t->buckets[i]; node != NULL; node = node->next) {
            entries[idx].word = node->word;
            entries[idx].count = node->count;
            ++idx;
        }
    }

    /* Sort: descending count, ascending word. */
    qsort(entries, t->unique_words, sizeof(*entries), compare_entries);

    *out_entries = entries;
    *out_len = t->unique_words;

    return WC_OK;
}

void wc_free_snapshot(wc_entry* entries)
{
    /*
     * MEM34-C: Only free memory allocated dynamically.
     * NULL is safe to pass to free().
     */
    free(entries);
}

const char* wc_version(void)
{
    return WC_VERSION_STRING;
}
