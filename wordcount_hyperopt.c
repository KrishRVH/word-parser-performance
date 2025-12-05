/*
 * wordcount_final.c - High-performance word frequency counter
 *
 * Build:
 *   gcc -O3 -march=native -pthread wordcount_final.c -o wc
 *
 * Design:
 *   - AVX-512 SIMD tokenization with scalar fallback
 *   - CRC32C hardware hashing (FNV-1a fallback)
 *   - Per-thread hash tables with arena allocation
 *   - V-Cache aware thread pinning (AMD Zen 4+)
 *   - No shared mutable state in hot path
 */

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif
#ifdef __AVX512BW__
#include <immintrin.h>
#endif

/*===========================================================================
 * Configuration
 *===========================================================================*/

enum
{
    NUM_THREADS = 6,
    INITIAL_CAP = 1 << 16,
    POOL_SIZE = 32 << 20,
    MAX_WORD = 100,
    TOP_N = 10,
    CACHELINE = 64,
};

/*===========================================================================
 * Arena Allocator (Wellons-inspired)
 *===========================================================================*/

typedef struct
{
    char *ptr;
    char *end;
    char **overflow;
    size_t overflow_count;
    size_t overflow_cap;
} Arena;

static void *arena_alloc(Arena *a, size_t size, size_t align)
{
    size_t pad = (-(uintptr_t)a->ptr) & (align - 1);
    if (a->ptr + pad + size > a->end)
    {
        char *p = malloc(size);
        if (!p)
        {
            perror("malloc");
            exit(1);
        }
        if (a->overflow_count == a->overflow_cap)
        {
            size_t cap = a->overflow_cap ? a->overflow_cap * 2 : 64;
            char **tmp = realloc(a->overflow, cap * sizeof(char *));
            if (!tmp)
            {
                free(p);
                perror("realloc");
                exit(1);
            }
            a->overflow = tmp;
            a->overflow_cap = cap;
        }
        a->overflow[a->overflow_count++] = p;
        return p;
    }
    void *p = a->ptr + pad;
    a->ptr += pad + size;
    return p;
}

static void arena_free(Arena *a)
{
    for (size_t i = 0; i < a->overflow_count; i++)
    {
        free(a->overflow[i]);
    }
    free(a->overflow);
}

/*===========================================================================
 * Hash Table (open addressing, linear probing)
 *===========================================================================*/

typedef struct
{
    char *word;
    uint32_t count;
    uint32_t hash;
    uint16_t len;
    uint16_t fp;
} Entry;

typedef struct __attribute__((aligned(CACHELINE)))
{
    Entry *entries;
    Arena arena;
    char *pool;
    size_t cap;
    size_t len;
    size_t total;
    int id;
} Table;

#ifdef __SSE4_2__
static inline uint32_t hash_finalize(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

static inline uint32_t hash_word(const char *s, size_t len)
{
    uint64_t h = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (len >= 8)
    {
        uint64_t v;
        memcpy(&v, p, 8);
        h = _mm_crc32_u64(h, v);
        p += 8;
        len -= 8;
    }
    while (len--)
    {
        h = _mm_crc32_u8((uint32_t)h, *p++);
    }
    return hash_finalize(h);
}
#else
static inline uint32_t hash_word(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
#endif

static void table_grow(Table *t)
{
    size_t new_cap = t->cap * 2;
    Entry *new_ent = aligned_alloc(CACHELINE, new_cap * sizeof(Entry));
    if (!new_ent)
    {
        perror("aligned_alloc");
        exit(1);
    }
    memset(new_ent, 0, new_cap * sizeof(Entry));

    size_t mask = new_cap - 1;
    for (size_t i = 0; i < t->cap; i++)
    {
        const Entry *e = &t->entries[i];
        if (!e->word)
            continue;
        size_t idx = e->hash & mask;
        while (new_ent[idx].word)
        {
            idx = (idx + 1) & mask;
        }
        new_ent[idx] = *e;
    }

    free(t->entries);
    t->entries = new_ent;
    t->cap = new_cap;
}

static void
table_insert(Table *t, const char *word, size_t len, uint32_t hash, uint16_t fp)
{
    if (len == 0 || len >= MAX_WORD)
        return;
    if (t->len * 10 > t->cap * 7)
        table_grow(t);

    size_t mask = t->cap - 1;
    size_t idx = hash & mask;

    for (;;)
    {
        Entry *e = &t->entries[idx];
        if (!e->word)
        {
            char *s = arena_alloc(&t->arena, len + 1, 1);
            memcpy(s, word, len);
            s[len] = '\0';
            e->word = s;
            e->count = 1;
            e->hash = hash;
            e->len = (uint16_t)len;
            e->fp = fp;
            t->len++;
            t->total++;
            return;
        }
        if (e->hash == hash && e->len == len && e->fp == fp &&
            memcmp(e->word, word, len) == 0)
        {
            e->count++;
            t->total++;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

/*===========================================================================
 * Word Buffer (eliminates duplication in tokenizers)
 *===========================================================================*/

typedef struct
{
    char buf[MAX_WORD];
    size_t len;
#ifdef __SSE4_2__
    uint64_t crc;
#endif
} WordBuf;

static inline void word_init(WordBuf *w)
{
    w->len = 0;
#ifdef __SSE4_2__
    w->crc = 0;
#endif
}

static inline void word_add(WordBuf *w, unsigned char c)
{
    if (w->len < MAX_WORD - 1)
    {
        w->buf[w->len++] = (char)c;
#ifdef __SSE4_2__
        w->crc = _mm_crc32_u8((uint32_t)w->crc, c);
#endif
    }
}

static inline void word_flush(Table *t, WordBuf *w)
{
    if (w->len == 0)
        return;
    w->buf[w->len] = '\0';
#ifdef __SSE4_2__
    uint32_t hash = hash_finalize(w->crc);
#else
    uint32_t hash = hash_word(w->buf, w->len);
#endif
    uint16_t fp = (uint16_t)(hash ^ (hash >> 16));
    table_insert(t, w->buf, w->len, hash, fp);
    word_init(w);
}

/*===========================================================================
 * Tokenizer - Scalar
 *===========================================================================*/

static inline int is_letter(unsigned int c)
{
    return ((c | 32u) - 'a') < 26u;
}

static void tokenize_scalar(Table *t, const char *data, size_t len, int skip)
{
    WordBuf w;
    word_init(&w);

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)data[i];

        /* Skip leading partial word if requested */
        if (skip)
        {
            if (!is_letter(c))
            {
                skip = 0;
            }
            continue;
        }

        /* Skip UTF-8 multibyte sequences */
        if (c >= 0x80)
        {
            while (i + 1 < len && (data[i + 1] & 0xC0) == 0x80)
            {
                i++;
            }
            word_flush(t, &w);
            continue;
        }

        if (is_letter(c))
        {
            word_add(&w, c | 0x20u);
        }
        else
        {
            word_flush(t, &w);
        }
    }
    word_flush(t, &w);
}

/*===========================================================================
 * Tokenizer - AVX-512 (64 bytes at a time)
 *===========================================================================*/

#ifdef __AVX512BW__
static void tokenize_avx512(Table *t, const char *data, size_t len, int skip)
{
    WordBuf w;
    word_init(&w);

    size_t i = 0;
    const size_t simd_end = len & ~63ULL;

    /* 0x80 as signed char to avoid overflow warning */
    const __m512i high_bit = _mm512_set1_epi8((char)-128);
    const __m512i char_A = _mm512_set1_epi8('A');
    const __m512i char_a = _mm512_set1_epi8('a');
    const __m512i char_26 = _mm512_set1_epi8(26);

    for (; i < simd_end; i += 64)
    {
        __m512i chunk = _mm512_loadu_si512(data + i);

        /* Find ASCII letters */
        __mmask64 ascii = _mm512_cmplt_epu8_mask(chunk, high_bit);
        __m512i up = _mm512_sub_epi8(chunk, char_A);
        __m512i lo = _mm512_sub_epi8(chunk, char_a);
        __mmask64 m_up = _mm512_cmplt_epu8_mask(up, char_26);
        __mmask64 m_lo = _mm512_cmplt_epu8_mask(lo, char_26);
        uint64_t letters = (uint64_t)((m_up | m_lo) & ascii);

        /* Handle skip mode */
        if (skip)
        {
            if (letters & 1ULL)
            {
                unsigned lead = (unsigned)__builtin_ctzll(~letters);
                letters &= ~((1ULL << lead) - 1);
            }
            skip = 0;
        }

        /* No letters in chunk */
        if (!letters)
        {
            word_flush(t, &w);
            continue;
        }

        /* Process runs of letters */
        uint64_t m = letters;
        while (m)
        {
            unsigned start = (unsigned)__builtin_ctzll(m);
            uint64_t tail = m >> start;
            unsigned run;
            if (tail == ~0ULL)
            {
                run = 64 - start;
            }
            else
            {
                run = (unsigned)__builtin_ctzll(~tail);
            }

            /* Flush before gap */
            if (start > 0 || (w.len > 0 && !(letters & 1ULL)))
            {
                word_flush(t, &w);
            }

            /* Add characters (lowercase) */
            const unsigned char *src = (const unsigned char *)data + i + start;
            for (unsigned k = 0; k < run; k++)
            {
                word_add(&w, src[k] | 0x20u);
            }

            /* Flush if run ended within chunk */
            if (start + run < 64)
            {
                word_flush(t, &w);
            }

            m &= ~(((1ULL << run) - 1) << start);
        }
    }

    /* Scalar tail */
    if (i < len)
    {
        tokenize_scalar(t, data + i, len - i, 0);
    }
    else
    {
        word_flush(t, &w);
    }
}
#endif

static void tokenize(Table *t, const char *data, size_t len, int skip)
{
#ifdef __AVX512BW__
    tokenize_avx512(t, data, len, skip);
#else
    tokenize_scalar(t, data, len, skip);
#endif
}

/*===========================================================================
 * V-Cache Detection (AMD Zen 4+)
 *===========================================================================*/

static int vcache_cpus[256];
static int vcache_count = 0;

static void detect_vcache(void)
{
    char path[256];
    char buf[256];
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    size_t best_l3 = 0;

    for (int cpu = 0; cpu < ncpus && cpu < 256; cpu++)
    {
        int written =
                snprintf(path,
                         sizeof(path),
                         "/sys/devices/system/cpu/cpu%d/cache/index3/size",
                         cpu);
        if (written < 0 || (size_t)written >= sizeof(path))
            continue;

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        if (!fgets(buf, (int)sizeof(buf), f))
        {
            (void)fclose(f);
            continue;
        }
        (void)fclose(f);

        /* Parse size like "96M" or "32768K" */
        char *endptr = NULL;
        unsigned long val = strtoul(buf, &endptr, 10);
        if (endptr == buf)
            continue;

        size_t l3_bytes = val;
        if (endptr && (*endptr == 'K' || *endptr == 'k'))
        {
            l3_bytes <<= 10;
        }
        else if (endptr && (*endptr == 'M' || *endptr == 'm'))
        {
            l3_bytes <<= 20;
        }

        if (l3_bytes > best_l3)
        {
            best_l3 = l3_bytes;
            vcache_count = 0;
        }
        if (l3_bytes == best_l3)
        {
            vcache_cpus[vcache_count++] = cpu;
        }
    }
}

/*===========================================================================
 * Worker Thread
 *===========================================================================*/

typedef struct
{
    const char *data;
    size_t start;
    size_t end;
    Table *table;
    int id;
    int skip;
} WorkUnit;

static Table tables[NUM_THREADS];
static WorkUnit units[NUM_THREADS];
static pthread_t threads[NUM_THREADS];
static pthread_barrier_t barrier;

static void *worker(void *arg)
{
    WorkUnit *u = arg;

    /* Pin to V-Cache CCD if available */
    if (vcache_count > 0)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int target_cpu = vcache_cpus[u->id % vcache_count];
        CPU_SET((size_t)target_cpu, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    (void)pthread_barrier_wait(&barrier);
    tokenize(u->table, u->data + u->start, u->end - u->start, u->skip);
    return NULL;
}

/*===========================================================================
 * Merge and Output
 *===========================================================================*/

static Entry *
merge_tables(size_t *out_unique, size_t *out_total, size_t *out_cap)
{
    size_t est = 0;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        est += tables[i].len;
    }

    size_t cap = 1;
    while (cap < est * 2)
    {
        cap <<= 1;
    }

    Entry *global = aligned_alloc(CACHELINE, cap * sizeof(Entry));
    if (!global)
    {
        perror("aligned_alloc");
        exit(1);
    }
    memset(global, 0, cap * sizeof(Entry));

    size_t glen = 0;
    size_t gtotal = 0;
    size_t mask = cap - 1;

    for (int ti = 0; ti < NUM_THREADS; ti++)
    {
        Table *tbl = &tables[ti];
        gtotal += tbl->total;

        for (size_t i = 0; i < tbl->cap; i++)
        {
            const Entry *e = &tbl->entries[i];
            if (!e->word)
                continue;

            size_t idx = e->hash & mask;
            while (global[idx].word)
            {
                if (global[idx].hash == e->hash && global[idx].len == e->len &&
                    memcmp(global[idx].word, e->word, e->len) == 0)
                {
                    global[idx].count += e->count;
                    goto next;
                }
                idx = (idx + 1) & mask;
            }
            global[idx] = *e;
            glen++;
next:;
        }
    }

    *out_unique = glen;
    *out_total = gtotal;
    *out_cap = cap;
    return global;
}

static int cmp_count_desc(const void *a, const void *b)
{
    const Entry *ea = a;
    const Entry *eb = b;
    if (ea->count != eb->count)
    {
        return (eb->count > ea->count) ? 1 : -1;
    }
    return strcmp(ea->word, eb->word);
}

static void print_top(const Entry *entries,
                      size_t cap,
                      size_t unique,
                      size_t total,
                      size_t file_size,
                      double ms)
{
    /* Compact to contiguous array */
    Entry *arr = malloc(unique * sizeof(Entry));
    if (!arr)
        return;

    size_t j = 0;
    for (size_t i = 0; i < cap && j < unique; i++)
    {
        if (entries[i].word)
        {
            arr[j++] = entries[i];
        }
    }

    qsort(arr, unique, sizeof(Entry), cmp_count_desc);

    printf("\n=== Top %d Words ===\n", TOP_N);
    size_t n = unique < TOP_N ? unique : TOP_N;
    for (size_t i = 0; i < n; i++)
    {
        double pct = 100.0 * (double)arr[i].count / (double)total;
        printf("%2zu. %-15s %9u  (%5.2f%%)\n",
               i + 1,
               arr[i].word,
               arr[i].count,
               pct);
    }

    double size_mb = (double)file_size / (1024.0 * 1024.0);
    printf("\nFile size:       %.2f MB\n", size_mb);
    printf("Total words:     %zu\n", total);
    printf("Unique words:    %zu\n", unique);
    printf("Time:            %.2f ms\n", ms);
    printf("Throughput:      %.2f MB/s\n", size_mb / (ms / 1000.0));

    free(arr);
}

/*===========================================================================
 * Main
 *===========================================================================*/

int main(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : "book.txt";
    int rc = 1;
    int fd = -1;
    char *data = MAP_FAILED;
    size_t size = 0;
    Entry *global = NULL;
    size_t global_cap = 0;
    int tables_initialized = 0;

    struct timespec t0;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);

    printf("Processing: %s\n", path);
#ifdef __AVX512BW__
    printf("Mode: AVX-512 + CRC32C\n");
#elif defined(__SSE4_2__)
    printf("Mode: CRC32C\n");
#else
    printf("Mode: Scalar + FNV-1a\n");
#endif

    detect_vcache();
    if (vcache_count > 0)
    {
        printf("V-Cache: %d cores\n", vcache_count);
    }

    /* Open and map file */
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        goto cleanup;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        perror("fstat");
        goto cleanup;
    }
    if (st.st_size <= 0)
    {
        (void)fprintf(stderr, "Empty or invalid file\n");
        goto cleanup;
    }
    size = (size_t)st.st_size;

    data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED)
    {
        perror("mmap");
        goto cleanup;
    }

    (void)madvise(data, size, MADV_SEQUENTIAL);
    (void)madvise(data, size, MADV_WILLNEED);

    /* Partition file at word boundaries */
    size_t cuts[NUM_THREADS + 1];
    cuts[0] = 0;
    cuts[NUM_THREADS] = size;
    for (int i = 1; i < NUM_THREADS; i++)
    {
        size_t c = (size * (size_t)i) / NUM_THREADS;
        while (c < size && is_letter((unsigned char)data[c]))
        {
            c++;
        }
        cuts[i] = c;
    }

    /* Initialize per-thread tables */
    for (int i = 0; i < NUM_THREADS; i++)
    {
        Table *t = &tables[i];
        t->cap = INITIAL_CAP;
        t->entries = aligned_alloc(CACHELINE, t->cap * sizeof(Entry));
        if (!t->entries)
        {
            perror("aligned_alloc");
            goto cleanup;
        }
        memset(t->entries, 0, t->cap * sizeof(Entry));

        t->pool = aligned_alloc(CACHELINE, POOL_SIZE);
        if (!t->pool)
        {
            perror("aligned_alloc");
            goto cleanup;
        }
        t->arena.ptr = t->pool;
        t->arena.end = t->pool + POOL_SIZE;
        t->arena.overflow = NULL;
        t->arena.overflow_count = 0;
        t->arena.overflow_cap = 0;
        t->len = 0;
        t->total = 0;
        t->id = i;

        units[i].data = data;
        units[i].start = cuts[i];
        units[i].end = cuts[i + 1];
        units[i].table = t;
        units[i].id = i;
        units[i].skip = 0;
    }
    tables_initialized = NUM_THREADS;

    /* Process in parallel */
    (void)pthread_barrier_init(&barrier, NULL, NUM_THREADS + 1);
    for (int i = 0; i < NUM_THREADS; i++)
    {
        (void)pthread_create(&threads[i], NULL, worker, &units[i]);
    }

    (void)pthread_barrier_wait(&barrier);
    for (int i = 0; i < NUM_THREADS; i++)
    {
        (void)pthread_join(threads[i], NULL);
    }
    (void)pthread_barrier_destroy(&barrier);

    /* Merge and output */
    size_t unique = 0;
    size_t total = 0;
    global = merge_tables(&unique, &total, &global_cap);

    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    print_top(global, global_cap, unique, total, size, ms);
    rc = 0;

cleanup:
    free(global);
    for (int i = 0; i < tables_initialized; i++)
    {
        free(tables[i].entries);
        free(tables[i].pool);
        arena_free(&tables[i].arena);
    }
    if (data != MAP_FAILED)
    {
        (void)munmap(data, size);
    }
    if (fd >= 0)
    {
        (void)close(fd);
    }
    return rc;
}
