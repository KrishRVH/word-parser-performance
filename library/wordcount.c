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
*/

#include <stdlib.h> /* for default WC_MALLOC if not overridden */
#include "wordcount.h"
#include <stdint.h>
#include <string.h>

/* --- Configuration --- */

#define INIT_CAP 4096
#define BLOCK_SZ 65536
#define MAX_WORD 1024
#define MIN_WORD 4
#define DEF_WORD 64

/* FNV-1a 64-bit constants */
#define FNV_OFF 14695981039346656037ULL
#define FNV_MUL 1099511628211ULL

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
    a->head = a->tail = block_new(BLOCK_SZ);
    return a->head ? 0 : -1;
}

static void arena_free(Arena *a)
{
    Block *b = a->head;
    while (b)
    {
        Block *n = b->next;
        WC_FREE(b);
        b = n;
    }
    a->head = a->tail = NULL;
}

static void *arena_alloc(Arena *a, size_t sz)
{
    size_t align = sizeof(void *);
    size_t pad;
    size_t avail;
    size_t cap;
    size_t need;
    char *p;
    Block *b;

    /*
    ** Defensive: verify alignment is power of two. This is true on
    ** all practical platforms, but we degrade gracefully if not.
    */
    /* cppcheck-suppress knownConditionTrueFalse */
    if ((align & (align - 1)) != 0)
    {
        align = 1;
    }

    /*
    ** Compute padding needed for alignment. Use uintptr_t (not size_t)
    ** for pointer-to-integer cast per C99 7.18.1.4 - size_t is not
    ** guaranteed to hold a pointer on segmented architectures.
    */
    pad = ((uintptr_t) - (uintptr_t)a->tail->cur) & (align - 1);

    /* Check available space (safe: cur <= end always) */
    avail = (size_t)(a->tail->end - a->tail->cur);
    if (avail >= pad && avail - pad >= sz)
    {
        p = a->tail->cur + pad;
        a->tail->cur = p + sz;
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

    pad = ((uintptr_t) - (uintptr_t)b->cur) & (align - 1);
    p = b->cur + pad;
    b->cur = p + sz;
    return memset(p, 0, sz);
}
/* --- Hash table --- */

typedef struct
{
    char *word;
    uint64_t hash;
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
};

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = FNV_OFF;
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

static Slot *tab_find(const wc *w, const char *word, size_t n, uint64_t h)
{
    size_t idx = (size_t)(h & (w->cap - 1));
    size_t start = idx;

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

static int tab_insert(wc *w, const char *word, size_t n, uint64_t h)
{
    Slot *s;
    char *copy;
    size_t alloc;

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

    return w;
}

void wc_close(wc *w)
{
    if (!w)
        return;
    arena_free(&w->arena);
    WC_FREE(w->tab);
    WC_FREE(w);
}

int wc_add(wc *w, const char *word)
{
    size_t n;
    uint64_t h;

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
** treated as word separators.
*/
static int isalpha_(int c)
{
    return ((unsigned)c | 32) - 'a' < 26;
}

int wc_scan(wc *w, const char *text, size_t len)
{
    const unsigned char *p;
    const unsigned char *end;
    char buf[MAX_WORD];

    if (!w)
        return WC_ERROR;
    if (len == 0)
        return WC_OK;
    if (!text)
        return WC_ERROR;

    p = (const unsigned char *)text;
    end = p + len;

    while (p < end)
    {
        size_t n;
        uint64_t h;

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

int wc_results(const wc *w, wc_word **out, size_t *n)
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

    qsort(arr, w->len, sizeof *arr, cmp);
    *out = arr;
    *n = w->len;
    return WC_OK;
}

void wc_results_free(wc_word *r)
{
    WC_FREE(r);
}

const char *wc_version(void)
{
    return WC_VERSION;
}
