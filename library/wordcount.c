/**
 * @file wordcount.c
 * @brief Word frequency counting library – implementation.
 *
 * Design overview
 * ---------------
 *
 * - Hash table:
 *     - Separate chaining with forward singly-linked lists.
 *     - Bucket count is always a power of two.
 *     - FNV-1a 64-bit hash for good distribution.
 *     - Hashes are cached in nodes; never recomputed.
 *
 * - Storage:
 *     - Each node uses a flexible array member (C99) for the word,
 *       so the node and its string live in a single allocation.
 *
 * - Growth strategy:
 *     - Table doubles its bucket count when load factor would exceed
 *       1.0 (unique_words >= bucket_count before insertion).
 *     - Maximum number of buckets is bounded by WC_MAX_HASH_BITS.
 *
 * - Character handling:
 *     - isalpha() and tolower() are always fed values cast from
 *       unsigned char, per STR37-C.
 *
 * - Portability:
 *     - The code is ISO C99, with optional GCC/Clang attributes
 *       controlled by macros in the public header.
 */

#include "wordcount.h"

#include <assert.h> /* assert */
#include <ctype.h>  /* isalpha, tolower */
#include <stdint.h> /* uint64_t, UINT64_C */
#include <stdlib.h> /* malloc, calloc, realloc, free, qsort */
#include <string.h> /* memcpy, strcmp */

/*===========================================================================
 * Version consistency check (header vs implementation)
 *===========================================================================*/

#if WC_VERSION_MAJOR != 2 || WC_VERSION_MINOR != 1 || WC_VERSION_PATCH != 0
#error "wordcount: version mismatch between wordcount.h and wordcount.c"
#endif

/*===========================================================================
 * Internal configuration constants
 *===========================================================================*/

enum
{
    WC_DEFAULT_HASH_BITS = 14, /* 2^14 = 16,384 buckets */
    WC_MAX_HASH_BITS = 24      /* 2^24 = 16,777,216 buckets */
};

enum
{
    WC_DEFAULT_MAX_WORD = 64,
    WC_MIN_MAX_WORD = 8,
    WC_MAX_MAX_WORD = 1024
};

/*===========================================================================
 * FNV-1a 64-bit hash constants
 *===========================================================================*/

static const uint64_t FNV64_OFFSET_BASIS = UINT64_C(14695981039346656037);
static const uint64_t FNV64_PRIME = UINT64_C(1099511628211);

/*===========================================================================
 * Internal data structures
 *===========================================================================*/

typedef struct wc_node
{
    struct wc_node *next;
    uint64_t hash;
    size_t count;
    char word[]; /* NUL-terminated */
} wc_node;

struct wc_table
{
    wc_node **buckets;
    size_t bucket_count;

    size_t total_words;
    size_t unique_words;

    size_t max_word_len;
};

typedef struct
{
    const char *ptr;
    size_t len;
} wc_span;

/*===========================================================================
 * Internal helper functions
 *===========================================================================*/

static size_t wc_clamp_size(size_t value, size_t min_value, size_t max_value)
{
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

static uint64_t wc_fnv1a_str(const char *str, size_t *out_len)
{
    uint64_t hash = FNV64_OFFSET_BASIS;
    const unsigned char *p = (const unsigned char *)str;

    while (*p != '\0')
    {
        hash ^= (uint64_t)(*p);
        hash *= FNV64_PRIME;
        ++p;
    }

    if (out_len != NULL)
    {
        *out_len = (size_t)(p - (const unsigned char *)str);
    }
    return hash;
}

static uint64_t wc_fnv1a_buf(const char *buf, size_t len)
{
    uint64_t hash = FNV64_OFFSET_BASIS;
    const unsigned char *p = (const unsigned char *)buf;
    const unsigned char *end = p + len;

    while (p < end)
    {
        hash ^= (uint64_t)(*p);
        hash *= FNV64_PRIME;
        ++p;
    }
    return hash;
}

static size_t wc_bucket_index(uint64_t hash, size_t bucket_count)
{
    assert(bucket_count != 0U);
    assert((bucket_count & (bucket_count - 1U)) == 0U);
    return (size_t)(hash & (uint64_t)(bucket_count - 1U));
}

static wc_node *wc_create_node(const char *word, size_t len, uint64_t hash)
{
    wc_node *node = (wc_node *)malloc(sizeof(*node) + len + 1U);
    if (node == NULL)
    {
        return NULL;
    }

    node->next = NULL;
    node->hash = hash;
    node->count = 1U;

    if (len > 0U)
    {
        (void)memcpy(node->word, word, len);
    }
    node->word[len] = '\0';

    return node;
}

static wc_node **
wc_find_slot(wc_table *t, const char *word, size_t len, uint64_t hash)
{
    (void)len;

    const size_t index = wc_bucket_index(hash, t->bucket_count);
    wc_node **slot = &t->buckets[index];

    while (*slot != NULL)
    {
        wc_node *candidate = *slot;

        if (candidate->hash == hash && strcmp(candidate->word, word) == 0)
        {
            break;
        }
        slot = &candidate->next;
    }

    return slot;
}

static wc_status wc_grow_table(wc_table *t)
{
    const size_t max_buckets = (size_t)1U << WC_MAX_HASH_BITS;

    if (t->bucket_count >= max_buckets)
    {
        return WC_OK;
    }

    const size_t new_count = t->bucket_count * 2U;
    wc_node **new_buckets = (wc_node **)calloc(new_count, sizeof(*new_buckets));
    if (new_buckets == NULL)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0U; i < t->bucket_count; ++i)
    {
        wc_node *node = t->buckets[i];
        while (node != NULL)
        {
            wc_node *next = node->next;

            const size_t new_index = wc_bucket_index(node->hash, new_count);
            node->next = new_buckets[new_index];
            new_buckets[new_index] = node;

            node = next;
        }
    }

    free(t->buckets);
    t->buckets = new_buckets;
    t->bucket_count = new_count;

    return WC_OK;
}

static wc_span wc_next_word(const char **pos, const char *end)
{
    const unsigned char *p = (const unsigned char *)(*pos);
    const unsigned char *e = (const unsigned char *)end;

    while (p < e && isalpha((int)(*p)) == 0)
    {
        ++p;
    }

    const unsigned char *start = p;

    while (p < e && isalpha((int)(*p)) != 0)
    {
        ++p;
    }

    *pos = (const char *)p;

    wc_span span;
    span.ptr = (const char *)start;
    span.len = (size_t)(p - start);
    return span;
}

static size_t wc_normalize_word(wc_span span, char *buf, size_t buf_size)
{
    const size_t max_copy = buf_size - 1U;
    const size_t to_copy = (span.len < max_copy) ? span.len : max_copy;

    const unsigned char *src = (const unsigned char *)span.ptr;

    for (size_t i = 0U; i < to_copy; ++i)
    {
        buf[i] = (char)tolower((int)(src[i]));
    }
    buf[to_copy] = '\0';

    return to_copy;
}

static wc_status
wc_add_word_internal(wc_table *t, const char *word, size_t len, uint64_t hash)
{
    if (len == 0U)
    {
        return WC_OK;
    }

    if (t->unique_words >= t->bucket_count)
    {
        wc_status st = wc_grow_table(t);
        if (st != WC_OK)
        {
            return st;
        }
    }

    wc_node **slot = wc_find_slot(t, word, len, hash);

    if (*slot != NULL)
    {
        ++((*slot)->count);
    }
    else
    {
        wc_node *node = wc_create_node(word, len, hash);
        if (node == NULL)
        {
            return WC_ERR_OUT_OF_MEMORY;
        }

        *slot = node;
        ++(t->unique_words);
    }

    ++(t->total_words);
    return WC_OK;
}

static int wc_compare_entries(const void *a, const void *b)
{
    const wc_entry *ea = (const wc_entry *)a;
    const wc_entry *eb = (const wc_entry *)b;

    if (ea->count > eb->count)
    {
        return -1;
    }
    if (ea->count < eb->count)
    {
        return 1;
    }
    return strcmp(ea->word, eb->word);
}

/*===========================================================================
 * Public API implementation
 *===========================================================================*/

wc_table *wc_create(const wc_config *cfg)
{
    unsigned hash_bits = WC_DEFAULT_HASH_BITS;
    size_t max_word = WC_DEFAULT_MAX_WORD;

    if (cfg != NULL)
    {
        if (cfg->initial_capacity > 0U)
        {
            const size_t default_cap = (size_t)1U << WC_DEFAULT_HASH_BITS;
            size_t requested = cfg->initial_capacity;

            if (requested < default_cap)
            {
                requested = default_cap;
            }

            unsigned bits = WC_DEFAULT_HASH_BITS;
            size_t size = default_cap;

            while (size < requested && bits < WC_MAX_HASH_BITS)
            {
                size <<= 1U;
                ++bits;
            }
            hash_bits = bits;
        }

        if (cfg->max_word_length > 0U)
        {
            max_word = wc_clamp_size(
                    cfg->max_word_length, WC_MIN_MAX_WORD, WC_MAX_MAX_WORD);
        }
    }

    wc_table *t = (wc_table *)malloc(sizeof(*t));
    if (t == NULL)
    {
        return NULL;
    }

    const size_t bucket_count = (size_t)1U << hash_bits;
    t->buckets = (wc_node **)calloc(bucket_count, sizeof(*t->buckets));
    if (t->buckets == NULL)
    {
        free(t);
        return NULL;
    }

    t->bucket_count = bucket_count;
    t->total_words = 0U;
    t->unique_words = 0U;
    t->max_word_len = max_word;

    return t;
}

void wc_destroy(wc_table *t)
{
    if (t == NULL)
    {
        return;
    }

    for (size_t i = 0U; i < t->bucket_count; ++i)
    {
        wc_node *node = t->buckets[i];
        while (node != NULL)
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
    if (t == NULL || word == NULL)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }

    size_t len = 0U;
    uint64_t hash = wc_fnv1a_str(word, &len);

    return wc_add_word_internal(t, word, len, hash);
}

wc_status wc_process_text(wc_table *t, const char *data, size_t len)
{
    if (t == NULL)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }
    if (len == 0U)
    {
        return WC_OK;
    }
    if (data == NULL)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }

    char *buf = (char *)malloc(t->max_word_len);
    if (buf == NULL)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    const char *pos = data;
    const char *end = data + len;

    wc_status status = WC_OK;

    while (pos < end)
    {
        wc_span span = wc_next_word(&pos, end);
        if (span.len == 0U)
        {
            break;
        }

        const size_t word_len = wc_normalize_word(span, buf, t->max_word_len);

        if (word_len == 0U)
        {
            continue;
        }

        const uint64_t hash = wc_fnv1a_buf(buf, word_len);
        status = wc_add_word_internal(t, buf, word_len, hash);
        if (status != WC_OK)
        {
            break;
        }
    }

    free(buf);
    return status;
}

size_t wc_total_words(const wc_table *t)
{
    return (t != NULL) ? t->total_words : 0U;
}

size_t wc_unique_words(const wc_table *t)
{
    return (t != NULL) ? t->unique_words : 0U;
}

wc_status
wc_snapshot(const wc_table *t, wc_entry **out_entries, size_t *out_len)
{
    if (t == NULL || out_entries == NULL || out_len == NULL)
    {
        return WC_ERR_INVALID_ARGUMENT;
    }

    if (t->unique_words == 0U)
    {
        *out_entries = NULL;
        *out_len = 0U;
        return WC_OK;
    }

    wc_entry *entries = (wc_entry *)malloc(t->unique_words * sizeof(*entries));
    if (entries == NULL)
    {
        return WC_ERR_OUT_OF_MEMORY;
    }

    size_t idx = 0U;

    for (size_t i = 0U; i < t->bucket_count; ++i)
    {
        wc_node *node = t->buckets[i];
        while (node != NULL)
        {
            entries[idx].word = node->word;
            entries[idx].count = node->count;
            ++idx;
            node = node->next;
        }
    }

    assert(idx == t->unique_words);

    qsort(entries, t->unique_words, sizeof(*entries), wc_compare_entries);

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
