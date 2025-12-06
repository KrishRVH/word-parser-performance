/*
** wordcount.c - Implementation
**
** Public domain.
*/

#include "wordcount.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/*
** Assertion configuration. Define WC_OMIT_ASSERT to disable.
*/
#ifndef WC_OMIT_ASSERT
#include <assert.h>
#define WC_ASSERT(x) assert(x)
#else
#define WC_ASSERT(x) ((void)0)
#endif

/*
** Compile-time verification of platform requirements.
*/
typedef char wc_check_ascii[('A' == 65 && 'Z' == 90 && 'a' == 97 &&
                             'z' == 122 && ('a' ^ 'A') == 32)
                                    ? 1
                                    : -1];
typedef char wc_check_char_bit[(CHAR_BIT == 8) ? 1 : -1];
typedef char wc_check_ptr_align[((sizeof(void *) & (sizeof(void *) - 1)) == 0)
                                        ? 1
                                        : -1];

typedef wc_check_ascii wc_used_1;
typedef wc_check_char_bit wc_used_2;
typedef wc_check_ptr_align wc_used_3;

/* --- Configuration defaults (platform-tuned) --- */
#define MAX_WORD 1024
#define MIN_WORD 4
#define DEF_WORD 64

/*
** FNV-1a constants.
*/
#define FNV_OFF 14695981039346656037ULL
#define FNV_MUL 1099511628211ULL

/*
** Portable alignment constant.
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
struct Block {
    Block *next;
    char *cur;
    char *end;
    char buf[];
};

typedef struct {
    Block *head;
    Block *tail;
    size_t block_sz;
} Arena;

/* --- Hash table slot --- */

typedef struct {
    char *word;
    unsigned long long hash;
    size_t cnt;
} Slot;

/* --- wc object --- */

struct wc {
    Slot *tab;
    size_t cap;
    size_t len;
    size_t tot;
    size_t maxw;
    Arena arena;
    size_t bytes_used;  /* internal allocations accounted */
    size_t bytes_limit; /* 0 = unlimited */
#if !WC_STACK_BUFFER
    char *scanbuf;
#endif
};

/* --- Internal helpers (forward declarations) --- */

static void *wc_xmalloc(wc *w, size_t n);
static void wc_xfree(wc *w, void *p, size_t n);

static Block *block_new(wc *w, size_t cap);
static int arena_init(wc *w, Arena *a, size_t block_sz);
static void arena_free(wc *w);
static void *arena_alloc(wc *w, size_t sz);

static unsigned long long fnv(const char *s, size_t n);
static int tab_grow(wc *w);
static Slot *tab_find(wc *w, const char *word, size_t n, unsigned long long h);
static int tab_insert(wc *w, const char *word, size_t n, unsigned long long h);

/* --- Allocation accounting --- */

static void *wc_xmalloc(wc *w, size_t n)
{
    void *p;

    if (n == 0)
        return NULL;

    if (add_overflows(w->bytes_used, n))
        return NULL;
    if (w->bytes_limit && w->bytes_used + n > w->bytes_limit)
        return NULL;

    p = WC_MALLOC(n);
    if (!p)
        return NULL;

    w->bytes_used += n;
    return p;
}

static void wc_xfree(wc *w, void *p, size_t n)
{
    if (!p)
        return;

    WC_FREE(p);

    if (w->bytes_used >= n)
        w->bytes_used -= n;
    else
        w->bytes_used = 0;
}

/* --- Arena implementation --- */

static Block *block_new(wc *w, size_t cap)
{
    Block *b;
    size_t total;

    if (add_overflows(sizeof(Block), cap))
        return NULL;
    total = sizeof(Block) + cap;

    b = (Block *)wc_xmalloc(w, total);
    if (!b)
        return NULL;
    b->next = NULL;
    b->cur = b->buf;
    b->end = b->buf + cap;
    return b;
}

static int arena_init(wc *w, Arena *a, size_t block_sz)
{
    Block *b;

    WC_ASSERT(a != NULL);
    a->head = a->tail = NULL;
    a->block_sz = block_sz;

    b = block_new(w, block_sz);
    if (!b)
        return -1;

    a->head = a->tail = b;
    return 0;
}

static void arena_free(wc *w)
{
    Block *b;

    WC_ASSERT(w != NULL);
    b = w->arena.head;
    while (b) {
        Block *n = b->next;
        size_t cap = (size_t)(b->end - b->buf);
        size_t total = sizeof(Block) + cap;
        wc_xfree(w, b, total);
        b = n;
    }
    w->arena.head = w->arena.tail = NULL;
}

/*
** Portable alignment calculation without uintptr_t.
*/
static void *arena_alloc(wc *w, size_t sz)
{
    size_t offset;
    size_t align = WC_ALIGN;
    size_t pad;
    size_t avail;
    size_t cap;
    size_t need;
    char *p;
    Block *b;
    Arena *a;

    WC_ASSERT(w != NULL);
    a = &w->arena;
    WC_ASSERT(a->tail != NULL);
    WC_ASSERT(a->tail->cur >= a->tail->buf);
    WC_ASSERT(a->tail->cur <= a->tail->end);

    offset = (size_t)(a->tail->cur - a->tail->buf);
    pad = (align - (offset % align)) % align;

    avail = (size_t)(a->tail->end - a->tail->cur);
    if (avail >= pad && avail - pad >= sz) {
        p = a->tail->cur + pad;
        a->tail->cur = p + sz;
        WC_ASSERT(a->tail->cur <= a->tail->end);
        return memset(p, 0, sz);
    }

    if (add_overflows(sz, align))
        return NULL;
    need = sz + align;
    cap = need > a->block_sz ? need : a->block_sz;

    b = block_new(w, cap);
    if (!b)
        return NULL;
    a->tail->next = b;
    a->tail = b;

    offset = 0;
    pad = (align - (offset % align)) % align;
    p = b->cur + pad;
    b->cur = p + sz;
    WC_ASSERT(b->cur <= b->end);
    return memset(p, 0, sz);
}

/* --- Hash table implementation --- */

static unsigned long long fnv(const char *s, size_t n)
{
    unsigned long long h = FNV_OFF;
    size_t i;
    for (i = 0; i < n; i++) {
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
    size_t old_cap;
    size_t old_alloc;
    Slot *old_tab;

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);

    if (mul_overflows(w->cap, 2))
        return -1;
    nc = w->cap * 2;

    if (mul_overflows(nc, sizeof(Slot)))
        return -1;
    alloc = nc * sizeof(Slot);

    ns = (Slot *)wc_xmalloc(w, alloc);
    if (!ns)
        return -1;
    memset(ns, 0, alloc);

    for (i = 0; i < w->cap; i++) {
        const Slot *s = &w->tab[i];
        if (!s->word)
            continue;
        idx = (size_t)(s->hash & (nc - 1));
        while (ns[idx].word)
            idx = (idx + 1) & (nc - 1);
        ns[idx] = *s;
    }

    old_tab = w->tab;
    old_cap = w->cap;
    old_alloc = old_cap * sizeof(Slot);

    w->tab = ns;
    w->cap = nc;

    wc_xfree(w, old_tab, old_alloc);
    return 0;
}

static Slot *tab_find(wc *w, const char *word, size_t n, unsigned long long h)
{
    size_t idx;
    size_t start;

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);
    WC_ASSERT(word != NULL || n == 0);

    idx = (size_t)(h & (w->cap - 1));
    start = idx;

    do {
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

    if (w->len * 10 >= w->cap * 7) {
        if (tab_grow(w) < 0)
            return -1;
    }

    s = tab_find(w, word, n, h);
    if (!s)
        return -1;

    if (s->word) {
        s->cnt++;
        w->tot++;
        return 0;
    }

    if (add_overflows(n, 1))
        return -1;
    alloc = n + 1;

    copy = (char *)arena_alloc(w, alloc);
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

/* --- Parameter tuning based on limits --- */

static void
tune_params(const wc_limits *lim, size_t *init_cap, size_t *block_sz)
{
    size_t cap = WC_DEFAULT_INIT_CAP;
    size_t blk = WC_DEFAULT_BLOCK_SZ;

    if (lim) {
        if (lim->init_cap)
            cap = lim->init_cap;
        if (lim->block_size)
            blk = lim->block_size;

        if (lim->max_bytes) {
            size_t b = lim->max_bytes;
            size_t table_budget = b / 2;
            size_t arena_budget = b - table_budget;

            if (!mul_overflows(cap, sizeof(Slot)) &&
                cap * sizeof(Slot) > table_budget && table_budget > 0) {
                size_t max_cap = table_budget / sizeof(Slot);
                if (max_cap < 16)
                    max_cap = 16;
                /* round down to power of two */
                {
                    size_t p = 1;
                    while ((p << 1) <= max_cap)
                        p <<= 1;
                    cap = p;
                }
            }

            size_t max_blk = arena_budget / 4;
            if (max_blk >= 256 && blk > max_blk)
                blk = max_blk;
        }
    }

    if (cap < 16)
        cap = 16;

    /* ensure power of two */
    {
        size_t p = 1;
        while (p < cap && p <= (SIZE_MAX / 2))
            p <<= 1;
        cap = p;
    }

    if (blk < 256)
        blk = 256;

    *init_cap = cap;
    *block_sz = blk;
}

/* --- Public API --- */

wc *wc_open_ex(size_t max_word, const wc_limits *limits)
{
    wc *w;
    size_t init_cap;
    size_t block_sz;
    size_t table_bytes;

    tune_params(limits, &init_cap, &block_sz);

    w = (wc *)WC_MALLOC(sizeof *w);
    if (!w)
        return NULL;
    memset(w, 0, sizeof *w);

    /* Set overall memory budget (0 = unlimited) */
    w->bytes_limit = (limits && limits->max_bytes) ? limits->max_bytes : 0;
    w->bytes_used = 0;

    /* Clamp max_word into [MIN_WORD, MAX_WORD] */
    if (max_word == 0)
        max_word = DEF_WORD;
    if (max_word < MIN_WORD)
        max_word = MIN_WORD;
    if (max_word > MAX_WORD)
        max_word = MAX_WORD;
    w->maxw = max_word;

    /* Allocate initial hash table */
    if (mul_overflows(init_cap, sizeof(Slot))) {
        WC_FREE(w);
        return NULL;
    }
    table_bytes = init_cap * sizeof(Slot);

    w->tab = (Slot *)wc_xmalloc(w, table_bytes);
    if (!w->tab) {
        WC_FREE(w);
        return NULL;
    }
    memset(w->tab, 0, table_bytes);
    w->cap = init_cap;

    /* Initialize arena */
    if (arena_init(w, &w->arena, block_sz) < 0) {
        wc_xfree(w, w->tab, table_bytes);
        WC_FREE(w);
        return NULL;
    }

#if !WC_STACK_BUFFER
    /* Optional heap-based scan buffer */
    w->scanbuf = (char *)wc_xmalloc(w, w->maxw);
    if (!w->scanbuf) {
        arena_free(w);
        wc_xfree(w, w->tab, table_bytes);
        WC_FREE(w);
        return NULL;
    }
#endif

    return w;
}

wc *wc_open(size_t max_word)
{
    return wc_open_ex(max_word, NULL);
}

void wc_close(wc *w)
{
    size_t table_bytes;

    if (!w)
        return;

#if !WC_STACK_BUFFER
    if (w->scanbuf)
        wc_xfree(w, w->scanbuf, w->maxw);
#endif

    if (w->tab && w->cap) {
        table_bytes = w->cap * sizeof(Slot);
        wc_xfree(w, w->tab, table_bytes);
    }

    arena_free(w);
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

    while (p < end) {
        size_t n;
        unsigned long long h;

        while (p < end && !isalpha_(*p))
            p++;
        if (p >= end)
            break;

        h = FNV_OFF;
        n = 0;
        while (p < end && isalpha_(*p)) {
            unsigned c = *p++ | 32;
            if (n < w->maxw) {
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
    const wc_word *x = (const wc_word *)a;
    const wc_word *y = (const wc_word *)b;
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
    if (w->len == 0) {
        *out = NULL;
        *n = 0;
        return WC_OK;
    }

    if (mul_overflows(w->len, sizeof *arr))
        return WC_NOMEM;
    alloc = w->len * sizeof *arr;

    /*
    ** Results buffer is allocated via WC_MALLOC without being
    ** accounted against bytes_limit, since its lifetime is under
    ** the caller's control.
    */
    arr = (wc_word *)WC_MALLOC(alloc);
    if (!arr)
        return WC_NOMEM;

    cnt = 0;
    for (i = 0; i < w->cap; i++) {
        if (w->tab[i].word)
            cnt++;
    }
    if (cnt != w->len) {
        WC_FREE(arr);
        return WC_ERROR;
    }

    for (i = 0, j = 0; i < w->cap; i++) {
        if (w->tab[i].word) {
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
    switch (rc) {
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
