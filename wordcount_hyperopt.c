/*
 * wordcount_hyperopt.c - High-performance word frequency counter
 *
 * Build:
 *   gcc -O3 -march=native -pthread wordcount_hyperopt.c -o wc
 *   gcc -O3 -march=znver5 -mtune=znver5 -mavx512f -mavx512bw -mavx512vl
 * -msse4.2 \ -flto -fomit-frame-pointer -funroll-loops -pthread
 * wordcount_hyperopt.c -o wc
 *
 * Design:
 *   - AVX-512 SIMD tokenization with scalar fallback
 *   - CRC32C hardware hashing (FNV-1a fallback)
 *   - Per-thread hash tables with arena allocation
 *   - V-Cache aware thread pinning (AMD Zen 4+)
 *   - No shared mutable state in hot path
 *   - Huge page hints for hash tables and string pools
 *   - Prefetch hints in hash table insert path
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX512BW__
#include <immintrin.h>
#endif

/*===========================================================================
 * Configuration (overridable via -D flags)
 *===========================================================================*/

#ifndef NUM_THREADS
#define NUM_THREADS 6
#endif

#ifndef INITIAL_CAP
#define INITIAL_CAP 65536
#endif

#ifndef POOL_SIZE
#define POOL_SIZE (32 << 20)
#endif

#ifndef MAX_WORD
#define MAX_WORD 100
#endif

#ifndef TOP_N
#define TOP_N 10
#endif

#define CACHELINE 64

/*===========================================================================
 * Hash Table Entry
 *===========================================================================*/

typedef struct {
    char *word;
    uint32_t count;
    uint32_t hash;
    uint16_t len;
    uint16_t fp16;
} Entry;

/*===========================================================================
 * Per-Thread Table with Arena
 *===========================================================================*/

typedef struct __attribute__((aligned(CACHELINE))) {
    Entry *entries;
    char *pool;
    size_t pool_used;
    size_t cap;
    size_t len;
    size_t total;
    int id;
    /* Overflow for when pool exhausts */
    char **overflow;
    size_t overflow_count;
    size_t overflow_cap;
} Table;

/*===========================================================================
 * Globals
 *===========================================================================*/

static Table tables[NUM_THREADS];
static pthread_t threads[NUM_THREADS];
static pthread_barrier_t barrier;

static int vcache_cpus[256];
static int vcache_count = 0;

/*===========================================================================
 * V-Cache Detection (AMD Zen 4+ with 3D V-Cache)
 *===========================================================================*/

static void detect_vcache(void)
{
    char path[256];
    char buf[512];
    char sizebuf[64];
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    size_t best_l3 = 0;
    int best_count = 0;

    for (int cpu = 0; cpu < ncpus && cpu < 256; cpu++) {
        (void)snprintf(path,
                       sizeof(path),
                       "/sys/devices/system/cpu/cpu%d/cache/index3/size",
                       cpu);
        FILE *fsz = fopen(path, "r");
        if (!fsz)
            continue;
        if (!fgets(sizebuf, (int)sizeof(sizebuf), fsz)) {
            (void)fclose(fsz);
            continue;
        }
        (void)fclose(fsz);

        char *endptr = NULL;
        unsigned long val = strtoul(sizebuf, &endptr, 10);
        if (endptr == sizebuf)
            continue;

        size_t l3_bytes = val;
        if (endptr && (*endptr == 'K' || *endptr == 'k'))
            l3_bytes <<= 10;
        else if (endptr && (*endptr == 'M' || *endptr == 'm'))
            l3_bytes <<= 20;
        else if (endptr && (*endptr == 'G' || *endptr == 'g'))
            l3_bytes <<= 30;

        /* Read shared_cpu_list to find all CPUs sharing this L3 */
        (void)snprintf(
                path,
                sizeof(path),
                "/sys/devices/system/cpu/cpu%d/cache/index3/shared_cpu_list",
                cpu);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(buf, (int)sizeof(buf), f)) {
            (void)fclose(f);
            continue;
        }
        (void)fclose(f);

        int list[256];
        int cnt = 0;
        const char *p = buf;
        while (*p && cnt < 256) {
            char *endp = NULL;
            long start = strtol(p, &endp, 10);
            if (endp == p)
                break;

            if (*endp == '-') {
                char *endp2 = NULL;
                long end = strtol(endp + 1, &endp2, 10);
                for (long i = start; i <= end && cnt < 256; i++)
                    list[cnt++] = (int)i;
                p = endp2;
            } else {
                list[cnt++] = (int)start;
                p = endp;
            }

            while (*p && *p != ',')
                p++;
            if (*p == ',')
                p++;
        }

        if (l3_bytes > best_l3 || (l3_bytes == best_l3 && cnt > best_count)) {
            best_l3 = l3_bytes;
            best_count = cnt;
            for (int j = 0; j < cnt && j < 256; j++)
                vcache_cpus[j] = list[j];
            vcache_count = cnt;
        }
    }
}

/*===========================================================================
 * Power of 2 Helper
 *===========================================================================*/

static inline size_t next_pow2(size_t x)
{
    if (x <= 1)
        return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

/*===========================================================================
 * Hash Functions
 *===========================================================================*/

#ifdef __SSE4_2__
static inline uint32_t crc32c_finalize(uint64_t h)
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
    const uint8_t *p = (const uint8_t *)s;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = _mm_crc32_u64(h, v);
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, p, 4);
        h = _mm_crc32_u32((uint32_t)h, v);
        p += 4;
        len -= 4;
    }
    while (len--) {
        h = _mm_crc32_u8((uint32_t)h, *p++);
    }
    return crc32c_finalize(h);
}
#else
static inline uint32_t hash_word(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}
#endif

/*===========================================================================
 * Pool Allocator (8-byte aligned, with overflow to malloc)
 *===========================================================================*/

static inline char *pool_alloc(Table *t, size_t len)
{
    size_t needed = (len + 1 + 7) & ~(size_t)7; /* 8-byte alignment */

    if (t->pool_used + needed > POOL_SIZE) {
        /* Overflow: use malloc */
        char *p = malloc(len + 1);
        if (!p) {
            perror("malloc");
            exit(1);
        }

        if (t->overflow_count == t->overflow_cap) {
            size_t cap = t->overflow_cap ? t->overflow_cap * 2 : 64;
            char **tmp = realloc(t->overflow, cap * sizeof(char *));
            if (!tmp) {
                free(p);
                perror("realloc");
                exit(1);
            }
            t->overflow = tmp;
            t->overflow_cap = cap;
        }
        t->overflow[t->overflow_count++] = p;
        return p;
    }

    char *ptr = t->pool + t->pool_used;
    t->pool_used += needed;
    return ptr;
}

/*===========================================================================
 * Hash Table Operations
 *===========================================================================*/

static void table_grow(Table *t)
{
    size_t new_cap = t->cap * 2;
    Entry *new_ent = aligned_alloc(CACHELINE, new_cap * sizeof(Entry));
    if (!new_ent) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(new_ent, 0, new_cap * sizeof(Entry));

    size_t mask = new_cap - 1;
    for (size_t i = 0; i < t->cap; i++) {
        const Entry *e = &t->entries[i];
        if (!e->word)
            continue;
        size_t idx = e->hash & mask;
        while (new_ent[idx].word)
            idx = (idx + 1) & mask;
        new_ent[idx] = *e;
    }

    free(t->entries);
    t->entries = new_ent;
    t->cap = new_cap;

    /* Huge page hint for new table */
    (void)madvise(new_ent, new_cap * sizeof(Entry), MADV_HUGEPAGE);
}

static inline void
table_insert(Table *t, const char *word, size_t len, uint32_t hash, uint16_t fp)
{
    if (len == 0 || len >= MAX_WORD)
        return;

    size_t mask = t->cap - 1;
    size_t idx = hash & mask;

    for (;;) {
        Entry *e = &t->entries[idx];

        if (!e->word) {
            /* New entry */
            char *s = pool_alloc(t, len);
            memcpy(s, word, len);
            s[len] = '\0';
            e->word = s;
            e->count = 1;
            e->hash = hash;
            e->len = (uint16_t)len;
            e->fp16 = fp;
            t->len++;
            t->total++;

            /* Prefetch next slots for future inserts */
#ifdef __SSE2__
            _mm_prefetch((const char *)&t->entries[(idx + 1) & mask],
                         _MM_HINT_T0);
            _mm_prefetch((const char *)&t->entries[(idx + 8) & mask],
                         _MM_HINT_T2);
#endif
            /* Grow if load factor > 0.7 */
            if (t->len * 10 > t->cap * 7)
                table_grow(t);
            return;
        }

        /* Check for match: hash + len + fingerprint + memcmp */
        if (e->hash == hash && e->len == len && e->fp16 == fp &&
            memcmp(e->word, word, len) == 0) {
            e->count++;
            t->total++;
            return;
        }

        idx = (idx + 1) & mask;
    }
}

/*===========================================================================
 * Character Classification
 *===========================================================================*/

static inline int is_letter(unsigned char c)
{
    return ((c | 32u) - 'a') < 26u;
}

/*===========================================================================
 * Scalar Tokenizer (with inline CRC accumulation)
 *===========================================================================*/

#ifndef __AVX512BW__
static void
process_scalar(Table *t, const char *data, size_t size, int drop_leading)
{
    char word[MAX_WORD];
    size_t word_len = 0;
#ifdef __SSE4_2__
    uint64_t crc = 0;
#endif

    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)data[i];

        if (drop_leading) {
            if (!is_letter(c))
                drop_leading = 0;
            else
                continue;
        }

        /* Handle UTF-8 multibyte sequences */
        if (c >= 0x80) {
            while (i + 1 < size && (data[i + 1] & 0xC0) == 0x80)
                i++;
            if (word_len > 0) {
                word[word_len] = '\0';
#ifdef __SSE4_2__
                uint32_t h = crc32c_finalize(crc);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
                crc = 0;
#else
                uint32_t h = hash_word(word, word_len);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
#endif
                word_len = 0;
            }
            continue;
        }

        if (is_letter(c)) {
            if (word_len < MAX_WORD - 1) {
                char lc = (char)(c | 0x20);
                word[word_len++] = lc;
#ifdef __SSE4_2__
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
#endif
            }
        } else if (word_len > 0) {
            word[word_len] = '\0';
#ifdef __SSE4_2__
            uint32_t h = crc32c_finalize(crc);
            uint16_t fp = (uint16_t)(h ^ (h >> 16));
            table_insert(t, word, word_len, h, fp);
            crc = 0;
#else
            uint32_t h = hash_word(word, word_len);
            uint16_t fp = (uint16_t)(h ^ (h >> 16));
            table_insert(t, word, word_len, h, fp);
#endif
            word_len = 0;
        }
    }

    /* Flush remaining word */
    if (word_len > 0) {
        word[word_len] = '\0';
#ifdef __SSE4_2__
        uint32_t h = crc32c_finalize(crc);
        uint16_t fp = (uint16_t)(h ^ (h >> 16));
        table_insert(t, word, word_len, h, fp);
#else
        uint32_t h = hash_word(word, word_len);
        uint16_t fp = (uint16_t)(h ^ (h >> 16));
        table_insert(t, word, word_len, h, fp);
#endif
    }
}
#endif /* !__AVX512BW__ */

/*===========================================================================
 * AVX-512 Tokenizer
 *===========================================================================*/

#ifdef __AVX512BW__
static void
process_avx512(Table *t, const char *data, size_t size, int drop_leading)
{
    char word[MAX_WORD];
    size_t word_len = 0;
    size_t i = 0;
    int prev_tail_letter = 0;
#ifdef __SSE4_2__
    uint64_t crc = 0;
#endif

    const size_t simd_end = size - (size % 64);

    for (; i < simd_end; i += 64) {
        __m512i chunk = _mm512_loadu_si512((const __m512i *)(data + i));

        /* Build letter mask */
        __mmask64 ascii =
                _mm512_cmplt_epu8_mask(chunk, _mm512_set1_epi8((char)0x80));
        __m512i up = _mm512_sub_epi8(chunk, _mm512_set1_epi8('A'));
        __m512i lo = _mm512_sub_epi8(chunk, _mm512_set1_epi8('a'));
        __mmask64 m_up = _mm512_cmplt_epu8_mask(up, _mm512_set1_epi8(26));
        __mmask64 m_lo = _mm512_cmplt_epu8_mask(lo, _mm512_set1_epi8(26));
        uint64_t letters = (uint64_t)((m_up | m_lo) & ascii);

        /* Handle word boundary from previous chunk */
        if (prev_tail_letter && !(letters & 1ULL)) {
            if (word_len > 0) {
                word[word_len] = '\0';
#ifdef __SSE4_2__
                uint32_t h = crc32c_finalize(crc);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
                crc = 0;
#else
                uint32_t h = hash_word(word, word_len);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
#endif
                word_len = 0;
            }
            prev_tail_letter = 0;
        }

        /* Handle drop_leading */
        if (drop_leading && (letters & 1ULL)) {
            if ((~letters) == 0ULL)
                continue;
            unsigned lead = (unsigned)__builtin_ctzll(~letters);
            letters &= (~0ULL << lead);
            drop_leading = 0;
        } else if (drop_leading) {
            drop_leading = 0;
        }

        /* No letters in chunk */
        if (letters == 0ULL) {
            if (word_len > 0) {
                word[word_len] = '\0';
#ifdef __SSE4_2__
                uint32_t h = crc32c_finalize(crc);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
                crc = 0;
#else
                uint32_t h = hash_word(word, word_len);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
#endif
                word_len = 0;
            }
            prev_tail_letter = 0;
            continue;
        }

        /* Process runs of letters */
        uint64_t m = letters;
        while (m) {
            unsigned start = (unsigned)__builtin_ctzll(m);
            uint64_t tail = m >> start;
            unsigned run_len = ((~tail) == 0ULL)
                                       ? (64 - start)
                                       : (unsigned)__builtin_ctzll(~tail);

            /* Flush word before gap */
            if (start > 0 && word_len > 0) {
                word[word_len] = '\0';
#ifdef __SSE4_2__
                uint32_t h = crc32c_finalize(crc);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
                crc = 0;
#else
                uint32_t h = hash_word(word, word_len);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
#endif
                word_len = 0;
            }

            /* Accumulate characters */
            const char *src = data + i + start;
            for (unsigned k = 0; k < run_len && word_len < MAX_WORD - 1; k++) {
                char lc = (char)(src[k] | 0x20);
                word[word_len++] = lc;
#ifdef __SSE4_2__
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
#endif
            }

            /* Flush if run ended within chunk */
            if (start + run_len < 64) {
                if (word_len > 0) {
                    word[word_len] = '\0';
#ifdef __SSE4_2__
                    uint32_t h = crc32c_finalize(crc);
                    uint16_t fp = (uint16_t)(h ^ (h >> 16));
                    table_insert(t, word, word_len, h, fp);
                    crc = 0;
#else
                    uint32_t h = hash_word(word, word_len);
                    uint16_t fp = (uint16_t)(h ^ (h >> 16));
                    table_insert(t, word, word_len, h, fp);
#endif
                    word_len = 0;
                }
                prev_tail_letter = 0;
            } else {
                prev_tail_letter = 1;
            }

            uint64_t mask =
                    (run_len >= 64)
                            ? ~0ULL
                            : (((run_len ? (1ULL << run_len) : 0ULL) - 1ULL)
                               << start);
            m &= ~mask;
        }
    }

    /* Process tail with scalar */
    for (; i < size; i++) {
        unsigned char c = (unsigned char)data[i];

        if (drop_leading) {
            if (!is_letter(c))
                drop_leading = 0;
            else
                continue;
        }

        if (c >= 0x80) {
            while (i + 1 < size && (data[i + 1] & 0xC0) == 0x80)
                i++;
            if (word_len > 0) {
                word[word_len] = '\0';
#ifdef __SSE4_2__
                uint32_t h = crc32c_finalize(crc);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
                crc = 0;
#else
                uint32_t h = hash_word(word, word_len);
                uint16_t fp = (uint16_t)(h ^ (h >> 16));
                table_insert(t, word, word_len, h, fp);
#endif
                word_len = 0;
            }
            continue;
        }

        if (is_letter(c)) {
            if (word_len < MAX_WORD - 1) {
                char lc = (char)(c | 0x20);
                word[word_len++] = lc;
#ifdef __SSE4_2__
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
#endif
            }
        } else if (word_len > 0) {
            word[word_len] = '\0';
#ifdef __SSE4_2__
            uint32_t h = crc32c_finalize(crc);
            uint16_t fp = (uint16_t)(h ^ (h >> 16));
            table_insert(t, word, word_len, h, fp);
            crc = 0;
#else
            uint32_t h = hash_word(word, word_len);
            uint16_t fp = (uint16_t)(h ^ (h >> 16));
            table_insert(t, word, word_len, h, fp);
#endif
            word_len = 0;
        }
    }

    /* Flush final word */
    if (word_len > 0) {
        word[word_len] = '\0';
#ifdef __SSE4_2__
        uint32_t h = crc32c_finalize(crc);
        uint16_t fp = (uint16_t)(h ^ (h >> 16));
        table_insert(t, word, word_len, h, fp);
#else
        uint32_t h = hash_word(word, word_len);
        uint16_t fp = (uint16_t)(h ^ (h >> 16));
        table_insert(t, word, word_len, h, fp);
#endif
    }
}
#endif

static void
process_chunk(Table *t, const char *data, size_t size, int drop_leading)
{
#ifdef __AVX512BW__
    process_avx512(t, data, size, drop_leading);
#else
    process_scalar(t, data, size, drop_leading);
#endif
}

/*===========================================================================
 * Worker Thread
 *===========================================================================*/

typedef struct {
    const char *data;
    size_t start;
    size_t end;
    Table *table;
    int id;
    int drop_leading;
} WorkUnit;

static WorkUnit units[NUM_THREADS];

static void *worker(void *arg)
{
    WorkUnit *u = arg;

    /* Pin to V-Cache CCD if available */
    if (vcache_count > 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int cpu = vcache_cpus[u->id % vcache_count];
        CPU_SET((size_t)cpu, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    (void)pthread_barrier_wait(&barrier);
    process_chunk(
            u->table, u->data + u->start, u->end - u->start, u->drop_leading);
    return NULL;
}

/*===========================================================================
 * Merge Tables
 *===========================================================================*/

static Entry *
merge_tables(size_t *out_unique, size_t *out_total, size_t *out_cap)
{
    size_t est = 0;
    for (int i = 0; i < NUM_THREADS; i++)
        est += tables[i].len;

    size_t cap = next_pow2(est * 2);
    Entry *global = aligned_alloc(CACHELINE, cap * sizeof(Entry));
    if (!global) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(global, 0, cap * sizeof(Entry));

    size_t glen = 0;
    size_t gtotal = 0;
    size_t mask = cap - 1;

    for (int ti = 0; ti < NUM_THREADS; ti++) {
        Table *tbl = &tables[ti];
        gtotal += tbl->total;

        for (size_t i = 0; i < tbl->cap; i++) {
            const Entry *e = &tbl->entries[i];
            if (!e->word)
                continue;

            size_t idx = e->hash & mask;
            while (global[idx].word) {
                if (global[idx].hash == e->hash && global[idx].len == e->len &&
                    global[idx].fp16 == e->fp16 &&
                    memcmp(global[idx].word, e->word, e->len) == 0) {
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

/*===========================================================================
 * Output
 *===========================================================================*/

static int cmp_count_desc(const void *a, const void *b)
{
    const Entry *ea = a;
    const Entry *eb = b;
    if (ea->count != eb->count)
        return (eb->count > ea->count) ? 1 : -1;
    return strcmp(ea->word, eb->word);
}

static void print_top(const Entry *entries,
                      size_t cap,
                      size_t unique,
                      size_t total,
                      size_t file_size,
                      double ms)
{
    Entry *arr = malloc(unique * sizeof(Entry));
    if (!arr)
        return;

    size_t j = 0;
    for (size_t i = 0; i < cap && j < unique; i++) {
        if (entries[i].word)
            arr[j++] = entries[i];
    }

    qsort(arr, unique, sizeof(Entry), cmp_count_desc);

    printf("\n=== Top %d Words ===\n", TOP_N);
    size_t n = unique < TOP_N ? unique : TOP_N;
    for (size_t i = 0; i < n; i++) {
        printf("%2zu. %-15s %9u  (%5.2f%%)\n",
               i + 1,
               arr[i].word,
               arr[i].count,
               100.0 * (double)arr[i].count / (double)total);
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
    size_t file_size = 0;
    Entry *global = NULL;
    size_t global_cap = 0;
    int tables_init = 0;

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
        printf("V-Cache: %d cores\n", vcache_count);

    /* Open and mmap file */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        goto cleanup;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        goto cleanup;
    }
    if (st.st_size <= 0) {
        (void)fprintf(stderr, "Empty file\n");
        goto cleanup;
    }
    file_size = (size_t)st.st_size;

    data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }

    (void)madvise(data, file_size, MADV_SEQUENTIAL);
    (void)madvise(data, file_size, MADV_WILLNEED);

    /* Compute non-overlapping partition cuts */
    size_t cuts[NUM_THREADS + 1];
    cuts[0] = 0;
    cuts[NUM_THREADS] = file_size;
    for (int i = 1; i < NUM_THREADS; i++) {
        size_t c = (file_size * (size_t)i) / NUM_THREADS;
        while (c < file_size && is_letter((unsigned char)data[c]))
            c++;
        cuts[i] = c;
    }

    /* Initialize per-thread tables with dynamic sizing */
    for (int i = 0; i < NUM_THREADS; i++) {
        Table *t = &tables[i];
        size_t chunk_size = cuts[i + 1] - cuts[i];
        size_t estimated_words = chunk_size / 5;
        size_t estimated_unique = estimated_words / 10;

        t->cap = next_pow2(estimated_unique * 2);
        if (t->cap < INITIAL_CAP)
            t->cap = INITIAL_CAP;

        t->entries = aligned_alloc(CACHELINE, t->cap * sizeof(Entry));
        if (!t->entries) {
            perror("aligned_alloc");
            goto cleanup;
        }
        memset(t->entries, 0, t->cap * sizeof(Entry));

        t->pool = aligned_alloc(CACHELINE, POOL_SIZE);
        if (!t->pool) {
            perror("aligned_alloc");
            goto cleanup;
        }

        t->pool_used = 0;
        t->len = 0;
        t->total = 0;
        t->id = i;
        t->overflow = NULL;
        t->overflow_count = 0;
        t->overflow_cap = 0;

        /* Huge page hints */
        (void)madvise(t->entries, t->cap * sizeof(Entry), MADV_HUGEPAGE);
        (void)madvise(t->pool, POOL_SIZE, MADV_HUGEPAGE);

        units[i].data = data;
        units[i].start = cuts[i];
        units[i].end = cuts[i + 1];
        units[i].table = t;
        units[i].id = i;
        units[i].drop_leading = 0;
    }
    tables_init = NUM_THREADS;

    /* Launch workers */
    (void)pthread_barrier_init(&barrier, NULL, NUM_THREADS + 1);
    for (int i = 0; i < NUM_THREADS; i++) {
        (void)pthread_create(&threads[i], NULL, worker, &units[i]);
    }

    (void)pthread_barrier_wait(&barrier);
    for (int i = 0; i < NUM_THREADS; i++) {
        (void)pthread_join(threads[i], NULL);
    }
    (void)pthread_barrier_destroy(&barrier);

    /* Merge */
    size_t unique = 0;
    size_t total = 0;
    global = merge_tables(&unique, &total, &global_cap);

    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    print_top(global, global_cap, unique, total, file_size, ms);
    rc = 0;

cleanup:
    free(global);
    for (int i = 0; i < tables_init; i++) {
        for (size_t j = 0; j < tables[i].overflow_count; j++) {
            free(tables[i].overflow[j]);
        }
        free(tables[i].overflow);
        free(tables[i].entries);
        free(tables[i].pool);
    }
    if (data != MAP_FAILED)
        (void)munmap(data, file_size);
    if (fd >= 0)
        (void)close(fd);
    return rc;
}
