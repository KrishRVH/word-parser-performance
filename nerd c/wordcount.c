/**
 * @file wordcount.c
 * @brief Word frequency counting library - implementation.
 *
 * Implementation notes:
 *
 * - Hash table uses separate chaining with linked lists.
 * - FNV-1a 64-bit hash for good distribution on modern 64-bit systems.
 * - Nodes use flexible array members (C99) for single-allocation storage.
 * - Hash values are cached in nodes for O(1) rehashing and fast comparisons.
 * - Automatic table growth when load factor reaches 1.0.
 * - Character handling follows STR37-C (cast to unsigned char).
 *
 * @see wordcount.h for public API documentation.
 */

#include "wordcount.h"

#include <ctype.h>  /* isalpha, tolower */
#include <stdint.h> /* uint64_t, UINT64_C */
#include <stdlib.h> /* malloc, calloc, free, qsort */
#include <string.h> /* memcpy, strcmp */

/*===========================================================================
 * Version (must match header)
 *===========================================================================*/

#if WC_VERSION_MAJOR != 2 || WC_VERSION_MINOR != 1 || WC_VERSION_PATCH != 0
#error "Version mismatch between wordcount.h and wordcount.c"
#endif

/*===========================================================================
 * Internal configuration constants
 *===========================================================================*/

enum
{
    WC_DEFAULT_HASH_BITS = 14, /* 2^14 = 16,384 buckets */
    WC_MAX_HASH_BITS = 24,     /* 2^24 = 16,777,216 buckets */
    WC_DEFAULT_MAX_WORD = 64,
    WC_MIN_MAX_WORD = 8,
    WC_MAX_MAX_WORD = 1024
};

/*===========================================================================
 * FNV-1a hash constants
 *===========================================================================*/

static const uint64_t FNV64_OFFSET_BASIS = UINT64_C(14695981039346656037);
static const uint64_t FNV64_PRIME = UINT64_C(1099511628211);

/*===========================================================================
 * Internal data structures
 *===========================================================================*/

/**
 * @brief A single node in the hash table chain.
 *
 * Uses a flexible array member (C99 §6.7.2.1) to store the word inline,
 * eliminating a separate allocation and improving cache locality.
 * The hash is cached to avoid recomputation during rehashing and to
 * enable fast-path rejection during lookup.
 */
typedef struct wc_node
{
    struct wc_node *next;
    uint64_t hash;
    size_t count;
    char word[];
} wc_node;

/**
 * @brief The word-count hash table.
 */
struct wc_table
{
    wc_node **buckets;
    size_t bucket_count;
    size_t total_words;
    size_t unique_words;
    size_t max_word_len;
};

/**
 * @brief A non-owning view into a character sequence.
 *
 * Used for zero-copy tokenization; the span points into the original
 * input buffer and is only valid for the lifetime of that buffer.
 */
typedef struct
{
    const char *ptr;
    size_t len;
} wc_span;

/*===========================================================================
 * Internal helper functions
 *===========================================================================*/

static inline size_t clamp_size(size_t val, size_t lo, size_t hi)
{
    return val < lo ? lo : (val > hi ? hi : val);
}

/**
 * @brief Compute FNV-1a 64-bit hash, returning length as a side effect.
 *
 * Single-pass computation of both hash and length. The length output
 * is optional (pass NULL if not needed).
 */
static uint64_t fnv1a_len(const char *str, size_t *out_len)
{
    uint64_t hash = FNV64_OFFSET_BASIS;
    const unsigned char *p = (const unsigned char *)str;

    while (*p)
    {
        hash ^= *p;
        hash *= FNV64_PRIME;
        ++p;
    }

    if (out_len)
    {
        *out_len = (size_t)(p - (const unsigned char *)str);
    }
    return hash;
}

/**
 * @brief Compute FNV-1a 64-bit hash over a sized buffer.
 */
static uint64_t fnv1a_buf(const char *buf, size_t len)
{
    uint64_t hash = FNV64_OFFSET_BASIS;
    const unsigned char *p = (const unsigned char *)buf;
    const unsigned char *end = p + len;

    while (p < end)
    {
        hash ^= *p++;
        hash *= FNV64_PRIME;
    }
    return hash;
}

/**
 * @brief Compute bucket index from hash.
 *
 * Bucket count is always a power of two, so bitwise AND replaces modulo.
 */
static inline size_t bucket_index(uint64_t hash, size_t bucket_count)
{
    return (size_t)(hash & (bucket_count - 1));
}

/**
 * @brief Allocate a node with inline word storage.
 *
 * The word is copied into the flexible array member, resulting in a
 * single contiguous allocation.
 */
static wc_node *create_node(const char *word, size_t len, uint64_t hash)
{
    wc_node *node = malloc(sizeof(*node) + len + 1);
    if (!node)
    {
        return NULL;
    }

    node->next = NULL;
    node->hash = hash;
    node->count = 1;
    memcpy(node->word, word, len);
    node->word[len] = '\0';

    return node;
}

/**
 * @brief Find the slot for a word, or the insertion point if not present.
 *
 * Returns a pointer to the node pointer - either the existing node's
 * location in the chain, or the NULL terminator where a new node should
 * be linked. This pointer-to-pointer pattern enables clean insert/update
 * without separate code paths.
 *
 * The hash is compared first as a fast-path rejection before the more
 * expensive strcmp.
 */
static wc_node **
find_slot(wc_table *t, const char *word, size_t len, uint64_t hash)
{
    size_t idx = bucket_index(hash, t->bucket_count);
    wc_node **slot = &t->buckets[idx];

    while (*slot)
    {
        if ((*slot)->hash == hash && strcmp((*slot)->word, word) == 0)
        {
            return slot;
        }
        slot = &(*slot)->next;
    }
    return slot;
}

/**
 * @brief Grow the hash table by doubling its size.
 *
 * Rehashing is O(n) but does not recompute hashes - cached hash values
 * are used to compute new bucket indices directly.
 */
static wc_status grow_table(wc_table *t)
{
    const size_t max_buckets = (size_t)1u << WC_MAX_HASH_BITS;
    if (t->bucket_count >= max_buckets)
    {
        return WC_OK;
    }

    size_t new_count = t->bucket_count * 2;
    wc_node **new_buckets = calloc(new_count, sizeof(*new_buckets));
    if (!new_buckets)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < t->bucket_count; ++i)
    {
        wc_node *node = t->buckets[i];
        while (node)
        {
            wc_node *next = node->next;
            size_t idx = bucket_index(node->hash, new_count);

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
 * @brief Extract the next alphabetic word from a buffer.
 *
 * Returns a span pointing into the original buffer. The span is not
 * null-terminated and not normalized (not lowercased). Normalization
 * is the caller's responsibility, which allows deferring the copy
 * until we know we need it.
 *
 * Returns a zero-length span when no more words remain.
 */
static wc_span next_word(const char **pos, const char *end)
{
    const unsigned char *p = (const unsigned char *)*pos;
    const unsigned char *e = (const unsigned char *)end;

    while (p < e && !isalpha(*p))
    {
        ++p;
    }

    const unsigned char *start = p;
    while (p < e && isalpha(*p))
    {
        ++p;
    }

    *pos = (const char *)p;
    return (wc_span){ (const char *)start, (size_t)(p - start) };
}

/**
 * @brief Normalize a word span into a buffer (lowercase copy).
 *
 * Writes at most buf_size - 1 characters plus a null terminator.
 * Returns the number of characters written (excluding terminator),
 * which may be less than span.len if truncation occurred.
 */
static size_t normalize_word(wc_span span, char *buf, size_t buf_size)
{
    size_t max = buf_size - 1;
    size_t len = span.len < max ? span.len : max;
    const unsigned char *src = (const unsigned char *)span.ptr;

    for (size_t i = 0; i < len; ++i)
    {
        buf[i] = (char)tolower(src[i]);
    }
    buf[len] = '\0';

    return len;
}

/**
 * @brief Add a pre-normalized word to the table.
 */
static wc_status
add_word_internal(wc_table *t, const char *word, size_t len, uint64_t hash)
{
    if (len == 0)
    {
        return WC_OK;
    }

    /*
     * Grow before insert to maintain load factor <= 1.0.
     * This ensures the current insert has reasonable chain lengths.
     */
    if (t->unique_words >= t->bucket_count)
    {
        wc_status st = grow_table(t);
        if (st != WC_OK)
        {
            return st;
        }
    }

    wc_node **slot = find_slot(t, word, len, hash);

    if (*slot)
    {
        (*slot)->count++;
    }
    else
    {
        wc_node *node = create_node(word, len, hash);
        if (!node)
        {
            return WC_ERR_OUT_OF_MEMORY;
        }
        *slot = node;
        t->unique_words++;
    }

    t->total_words++;
    return WC_OK;
}

/**
 * @brief Comparison function for qsort: descending count, ascending word.
 */
static int compare_entries(const void *a, const void *b)
{
    const wc_entry *ea = (const wc_entry *)a;
    const wc_entry *eb = (const wc_entry *)b;

    if (ea->count > eb->count)
        return -1;
    if (ea->count < eb->count)
        return 1;
    return strcmp(ea->word, eb->word);
}

/*===========================================================================
 * Public API implementation
 *===========================================================================*/

wc_table *wc_create(const wc_config *cfg)
{
    unsigned hash_bits = WC_DEFAULT_HASH_BITS;
    size_t max_word = WC_DEFAULT_MAX_WORD;

    if (cfg)
    {
        if (cfg->initial_capacity > 0)
        {
            size_t cap = cfg->initial_capacity;
            size_t default_cap = (size_t)1u << WC_DEFAULT_HASH_BITS;

            if (cap < default_cap)
            {
                cap = default_cap;
            }

            unsigned bits = WC_DEFAULT_HASH_BITS;
            size_t size = default_cap;

            while (size < cap && bits < WC_MAX_HASH_BITS)
            {
                size <<= 1;
                ++bits;
            }
            hash_bits = bits;
        }

        if (cfg->max_word_length > 0)
        {
            max_word = clamp_size(
                    cfg->max_word_length, WC_MIN_MAX_WORD, WC_MAX_MAX_WORD);
        }
    }

    wc_table *t = malloc(sizeof(*t));
    if (!t)
    {
        return NULL;
    }

    size_t bucket_count = (size_t)1u << hash_bits;
    t->buckets = calloc(bucket_count, sizeof(*t->buckets));
    if (!t->buckets)
    {
        free(t);
        return NULL;
    }

    t->bucket_count = bucket_count;
    t->total_words = 0;
    t->unique_words = 0;
    t->max_word_len = max_word;

    return t;
}

void wc_destroy(wc_table *t)
{
    if (!t)
    {
        return;
    }

    for (size_t i = 0; i < t->bucket_count; ++i)
    {
        wc_node *node = t->buckets[i];
        while (node)
        {
            wc_node *next = node->next;
            free(node);
            node = next;
        }
    }

    free(t->buckets);
    free(t);
}

wc_status wc_add_word(wc_table *t, const char *word)
{
    if (!t || !word)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }

    size_t len;
    uint64_t hash = fnv1a_len(word, &len);

    return add_word_internal(t, word, len, hash);
}

wc_status wc_process_text(wc_table *t, const char *data, size_t len)
{
    if (!t)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }
    if (!data && len != 0)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }
    if (len == 0)
    {
        return WC_OK;
    }

    char *buf = malloc(t->max_word_len);
    if (!buf)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    const char *pos = data;
    const char *end = data + len;
    wc_status status = WC_OK;

    while (pos < end)
    {
        wc_span span = next_word(&pos, end);
        if (span.len == 0)
        {
            break;
        }

        size_t word_len = normalize_word(span, buf, t->max_word_len);
        if (word_len > 0)
        {
            uint64_t hash = fnv1a_buf(buf, word_len);
            status = add_word_internal(t, buf, word_len, hash);
            if (status != WC_OK)
            {
                break;
            }
        }
    }

    free(buf);
    return status;
}

size_t wc_total_words(const wc_table *t)
{
    return t ? t->total_words : 0;
}

size_t wc_unique_words(const wc_table *t)
{
    return t ? t->unique_words : 0;
}

wc_status
wc_snapshot(const wc_table *t, wc_entry **out_entries, size_t *out_len)
{
    if (!t || !out_entries || !out_len)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }

    if (t->unique_words == 0)
    {
        *out_entries = NULL;
        *out_len = 0;
        return WC_OK;
    }

    wc_entry *entries = malloc(t->unique_words * sizeof(*entries));
    if (!entries)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    size_t idx = 0;
    for (size_t i = 0; i < t->bucket_count; ++i)
    {
        for (wc_node *node = t->buckets[i]; node; node = node->next)
        {
            entries[idx].word = node->word;
            entries[idx].count = node->count;
            ++idx;
        }
    }

    qsort(entries, t->unique_words, sizeof(*entries), compare_entries);

    *out_entries = entries;
    *out_len = t->unique_words;

    return WC_OK;
}

void wc_free_snapshot(wc_entry *entries)
{
    free(entries);
}

const char *wc_version(void)
{
    return WC_VERSION_STRING;
}