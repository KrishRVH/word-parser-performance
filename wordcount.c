/*
 * wordcount.c — Parallel word frequency counter
 *
 * Build:  cc -std=c11 -O2 -pthread wordcount.c -o wordcount
 * Usage:  ./wordcount <file>
 *
 * Design: Memory-mapped I/O, per-thread hash tables with arena-allocated
 * strings, embarrassingly parallel (no shared mutable state in hot path).
 */

#if defined(_WIN32)
#define PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#else
#define _POSIX_C_SOURCE 200809L
#define PLATFORM_POSIX
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════*/

enum {
    MAX_THREADS = 32,
    INITIAL_CAP = 1 << 12,
    MAX_WORD_LEN = 63,
    TOP_N = 10,
};

_Static_assert((INITIAL_CAP & (INITIAL_CAP - 1)) == 0, "must be power of 2");

/* ═══════════════════════════════════════════════════════════════════════════
 * Platform: Memory-Mapped Files
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    const char *data;
    size_t size;
#ifdef PLATFORM_WINDOWS
    HANDLE file, mapping;
#else
    int fd;
#endif
} MappedFile;

static int mf_open(MappedFile *mf, const char *path)
{
    *mf = (MappedFile){ 0 };

#ifdef PLATFORM_WINDOWS
    mf->file = CreateFileA(path,
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (mf->file == INVALID_HANDLE_VALUE)
        return -1;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(mf->file, &li) || li.QuadPart == 0)
        goto fail;
    mf->size = (size_t)li.QuadPart;

    mf->mapping = CreateFileMappingA(mf->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mf->mapping)
        goto fail;

    mf->data = MapViewOfFile(mf->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!mf->data) {
        CloseHandle(mf->mapping);
        mf->mapping = NULL;
        goto fail;
    }
    return 0;

fail:
    CloseHandle(mf->file);
    mf->file = NULL;
    return -1;

#else
    mf->fd = open(path, O_RDONLY);
    if (mf->fd < 0)
        return -1;

    struct stat st;
    if (fstat(mf->fd, &st) < 0 || st.st_size == 0) {
        close(mf->fd);
        mf->fd = -1;
        return -1;
    }
    mf->size = (size_t)st.st_size;

    mf->data = mmap(NULL, mf->size, PROT_READ, MAP_PRIVATE, mf->fd, 0);
    if (mf->data == MAP_FAILED) {
        mf->data = NULL;
        close(mf->fd);
        mf->fd = -1;
        return -1;
    }

#ifdef POSIX_MADV_SEQUENTIAL
    posix_madvise((void *)mf->data, mf->size, POSIX_MADV_SEQUENTIAL);
#endif
    return 0;
#endif
}

static void mf_close(MappedFile *mf)
{
#ifdef PLATFORM_WINDOWS
    if (mf->data)
        UnmapViewOfFile(mf->data);
    if (mf->mapping)
        CloseHandle(mf->mapping);
    if (mf->file && mf->file != INVALID_HANDLE_VALUE)
        CloseHandle(mf->file);
#else
    if (mf->data)
        munmap((void *)mf->data, mf->size);
    if (mf->fd > 0)
        close(mf->fd);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Platform: Threading
 * ═══════════════════════════════════════════════════════════════════════════*/

#ifdef PLATFORM_WINDOWS
static HANDLE g_threads[MAX_THREADS];
#else
static pthread_t g_threads[MAX_THREADS];
#endif
static int g_nthreads;

static int ncpu(void)
{
#ifdef PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Arena Allocator
 *
 * Bump allocator for string storage. No individual frees—entire arena
 * released at once. Alignment handled via pointer arithmetic.
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    char *buf, *ptr, *end;
} Arena;

static int arena_init(Arena *a, size_t cap)
{
    a->buf = malloc(cap);
    if (!a->buf)
        return -1;
    a->ptr = a->buf;
    a->end = a->buf + cap;
    return 0;
}

static void arena_free(Arena *a)
{
    free(a->buf);
    a->buf = NULL;
}

static void *arena_alloc(Arena *a, size_t size, size_t align)
{
    uintptr_t p = (uintptr_t)a->ptr;
    uintptr_t aligned = (p + align - 1) & ~(align - 1);
    if (aligned + size > (uintptr_t)a->end)
        return NULL;
    a->ptr = (char *)(aligned + size);
    return (void *)aligned;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hash Table
 *
 * Open addressing with linear probing. FNV-1a hash computed incrementally
 * during tokenization. Power-of-2 capacity for fast modulo via bitmask.
 * ═══════════════════════════════════════════════════════════════════════════*/

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

typedef struct {
    char *key;
    size_t count;
    uint64_t hash;
} Entry;

typedef struct {
    Entry *entries;
    size_t cap, len, total;
    Arena strings;
} Table;

static int table_init(Table *t, size_t cap, size_t arena_cap)
{
    t->entries = calloc(cap, sizeof(Entry));
    if (!t->entries)
        return -1;
    if (arena_init(&t->strings, arena_cap) < 0) {
        free(t->entries);
        t->entries = NULL;
        return -1;
    }
    t->cap = cap;
    t->len = t->total = 0;
    return 0;
}

static void table_free(Table *t)
{
    if (!t)
        return;
    free(t->entries);
    t->entries = NULL;
    arena_free(&t->strings);
}

static int table_grow(Table *t)
{
    size_t new_cap = t->cap * 2;
    Entry *new_ent = calloc(new_cap, sizeof(Entry));
    if (!new_ent)
        return -1;

    for (size_t i = 0; i < t->cap; i++) {
        const Entry *e = &t->entries[i];
        if (!e->key)
            continue;
        size_t idx = e->hash & (new_cap - 1);
        while (new_ent[idx].key)
            idx = (idx + 1) & (new_cap - 1);
        new_ent[idx] = *e;
    }
    free(t->entries);
    t->entries = new_ent;
    t->cap = new_cap;
    return 0;
}

static int table_add(Table *t, const char *word, size_t len, uint64_t hash)
{
    if (t->len * 10 >= t->cap * 7 && table_grow(t) < 0)
        return -1;

    size_t idx = hash & (t->cap - 1);
    for (;;) {
        Entry *e = &t->entries[idx];
        if (!e->key) {
            char *s = arena_alloc(&t->strings, len + 1, 1);
            if (!s)
                return -1;
            memcpy(s, word, len);
            s[len] = '\0';
            *e = (Entry){ .key = s, .count = 1, .hash = hash };
            t->len++;
            t->total++;
            return 0;
        }
        if (e->hash == hash && memcmp(e->key, word, len) == 0 && !e->key[len]) {
            e->count++;
            t->total++;
            return 0;
        }
        idx = (idx + 1) & (t->cap - 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tokenizer
 *
 * Extracts words, lowercases, computes FNV-1a hash in single pass.
 * Branchless is_alpha avoids table lookups.
 * ═══════════════════════════════════════════════════════════════════════════*/

static inline bool is_alpha(unsigned c)
{
    return ((c | 32) - 'a') < 26u;
}

static int tokenize(Table *t, const char *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    const unsigned char *end = p + len;
    char word[MAX_WORD_LEN + 1];

    while (p < end) {
        while (p < end && !is_alpha(*p))
            p++;
        if (p >= end)
            break;

        uint64_t hash = FNV_OFFSET;
        size_t wlen = 0;

        while (p < end && is_alpha(*p)) {
            unsigned c = *p++ | 32;
            hash = (hash ^ c) * FNV_PRIME;
            if (wlen < MAX_WORD_LEN)
                word[wlen++] = (char)c;
        }
        if (table_add(t, word, wlen, hash) < 0)
            return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Worker Threads
 * ═══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    const char *data;
    size_t len;
    Table table;
    int err;
} Worker;

static void process_chunk(Worker *w)
{
    w->err = tokenize(&w->table, w->data, w->len);
}

#ifdef PLATFORM_WINDOWS
static unsigned __stdcall worker_entry(void *arg)
{
    process_chunk(arg);
    return 0;
}
#else
static void *worker_entry(void *arg)
{
    process_chunk(arg);
    return NULL;
}
#endif

static int spawn_workers(Worker *w, int n)
{
    for (int i = 0; i < n; i++) {
#ifdef PLATFORM_WINDOWS
        g_threads[i] =
                (HANDLE)_beginthreadex(NULL, 0, worker_entry, &w[i], 0, NULL);
        if (!g_threads[i])
            return -1;
#else
        if (pthread_create(&g_threads[i], NULL, worker_entry, &w[i]))
            return -1;
#endif
    }
    g_nthreads = n;
    return 0;
}

static void join_workers(void)
{
#ifdef PLATFORM_WINDOWS
    WaitForMultipleObjects(g_nthreads, g_threads, TRUE, INFINITE);
    for (int i = 0; i < g_nthreads; i++)
        CloseHandle(g_threads[i]);
#else
    for (int i = 0; i < g_nthreads; i++)
        pthread_join(g_threads[i], NULL);
#endif
    g_nthreads = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Merge & Output
 * ═══════════════════════════════════════════════════════════════════════════*/

static int merge_into(Table *dst, Worker *workers, int n)
{
    for (int w = 0; w < n; w++) {
        Table *src = &workers[w].table;
        for (size_t i = 0; i < src->cap; i++) {
            const Entry *e = &src->entries[i];
            if (!e->key)
                continue;

            if (dst->len * 10 >= dst->cap * 7 && table_grow(dst) < 0)
                return -1;

            size_t idx = e->hash & (dst->cap - 1);
            for (;;) {
                Entry *d = &dst->entries[idx];
                if (!d->key) {
                    *d = *e;
                    dst->len++;
                    dst->total += e->count;
                    break;
                }
                if (d->hash == e->hash && strcmp(d->key, e->key) == 0) {
                    d->count += e->count;
                    dst->total += e->count;
                    break;
                }
                idx = (idx + 1) & (dst->cap - 1);
            }
        }
    }
    return 0;
}

static int cmp_count_desc(const void *a, const void *b)
{
    const Entry *ea = a;
    const Entry *eb = b;
    if (ea->count != eb->count)
        return ea->count < eb->count ? 1 : -1;
    return strcmp(ea->key, eb->key);
}

static void print_top(const Table *t, size_t n)
{
    if (t->len == 0)
        return;

    Entry *sorted = malloc(t->len * sizeof(Entry));
    if (!sorted)
        return;

    size_t j = 0;
    for (size_t i = 0; i < t->cap && j < t->len; i++)
        if (t->entries[i].key)
            sorted[j++] = t->entries[i];

    qsort(sorted, j, sizeof(Entry), cmp_count_desc);

    printf("\n%-4s  %-20s  %10s  %6s\n", "Rank", "Word", "Count", "%");
    printf("────  ────────────────────  ──────────  ──────\n");

    size_t top = n < j ? n : j;
    for (size_t i = 0; i < top; i++)
        printf("%4zu  %-20s  %10zu  %5.2f%%\n",
               i + 1,
               sorted[i].key,
               sorted[i].count,
               100.0 * (double)sorted[i].count / (double)t->total);

    free(sorted);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════*/

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    int rc = 1;
    MappedFile mf = { 0 };
    Worker *workers = NULL;
    Entry *merged_entries = NULL;
    int nworkers = 0;

    if (mf_open(&mf, argv[1]) < 0) {
        (void)fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        goto cleanup;
    }

    nworkers = ncpu();
    if (nworkers > MAX_THREADS)
        nworkers = MAX_THREADS;
    if (mf.size < (1 << 16))
        nworkers = 1;

    workers = calloc((size_t)nworkers, sizeof(Worker));
    if (!workers)
        goto cleanup;

    /* Partition file into chunks, respecting word boundaries */
    size_t chunk = mf.size / (size_t)nworkers;
    size_t arena_size = (chunk / 4 > (1 << 20)) ? chunk / 4 : (1 << 20);

    for (int i = 0; i < nworkers; i++) {
        size_t start = (size_t)i * chunk;
        size_t end = (i == nworkers - 1) ? mf.size : (size_t)(i + 1) * chunk;

        if (i < nworkers - 1)
            while (end < mf.size && is_alpha((unsigned char)mf.data[end]))
                end++;
        if (i > 0)
            while (start < end && is_alpha((unsigned char)mf.data[start]))
                start++;

        workers[i].data = mf.data + start;
        workers[i].len = end - start;
        if (table_init(&workers[i].table, INITIAL_CAP, arena_size) < 0)
            goto cleanup;
    }

    if (spawn_workers(workers, nworkers) < 0)
        goto cleanup;
    join_workers();

    for (int i = 0; i < nworkers; i++)
        if (workers[i].err)
            goto cleanup;

    /* Merge per-thread tables */
    size_t total_unique = 0;
    for (int i = 0; i < nworkers; i++)
        total_unique += workers[i].table.len;

    size_t merged_cap = INITIAL_CAP;
    while (merged_cap < total_unique * 2)
        merged_cap *= 2;

    merged_entries = calloc(merged_cap, sizeof(Entry));
    if (!merged_entries)
        goto cleanup;

    Table merged = { .entries = merged_entries, .cap = merged_cap };
    if (merge_into(&merged, workers, nworkers) < 0)
        goto cleanup;

    /* Output */
    printf("File:   %s\n", argv[1]);
    printf("Size:   %.2f MB\n", (double)mf.size / (1024.0 * 1024.0));
    printf("Words:  %zu total, %zu unique\n", merged.total, merged.len);
    print_top(&merged, TOP_N);

    rc = 0;

cleanup:
    free(merged_entries);
    if (workers) {
        for (int i = 0; i < nworkers; i++)
            table_free(&workers[i].table);
        free(workers);
    }
    mf_close(&mf);
    return rc;
}
