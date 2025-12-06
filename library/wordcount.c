/*
** wordcount.c - Implementation
**
** Public domain.
**
** DESIGN
**
**   Hash table with open addressing and linear probing. Load factor
**   kept below 70%. Deletion not supported.
**
**   Arena allocator uses block chains: when a block fills, allocate
**   a new block and link it. Old blocks never move, so pointers into
**   the arena remain valid for its lifetime.
**
**   Both wc_add() and wc_scan() truncate at max_word and hash only
**   stored characters. They differ only in case handling.
**
**   Memory allocation is routed through WC_MALLOC/WC_FREE macros
**   to allow embedding applications to redirect to custom allocators.
**
** PORTABILITY
**
**   Uses only C99-guaranteed types. The hash uses unsigned long long
**   (guaranteed 64+ bits) rather than optional uint64_t. Alignment
**   calculations use a portable method that doesn't require uintptr_t.
**
**   Assumes ASCII-compatible character encoding for letter detection.
**   Verified at compile time along with other platform requirements.
*/

#include "wordcount.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/*
** Assertion configuration. Define WC_OMIT_ASSERT to disable.
** In debug builds, assertions catch internal invariant violations.
*/
#ifndef WC_OMIT_ASSERT
#include <assert.h>
#define WC_ASSERT(x) assert(x)
#else
#define WC_ASSERT(x) ((void)0)
#endif

/*
** Compile-time verification of platform requirements.
**
** 1. ASCII-compatible character set: Letters must have expected values
**    and the uppercase/lowercase bit difference must be 0x20.
**
** 2. 8-bit bytes: CHAR_BIT must be 8. This catches DSPs and other
**    exotic platforms where chars are 16 or 32 bits.
**
** 3. Pointer size is power of two: Required for efficient alignment.
**    True on all known architectures.
**
** These checks cause a compile-time error on non-conforming platforms
** rather than silent misbehavior at runtime.
*/
typedef char wc_check_ascii[('A' == 65 && 'Z' == 90 && 'a' == 97 &&
                             'z' == 122 && ('a' ^ 'A') == 32)
                                    ? 1
                                    : -1];
typedef char wc_check_char_bit[(CHAR_BIT == 8) ? 1 : -1];
typedef char wc_check_ptr_align[((sizeof(void *) & (sizeof(void *) - 1)) == 0)
                                        ? 1
                                        : -1];

/* Suppress unused typedef warnings */
typedef wc_check_ascii wc_used_1;
typedef wc_check_char_bit wc_used_2;
typedef wc_check_ptr_align wc_used_3;

/* --- Configuration --- */

#define INIT_CAP 4096
#define BLOCK_SZ 65536
#define MAX_WORD 1024
#define MIN_WORD 4
#define DEF_WORD 64

/*
** FNV-1a constants. Using unsigned long long which C99 guarantees
** is at least 64 bits, avoiding dependency on optional uint64_t.
*/
#define FNV_OFF 14695981039346656037ULL
#define FNV_MUL 1099511628211ULL

/*
** Portable alignment constant. sizeof(void*) works on all hosted
** implementations and is always a power of two in practice.
*/
#define WC_ALIGN sizeof(void *)

/* --- Overflow-safe arithmetic --- */

static int add_overflows(size_t a, size_t b)
{
    return a > SIZE_MAX - b;
}

static int mul_overflows(size_t a, size_t b)
{
    return b != 0 && a > SIZE_MAX / b;
}

/* --- Arena allocator --- */

typedef struct Block Block;
struct Block
{
    Block *next;
    char *cur;
    char *end;
    char buf[];
};

typedef struct
{
    Block *head;
    Block *tail;
} Arena;

static Block *block_new(size_t cap)
{
    Block *b;
    size_t total;

    if (add_overflows(sizeof(Block), cap))
        return NULL;
    total = sizeof(Block) + cap;

    b = WC_MALLOC(total);
    if (!b)
        return NULL;
    b->next = NULL;
    b->cur = b->buf;
    b->end = b->buf + cap;
    return b;
}

static int arena_init(Arena *a)
{
    WC_ASSERT(a != NULL);
    a->head = a->tail = block_new(BLOCK_SZ);
    return a->head ? 0 : -1;
}

static void arena_free(Arena *a)
{
    Block *b;

    WC_ASSERT(a != NULL);
    b = a->head;
    while (b)
    {
        Block *n = b->next;
        WC_FREE(b);
        b = n;
    }
    a->head = a->tail = NULL;
}

/*
** Portable alignment calculation without uintptr_t.
**
** Rather than casting pointers to integers (which requires the
** optional uintptr_t type), we compute padding by examining the
** offset from the block start. Since block->buf is allocated via
** malloc, it's suitably aligned for any type. We track position
** as an offset from buf and align that offset.
*/
static void *arena_alloc(Arena *a, size_t sz)
{
    size_t offset;
    size_t align = WC_ALIGN;
    size_t pad;
    size_t avail;
    size_t cap;
    size_t need;
    char *p;
    Block *b;

    WC_ASSERT(a != NULL);
    WC_ASSERT(a->tail != NULL);
    WC_ASSERT(a->tail->cur >= a->tail->buf);
    WC_ASSERT(a->tail->cur <= a->tail->end);

    /*
    ** Compute current offset from block start, then padding needed.
    ** This works because (offset % align) gives misalignment, and
    ** (align - misalignment) % align gives required padding.
    */
    offset = (size_t)(a->tail->cur - a->tail->buf);
    pad = (align - (offset % align)) % align;

    /* Check available space (safe: cur <= end always) */
    avail = (size_t)(a->tail->end - a->tail->cur);
    if (avail >= pad && avail - pad >= sz)
    {
        p = a->tail->cur + pad;
        a->tail->cur = p + sz;
        WC_ASSERT(a->tail->cur <= a->tail->end);
        return memset(p, 0, sz);
    }

    /* Need new block - overflow-safe size calculation */
    if (add_overflows(sz, align))
        return NULL;
    need = sz + align;
    cap = need > BLOCK_SZ ? need : BLOCK_SZ;

    b = block_new(cap);
    if (!b)
        return NULL;
    a->tail->next = b;
    a->tail = b;

    /*
    ** Fresh block from malloc is already aligned, but we still
    ** compute padding for consistency and future-proofing.
    */
    offset = 0;
    pad = (align - (offset % align)) % align;
    p = b->cur + pad;
    b->cur = p + sz;
    WC_ASSERT(b->cur <= b->end);
    return memset(p, 0, sz);
}

/* --- Hash table --- */

typedef struct
{
    char *word;
    unsigned long long hash;
    size_t cnt;
} Slot;

struct wc
{
    Slot *tab;
    size_t cap;
    size_t len;
    size_t tot;
    size_t maxw;
    Arena arena;
#if !WC_STACK_BUFFER
    char *scanbuf;
#endif
};

static unsigned long long fnv(const char *s, size_t n)
{
    unsigned long long h = FNV_OFF;
    size_t i;
    for (i = 0; i < n; i++)
    {
        h ^= (unsigned char)s[i];
        h *= FNV_MUL;
    }
    return h;
}

static int tab_grow(wc *w)
{
    size_t nc;
    size_t i;
    size_t idx;
    size_t alloc;
    Slot *ns;

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);

    if (mul_overflows(w->cap, 2))
        return -1;
    nc = w->cap * 2;

    if (mul_overflows(nc, sizeof(Slot)))
        return -1;
    alloc = nc * sizeof(Slot);

    ns = WC_MALLOC(alloc);
    if (!ns)
        return -1;
    memset(ns, 0, alloc);

    for (i = 0; i < w->cap; i++)
    {
        const Slot *s = &w->tab[i];
        if (!s->word)
            continue;
        idx = (size_t)(s->hash & (nc - 1));
        while (ns[idx].word)
            idx = (idx + 1) & (nc - 1);
        ns[idx] = *s;
    }

    WC_FREE(w->tab);
    w->tab = ns;
    w->cap = nc;
    return 0;
}

static Slot *
tab_find(const wc *w, const char *word, size_t n, unsigned long long h)
{
    size_t idx;
    size_t start;

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);
    WC_ASSERT(word != NULL || n == 0);

    idx = (size_t)(h & (w->cap - 1));
    start = idx;

    do
    {
        Slot *s = &w->tab[idx];
        if (!s->word)
            return s;
        if (s->hash == h && memcmp(s->word, word, n) == 0 && s->word[n] == '\0')
            return s;
        idx = (idx + 1) & (w->cap - 1);
    } while (idx != start);

    return NULL;
}

static int tab_insert(wc *w, const char *word, size_t n, unsigned long long h)
{
    Slot *s;
    char *copy;
    size_t alloc;

    WC_ASSERT(w != NULL);
    WC_ASSERT(word != NULL);
    WC_ASSERT(n > 0);

    if (w->len * 10 >= w->cap * 7)
    {
        if (tab_grow(w) < 0)
            return -1;
    }

    s = tab_find(w, word, n, h);
    if (!s)
        return -1;

    if (s->word)
    {
        s->cnt++;
        w->tot++;
        return 0;
    }

    if (add_overflows(n, 1))
        return -1;
    alloc = n + 1;

    copy = arena_alloc(&w->arena, alloc);
    if (!copy)
        return -1;
    memcpy(copy, word, n);

    s->word = copy;
    s->hash = h;
    s->cnt = 1;
    w->len++;
    w->tot++;
    return 0;
}

/* --- Public API --- */

wc *wc_open(size_t max_word)
{
    wc *w;
    size_t alloc;

    w = WC_MALLOC(sizeof *w);
    if (!w)
        return NULL;
    memset(w, 0, sizeof *w);

    if (max_word == 0)
        max_word = DEF_WORD;
    if (max_word < MIN_WORD)
        max_word = MIN_WORD;
    if (max_word > MAX_WORD)
        max_word = MAX_WORD;
    w->maxw = max_word;

    if (mul_overflows(INIT_CAP, sizeof(Slot)))
    {
        WC_FREE(w);
        return NULL;
    }
    alloc = INIT_CAP * sizeof(Slot);

    w->tab = WC_MALLOC(alloc);
    if (!w->tab)
    {
        WC_FREE(w);
        return NULL;
    }
    memset(w->tab, 0, alloc);
    w->cap = INIT_CAP;

    if (arena_init(&w->arena) < 0)
    {
        WC_FREE(w->tab);
        WC_FREE(w);
        return NULL;
    }

#if !WC_STACK_BUFFER
    w->scanbuf = WC_MALLOC(max_word);
    if (!w->scanbuf)
    {
        arena_free(&w->arena);
        WC_FREE(w->tab);
        WC_FREE(w);
        return NULL;
    }
#endif

    return w;
}

void wc_close(wc *w)
{
    if (!w)
        return;
#if !WC_STACK_BUFFER
    WC_FREE(w->scanbuf);
#endif
    arena_free(&w->arena);
    WC_FREE(w->tab);
    WC_FREE(w);
}

int wc_add(wc *restrict w, const char *restrict word)
{
    size_t n;
    unsigned long long h;

    if (!w || !word)
        return WC_ERROR;

    for (n = 0; n < w->maxw && word[n]; n++)
        ;
    if (n == 0)
        return WC_OK;

    h = fnv(word, n);
    return tab_insert(w, word, n, h) < 0 ? WC_NOMEM : WC_OK;
}

/*
** ASCII-only letter check. Non-ASCII bytes (including UTF-8) are
** treated as word separators. The bit-twiddling works because in
** ASCII, lowercase letters differ from uppercase by exactly bit 5.
*/
static int isalpha_(int c)
{
    return ((unsigned)c | 32) - 'a' < 26;
}

int wc_scan(wc *restrict w, const char *restrict text, size_t len)
{
    const unsigned char *p;
    const unsigned char *end;
#if WC_STACK_BUFFER
    char buf[MAX_WORD];
#else
    char *buf;
#endif

    if (!w)
        return WC_ERROR;
    if (len == 0)
        return WC_OK;
    if (!text)
        return WC_ERROR;

#if !WC_STACK_BUFFER
    buf = w->scanbuf;
    WC_ASSERT(buf != NULL);
#endif

    p = (const unsigned char *)text;
    end = p + len;

    while (p < end)
    {
        size_t n;
        unsigned long long h;

        while (p < end && !isalpha_(*p))
            p++;
        if (p >= end)
            break;

        h = FNV_OFF;
        n = 0;
        while (p < end && isalpha_(*p))
        {
            unsigned c = *p++ | 32;
            if (n < w->maxw)
            {
                buf[n++] = (char)c;
                h ^= c;
                h *= FNV_MUL;
            }
        }

        WC_ASSERT(n > 0);
        WC_ASSERT(n <= w->maxw);
        if (tab_insert(w, buf, n, h) < 0)
            return WC_NOMEM;
    }

    return WC_OK;
}

size_t wc_total(const wc *w)
{
    return w ? w->tot : 0;
}

size_t wc_unique(const wc *w)
{
    return w ? w->len : 0;
}

static int cmp(const void *a, const void *b)
{
    const wc_word *x = a;
    const wc_word *y = b;
    if (x->count != y->count)
        return x->count > y->count ? -1 : 1;
    return strcmp(x->word, y->word);
}

int wc_results(const wc *restrict w, wc_word **restrict out, size_t *restrict n)
{
    wc_word *arr;
    size_t i;
    size_t j;
    size_t cnt;
    size_t alloc;

    if (!w || !out || !n)
        return WC_ERROR;
    if (w->len == 0)
    {
        *out = NULL;
        *n = 0;
        return WC_OK;
    }

    if (mul_overflows(w->len, sizeof *arr))
        return WC_NOMEM;
    alloc = w->len * sizeof *arr;

    arr = WC_MALLOC(alloc);
    if (!arr)
        return WC_NOMEM;

    cnt = 0;
    for (i = 0; i < w->cap; i++)
    {
        if (w->tab[i].word)
            cnt++;
    }
    if (cnt != w->len)
    {
        WC_FREE(arr);
        return WC_ERROR;
    }

    for (i = 0, j = 0; i < w->cap; i++)
    {
        if (w->tab[i].word)
        {
            arr[j].word = w->tab[i].word;
            arr[j].count = w->tab[i].cnt;
            j++;
        }
    }
    WC_ASSERT(j == w->len);

    qsort(arr, w->len, sizeof *arr, cmp);
    *out = arr;
    *n = w->len;
    return WC_OK;
}

void wc_results_free(wc_word *r)
{
    WC_FREE(r);
}

const char *wc_errstr(int rc)
{
    switch (rc)
    {
        case WC_OK:
            return "success";
        case WC_ERROR:
            return "invalid argument or corrupted state";
        case WC_NOMEM:
            return "memory allocation failed";
        default:
            return "unknown error";
    }
}

const char *wc_version(void)
{
    return WC_VERSION;
}
