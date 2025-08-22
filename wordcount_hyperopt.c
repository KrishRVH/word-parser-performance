// wordcount_hyperopt.c - v3.3 FINAL
// gcc -O3 -march=native -mtune=native -flto -fomit-frame-pointer -funroll-loops -pthread wordcount_hyperopt.c -o wordcount_hopt -lm

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <immintrin.h>
#include <nmmintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>

#ifdef DEBUG
FILE* debug_log;
uint64_t* collision_counts;
uint64_t* probe_lengths;
uint64_t max_probe_length = 0;
double* thread_times;
uint64_t* thread_word_counts;
uint64_t* thread_unique_counts;
uint64_t* thread_bytes_processed;
uint64_t utf8_errors = 0;
uint64_t utf8_sequences_2byte = 0;
uint64_t utf8_sequences_3byte = 0;
uint64_t utf8_sequences_4byte = 0;
uint64_t hash_resizes = 0;
uint64_t memory_allocated = 0;
uint64_t memory_freed = 0;
uint64_t words_truncated = 0;
uint64_t empty_words = 0;
uint64_t duplicate_merges = 0;
uint64_t simd_chunks = 0;
uint64_t scalar_chunks = 0;
uint64_t pool_exhaustions = 0;
uint64_t* hash_distribution;
uint64_t validation_checksum = 0;

#define DBG(...) do { fprintf(debug_log, "[%s:%d] ", __func__, __LINE__); fprintf(debug_log, __VA_ARGS__); fprintf(debug_log, "\n"); fflush(debug_log); } while(0)
#define TIMING_START(x) struct timespec _t1_##x; clock_gettime(CLOCK_MONOTONIC, &_t1_##x)
#define TIMING_END(x, desc) do { struct timespec _t2_##x; clock_gettime(CLOCK_MONOTONIC, &_t2_##x); double _ms = (_t2_##x.tv_sec - _t1_##x.tv_sec) * 1000.0 + (_t2_##x.tv_nsec - _t1_##x.tv_nsec) / 1000000.0; DBG("%s: %.3f ms", desc, _ms); } while(0)
#define CHECK_NULL(p, desc) do { if (!(p)) { DBG("FATAL: NULL pointer: %s", desc); abort(); } } while(0)
#define CHECK_RANGE(v, min, max, desc) do { if ((v) < (min) || (v) > (max)) { DBG("FATAL: %s out of range: %ld (expected %ld-%ld)", desc, (long)(v), (long)(min), (long)(max)); abort(); } } while(0)

void debug_init() {
    debug_log = fopen("wordcount_debug.log", "w");
    if (!debug_log) debug_log = stderr;
    collision_counts = calloc(16, sizeof(uint64_t));
    probe_lengths = calloc(16, sizeof(uint64_t));
    thread_times = calloc(16, sizeof(double));
    thread_word_counts = calloc(16, sizeof(uint64_t));
    thread_unique_counts = calloc(16, sizeof(uint64_t));
    thread_bytes_processed = calloc(16, sizeof(uint64_t));
    hash_distribution = calloc(256, sizeof(uint64_t));
    DBG("Debug initialized - PID: %d", getpid());
}

void debug_dump_stats() {
    DBG("=== FINAL STATISTICS ===");
    DBG("Memory: allocated=%lu freed=%lu leaked=%lu", memory_allocated, memory_freed, memory_allocated - memory_freed);
    DBG("Pool exhaustions: %lu", pool_exhaustions);
    DBG("Hash resizes: %lu", hash_resizes);
    DBG("Words truncated: %lu", words_truncated);
    DBG("Max probe length: %lu", max_probe_length);
    DBG("SIMD chunks: %lu, Scalar chunks: %lu", simd_chunks, scalar_chunks);
    
    uint64_t total_probes = 0, total_collisions = 0;
    for (int i = 0; i < 16; i++) {
        if (thread_word_counts[i] > 0) {
            total_probes += probe_lengths[i];
            total_collisions += collision_counts[i];
            DBG("T%02d: %.3fms %lu words (%lu unique) %lu collisions (avg probe %.2f)",
                i, thread_times[i], thread_word_counts[i], thread_unique_counts[i],
                collision_counts[i], collision_counts[i] > 0 ? (double)probe_lengths[i]/collision_counts[i] : 0.0);
        }
    }
    if (total_collisions > 0) {
        DBG("Overall avg probe length: %.2f", (double)total_probes / total_collisions);
    }
}

void signal_handler(int sig) {
    void* array[100];
    size_t size = backtrace(array, 100);
    DBG("Signal %d received", sig);
    backtrace_symbols_fd(array, size, fileno(debug_log));
    debug_dump_stats();
    exit(1);
}

static inline void* xaligned_alloc(size_t align, size_t size) {
    size_t aligned_size = (size + (align - 1)) & ~(align - 1);
    void* p = aligned_alloc(align, aligned_size);
    if (p) memory_allocated += aligned_size;
    return p;
}

#define MEMORY_ALLOC(size) ({ void* _p = malloc(size); if (_p) memory_allocated += (size); CHECK_NULL(_p, "malloc"); _p; })
#define MEMORY_ALLOC_ALIGNED(align, size) ({ void* _p = xaligned_alloc(align, size); CHECK_NULL(_p, "aligned_alloc"); _p; })
#define MEMORY_FREE(p, size) do { free(p); memory_freed += (size); } while(0)

#else
#define DBG(...)
#define TIMING_START(x)
#define TIMING_END(x, desc)
#define CHECK_NULL(p, desc)
#define CHECK_RANGE(v, min, max, desc)
#define MEMORY_ALLOC(size) malloc(size)
#define MEMORY_ALLOC_ALIGNED(align, size) xaligned_alloc(align, size)
#define MEMORY_FREE(p, size) free(p)

static inline void* xaligned_alloc(size_t align, size_t size) {
    size_t aligned_size = (size + (align - 1)) & ~(align - 1);
    return aligned_alloc(align, aligned_size);
}
#endif

#define CACHELINE 64

// Thread configuration optimized for AMD Ryzen 9 9950X3D
// Testing showed 8 threads (matching V-Cache CCD physical cores) delivers
// ~46% better performance than 16 threads (832 MB/s vs 571 MB/s)
// This avoids cross-CCD communication overhead and cache thrashing
#ifndef NUM_THREADS
#define NUM_THREADS 8  // Optimal for 9950X3D V-Cache CCD
#endif

// Note on vector width: Testing showed 256-bit vectors slightly hurt performance
// (796 MB/s vs 832 MB/s). The default 512-bit AVX-512 vectors perform best on Zen 5
// despite AMD's general preference for 256-bit operations in some workloads.

#define INITIAL_CAPACITY 65536
#define STRING_POOL_SIZE (32 * 1024 * 1024)
#define MAX_WORD 100
#define TOP_N 100

// Entry with 16-bit fingerprint for fast rejection
typedef struct {
    char* word;
    uint32_t count;
    uint32_t hash;
    uint16_t len;
    uint16_t fp16;  // Fingerprint for quick comparison
} Entry;

typedef struct __attribute__((aligned(CACHELINE))) {
    Entry* entries;
    char* string_pool;
    size_t pool_used;
    size_t capacity;
    size_t size;
    uint64_t total_words;
    int thread_id;
    char** malloc_words;  // Track fallback mallocs for cleanup
    size_t malloc_count;
    size_t malloc_cap;
} ThreadTable;

typedef struct {
    const char* data;
    size_t start;
    size_t end;
    ThreadTable* table;
    int thread_id;
    int drop_leading;
} WorkUnit;

ThreadTable tables[NUM_THREADS];
pthread_t threads[NUM_THREADS];
WorkUnit units[NUM_THREADS];
pthread_barrier_t barrier;

// V-Cache CCD discovery
static int vcache_cpus[256];
static int vcache_cpu_count = 0;

static void discover_vcache_cpus() {
    char path[256], buf[512], sizebuf[64];
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    size_t best_l3_bytes = 0;
    int best_count = 0;
    
    for (int cpu = 0; cpu < ncpus; cpu++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/size", cpu);
        FILE* fsz = fopen(path, "r");
        if (!fsz) continue;
        if (!fgets(sizebuf, sizeof(sizebuf), fsz)) { fclose(fsz); continue; }
        fclose(fsz);
        size_t val = 0; 
        char unit = 0;
        if (sscanf(sizebuf, "%zu%c", &val, &unit) != 2) continue;
        size_t l3_bytes = val;
        if (unit == 'K' || unit == 'k') l3_bytes <<= 10;
        else if (unit == 'M' || unit == 'm') l3_bytes <<= 20;
        else if (unit == 'G' || unit == 'g') l3_bytes <<= 30;
        
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/shared_cpu_list", cpu);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        fclose(f);
        
        int list[256], cnt = 0;
        char* p = buf;
        while (*p && cnt < 256) {
            int start = 0, end = 0;
            if (sscanf(p, "%d-%d", &start, &end) == 2) {
                for (int i = start; i <= end && cnt < 256; i++) list[cnt++] = i;
            } else if (sscanf(p, "%d", &start) == 1) {
                list[cnt++] = start;
            }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
        
        if (l3_bytes > best_l3_bytes || (l3_bytes == best_l3_bytes && cnt > best_count)) {
            best_l3_bytes = l3_bytes;
            best_count = cnt;
            for (int j = 0; j < cnt && j < 256; j++) vcache_cpus[j] = list[j];
            vcache_cpu_count = cnt;
        }
    }
    
    #ifdef DEBUG
    if (vcache_cpu_count > 0) {
        DBG("V-Cache CCD detected: %d CPUs with %zu bytes L3", 
            vcache_cpu_count, best_l3_bytes);
    }
    #endif
}

static inline size_t next_pow2(size_t x) {
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

// CRC32C hash with SSE4.2
#if defined(__SSE4_2__)
static inline uint32_t crc32c_finalize64(uint64_t h) {
    // Full fmix64 for strong avalanche
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

static inline uint32_t hash_word(const char* p, size_t len) {
    uint64_t h = 0;
    const uint8_t* s = (const uint8_t*)p;
    
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, s, 8);
        h = _mm_crc32_u64(h, v);
        s += 8;
        len -= 8;
    }
    
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, s, 4);
        h = _mm_crc32_u32((uint32_t)h, v);
        s += 4;
        len -= 4;
    }
    
    while (len--) {
        h = _mm_crc32_u8((uint32_t)h, *s++);
    }
    
    return crc32c_finalize64(h);
}
#else
static inline uint32_t hash_word(const char* word, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)word[i];
        hash *= 16777619;
    }
    return hash;
}
#endif

static inline char* pool_alloc(ThreadTable* t, size_t len) {
    if (t->pool_used + len + 1 > STRING_POOL_SIZE) {
        #ifdef DEBUG
        pool_exhaustions++;
        #endif
        char* p = (char*)malloc(len + 1);
        if (p == NULL) { perror("malloc"); exit(1); }
        // Track fallback so we can free at shutdown
        if (t->malloc_count == t->malloc_cap) {
            size_t new_cap = t->malloc_cap ? t->malloc_cap * 2 : 1024;
            char** new_arr = (char**)realloc(t->malloc_words,
                                             new_cap * sizeof(char*));
            if (!new_arr) { perror("realloc"); exit(1); }
            t->malloc_words = new_arr;
            t->malloc_cap = new_cap;
        }
        t->malloc_words[t->malloc_count++] = p;
        return p;
    }
    char* ptr = t->string_pool + t->pool_used;
    t->pool_used += (len + 1 + 7) & ~7;
    return ptr;
}

// Dynamic hash table growth
static void table_grow(ThreadTable* t) {
    size_t new_cap = t->capacity * 2;
    Entry* new_entries = (Entry*)MEMORY_ALLOC_ALIGNED(CACHELINE, new_cap * sizeof(Entry));
    memset(new_entries, 0, new_cap * sizeof(Entry));
    
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < t->capacity; i++) {
        Entry e = t->entries[i];
        if (!e.word) continue;
        
        size_t idx = e.hash & mask;
        while (new_entries[idx].word) {
            idx = (idx + 1) & mask;
        }
        new_entries[idx] = e;
    }
    
    MEMORY_FREE(t->entries, t->capacity * sizeof(Entry));
    t->entries = new_entries;
    t->capacity = new_cap;
    
    #ifdef DEBUG
    hash_resizes++;
    DBG("Thread %d resized hash table to %zu", t->thread_id, new_cap);
    #endif
}

static inline void table_insert_hashed(ThreadTable* t,
                                       const char* word,
                                       size_t len,
                                       uint32_t hash,
                                       uint16_t fp) {
    if (len == 0 || len >= MAX_WORD) {
        #ifdef DEBUG
        if (len == 0) empty_words++;
        if (len >= MAX_WORD) words_truncated++;
        #endif
        return;
    }
    
    size_t mask = t->capacity - 1;
    size_t idx = hash & mask;
    
    #ifdef DEBUG
    uint64_t probes = 0;
    hash_distribution[hash & 0xFF]++;
    #endif
    
    size_t start_idx = idx;
    do {
            if (!t->entries[idx].word) {
                char* stored = pool_alloc(t, len);
                memcpy(stored, word, len);
                stored[len] = '\0';
                
                t->entries[idx].word = stored;
                t->entries[idx].count = 1;
                t->entries[idx].hash = hash;
                t->entries[idx].len = len;
                t->entries[idx].fp16 = fp;
                t->size++;
                t->total_words++;
                
                #ifdef __SSE2__
                _mm_prefetch((const char*)&t->entries[(idx + 1) & mask], _MM_HINT_T0);
                _mm_prefetch((const char*)&t->entries[(idx + 8) & mask], _MM_HINT_T2);
                #endif
            
            #ifdef DEBUG
            thread_unique_counts[t->thread_id]++;
            thread_word_counts[t->thread_id]++;
            if (probes > 0) {
                collision_counts[t->thread_id]++;
                probe_lengths[t->thread_id] += probes;
                if (probes > max_probe_length) max_probe_length = probes;
            }
            #endif
            
    if (t->size * 10 > t->capacity * 7) {  // 70% load factor
        table_grow(t);
    }
            return;
        }
        
        if (t->entries[idx].hash == hash &&
            t->entries[idx].len == len &&
            t->entries[idx].fp16 == fp &&
            memcmp(t->entries[idx].word, word, len) == 0) {
            // Found existing word
            t->entries[idx].count++;
            t->total_words++;
            #ifdef DEBUG
            thread_word_counts[t->thread_id]++;
            #endif
            return;
        }
        
        idx = (idx + 1) & mask;
        #ifdef DEBUG
        probes++;
        #endif
    } while (idx != start_idx);
    
    // Table truly full (pathological). Ensure progress in release mode too.
    #ifdef DEBUG
    DBG("FATAL: Thread %d hash table full despite resize logic!", t->thread_id);
    abort();
    #else
    table_grow(t);
    table_insert_hashed(t, word, len, hash, fp);  // Retry after growth
    return;
    #endif
}

static inline void table_insert(ThreadTable* t, const char* word, size_t len) {
    if (len == 0 || len >= MAX_WORD) {
        #ifdef DEBUG
        if (len == 0) empty_words++;
        if (len >= MAX_WORD) words_truncated++;
        #endif
        return;
    }
    
    uint32_t hash = hash_word(word, len);
    uint16_t fp = (uint16_t)(hash ^ (hash >> 16));
    table_insert_hashed(t, word, len, hash, fp);
}

static inline int is_ascii_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Scalar fallback
static void process_chunk(ThreadTable* t, const char* data, size_t size, int drop_leading) {
    char word[MAX_WORD];
    size_t word_len = 0;
    #if defined(__SSE4_2__)
    uint64_t crc = 0;
    #endif
    
    for (size_t i = 0; i < size; i++) {
        unsigned char c = data[i];
        
        if (drop_leading) {
            if (!is_ascii_letter(c)) {
                drop_leading = 0;
            } else {
                continue;
            }
        }
        
        if (c >= 0x80) {
            #ifdef DEBUG
            if ((c & 0xE0) == 0xC0) utf8_sequences_2byte++;
            else if ((c & 0xF0) == 0xE0) utf8_sequences_3byte++;
            else if ((c & 0xF8) == 0xF0) utf8_sequences_4byte++;
            else utf8_errors++;
            #endif
            
            while (i + 1 < size && (data[i + 1] & 0xC0) == 0x80) i++;
            
            if (word_len > 0) {
                word[word_len] = '\0';
                #if defined(__SSE4_2__)
                uint32_t h32 = crc32c_finalize64(crc);
                uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                table_insert_hashed(t, word, word_len, h32, fp);
                crc = 0;
                #else
                table_insert(t, word, word_len);
                #endif
                word_len = 0;
            }
        } else if (is_ascii_letter(c)) {
            if (word_len < MAX_WORD - 1) {
                char lc = (char)(c | 0x20);
                word[word_len++] = lc;
                #if defined(__SSE4_2__)
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
                #endif
            }
        } else if (word_len > 0) {
            word[word_len] = '\0';
            #if defined(__SSE4_2__)
            uint32_t h32 = crc32c_finalize64(crc);
            uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
            table_insert_hashed(t, word, word_len, h32, fp);
            crc = 0;
            #else
            table_insert(t, word, word_len);
            #endif
            word_len = 0;
        }
    }
    
    if (word_len > 0) {
        word[word_len] = '\0';
        #if defined(__SSE4_2__)
        uint32_t h32 = crc32c_finalize64(crc);
        uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
        table_insert_hashed(t, word, word_len, h32, fp);
        #else
        table_insert(t, word, word_len);
        #endif
    }
}

// Real AVX-512 run extraction
static void process_chunk_avx512(ThreadTable* t, const char* data, size_t size, int drop_leading) {
    #ifdef __AVX512BW__
    char word[MAX_WORD];
    size_t word_len = 0;
    size_t i = 0;
    int prev_tail_letter = 0;
    #if defined(__SSE4_2__)
    uint64_t crc = 0;
    #endif
    
    const size_t simd_end = size - (size % 64);
    
    for (; i < simd_end; i += 64) {
        #ifdef DEBUG
        simd_chunks++;
        #endif
        
        __m512i chunk = _mm512_loadu_si512((const __m512i*)(data + i));
        
        // ASCII and letter detection
        __mmask64 ascii_mask = _mm512_cmplt_epu8_mask(chunk, _mm512_set1_epi8(0x80));
        __m512i up = _mm512_sub_epi8(chunk, _mm512_set1_epi8('A'));
        __m512i lo = _mm512_sub_epi8(chunk, _mm512_set1_epi8('a'));
        __mmask64 m_up = _mm512_cmplt_epu8_mask(up, _mm512_set1_epi8(26));
        __mmask64 m_lo = _mm512_cmplt_epu8_mask(lo, _mm512_set1_epi8(26));
        uint64_t letters = (uint64_t)((m_up | m_lo) & ascii_mask);
        
        // Handle word continuation from previous chunk
        if (prev_tail_letter && ((letters & 1ull) == 0)) {
            if (word_len > 0) {
                word[word_len] = '\0';
                #if defined(__SSE4_2__)
                uint32_t h32 = crc32c_finalize64(crc);
                uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                table_insert_hashed(t, word, word_len, h32, fp);
                crc = 0;
                #else
                table_insert(t, word, word_len);
                #endif
                word_len = 0;
            }
            prev_tail_letter = 0;
        }
        
        // Handle cross-thread straddle
        if (drop_leading && (letters & 1ull)) {
            // If the entire block is letters, skip whole block safely
            if ((~letters) == 0ull) {
                continue;
            }
            unsigned lead = __builtin_ctzll(~letters);
            // Clear the first 'lead' bits (skip straddled prefix)
            letters &= (~0ull << lead);
            drop_leading = 0;
        } else if (drop_leading) {
            drop_leading = 0;
        }
        
        if (letters == 0ull) {
            // No letters - flush any open word
            if (word_len > 0) {
                word[word_len] = '\0';
                #if defined(__SSE4_2__)
                uint32_t h32 = crc32c_finalize64(crc);
                uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                table_insert_hashed(t, word, word_len, h32, fp);
                crc = 0;
                #else
                table_insert(t, word, word_len);
                #endif
                word_len = 0;
            }
            prev_tail_letter = 0;
            continue;
        }
        
        #ifdef DEBUG
        scalar_chunks++;
        #endif
        
        // Extract runs of letters
        uint64_t m = letters;
        while (m) {
            unsigned start = __builtin_ctzll(m);
            uint64_t tail = m >> start;
            unsigned run_len;
            if ((~tail) == 0ull) {
                // Run reaches block end
                run_len = 64 - start;
            } else {
                run_len = __builtin_ctzll(~tail);
            }
            
            // Flush word if we hit a delimiter
            if (start > 0 && word_len > 0) {
                word[word_len] = '\0';
                #if defined(__SSE4_2__)
                uint32_t h32 = crc32c_finalize64(crc);
                uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                table_insert_hashed(t, word, word_len, h32, fp);
                crc = 0;
                #else
                table_insert(t, word, word_len);
                #endif
                word_len = 0;
            }
            
            // Append run to word buffer (lowercase)
            const char* src = data + i + start;
            for (unsigned k = 0; k < run_len && word_len < MAX_WORD - 1; k++) {
                char lc = (char)(src[k] | 0x20);
                word[word_len++] = lc;
                #if defined(__SSE4_2__)
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
                #endif
            }
            
            // Check if run ends before block end
            if ((start + run_len) < 64) {
                if (word_len > 0) {
                    word[word_len] = '\0';
                    #if defined(__SSE4_2__)
                    uint32_t h32 = crc32c_finalize64(crc);
                    uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                    table_insert_hashed(t, word, word_len, h32, fp);
                    crc = 0;
                    #else
                    table_insert(t, word, word_len);
                    #endif
                    word_len = 0;
                }
                prev_tail_letter = 0;
            } else {
                prev_tail_letter = 1;
            }
            
            // Clear processed bits
            uint64_t mask = (run_len >= 64) ? ~0ull
                                            : (((run_len ? (1ull << run_len) : 0ull) - 1ull)
                                               << start);
            m &= ~mask;
        }
    }
    
    // Handle remainder
    for (; i < size; i++) {
        unsigned char c = data[i];
        
        if (drop_leading) {
            if (!is_ascii_letter(c)) drop_leading = 0;
            else continue;
        }
        
        if (c >= 0x80) {
            while (i + 1 < size && (data[i + 1] & 0xC0) == 0x80) i++;
            if (word_len > 0) {
                word[word_len] = '\0';
                #if defined(__SSE4_2__)
                uint32_t h32 = crc32c_finalize64(crc);
                uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
                table_insert_hashed(t, word, word_len, h32, fp);
                crc = 0;
                #else
                table_insert(t, word, word_len);
                #endif
                word_len = 0;
            }
        } else if (is_ascii_letter(c)) {
            if (word_len < MAX_WORD - 1) {
                char lc = (char)(c | 0x20);
                word[word_len++] = lc;
                #if defined(__SSE4_2__)
                crc = _mm_crc32_u8((uint32_t)crc, (uint8_t)lc);
                #endif
            }
        } else if (word_len > 0) {
            word[word_len] = '\0';
            #if defined(__SSE4_2__)
            uint32_t h32 = crc32c_finalize64(crc);
            uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
            table_insert_hashed(t, word, word_len, h32, fp);
            crc = 0;
            #else
            table_insert(t, word, word_len);
            #endif
            word_len = 0;
        }
    }
    
    if (word_len > 0) {
        word[word_len] = '\0';
        #if defined(__SSE4_2__)
        uint32_t h32 = crc32c_finalize64(crc);
        uint16_t fp = (uint16_t)(h32 ^ (h32 >> 16));
        table_insert_hashed(t, word, word_len, h32, fp);
        #else
        table_insert(t, word, word_len);
        #endif
    }
    #else
    process_chunk(t, data, size, drop_leading);
    #endif
}

static void* worker(void* arg) {
    WorkUnit* unit = (WorkUnit*)arg;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int cpu_id = unit->thread_id;
    if (vcache_cpu_count > 0) {
        cpu_id = vcache_cpus[unit->thread_id % vcache_cpu_count];
    }
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    pthread_barrier_wait(&barrier);
    
    #ifdef DEBUG
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    thread_bytes_processed[unit->thread_id] = unit->end - unit->start;
    #endif
    
    process_chunk_avx512(unit->table, unit->data + unit->start, 
                         unit->end - unit->start, unit->drop_leading);
    
    #ifdef DEBUG
    clock_gettime(CLOCK_MONOTONIC, &t2);
    thread_times[unit->thread_id] = (t2.tv_sec - t1.tv_sec) * 1000.0 + 
                                     (t2.tv_nsec - t1.tv_nsec) / 1000000.0;
    #endif
    
    return NULL;
}

static Entry* merge_tables(size_t* total_unique, size_t* total_words, size_t* out_capacity) {
    size_t estimated = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        estimated += tables[i].size;
    }
    
    size_t global_cap = next_pow2(estimated * 2);
    Entry* global = MEMORY_ALLOC_ALIGNED(CACHELINE, global_cap * sizeof(Entry));
    memset(global, 0, global_cap * sizeof(Entry));
    
    size_t global_size = 0;
    *total_words = 0;
    
    for (int t = 0; t < NUM_THREADS; t++) {
        ThreadTable* tbl = &tables[t];
        *total_words += tbl->total_words;
        
        for (size_t i = 0; i < tbl->capacity; i++) {
            if (!tbl->entries[i].word) continue;
            
            uint32_t hash = tbl->entries[i].hash;
            uint16_t fp = tbl->entries[i].fp16;
            size_t idx = hash & (global_cap - 1);
            
            while (global[idx].word) {
                if (global[idx].hash == hash &&
                    global[idx].len == tbl->entries[i].len &&
                    global[idx].fp16 == fp &&
                    memcmp(global[idx].word, tbl->entries[i].word, tbl->entries[i].len) == 0) {
                    global[idx].count += tbl->entries[i].count;
                    #ifdef DEBUG
                    duplicate_merges++;
                    #endif
                    goto next_word;
                }
                idx = (idx + 1) & (global_cap - 1);
            }
            
            global[idx] = tbl->entries[i];
            global_size++;
            
            next_word:;
        }
    }
    
    *total_unique = global_size;
    *out_capacity = global_cap;
    
    return global;
}

static int cmp_entry(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    // Avoid overflow with large counts
    if (eb->count != ea->count) return (eb->count > ea->count) ? 1 : -1;
    return strcmp(ea->word, eb->word);
}

// FIXED Min-heap for top-K
typedef struct {
    Entry* entries;
    int size;
    int capacity;
} MinHeap;

static inline void heap_swap(Entry* a, Entry* b) {
    Entry t = *a;
    *a = *b;
    *b = t;
}

static void heap_sift_up(MinHeap* h, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->entries[p].count <= h->entries[i].count) break;
        heap_swap(&h->entries[p], &h->entries[i]);
        i = p;
    }
}

static void heap_sift_down(MinHeap* h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < h->size && h->entries[l].count < h->entries[s].count) s = l;
        if (r < h->size && h->entries[r].count < h->entries[s].count) s = r;
        if (s == i) break;
        heap_swap(&h->entries[i], &h->entries[s]);
        i = s;
    }
}

static inline void heap_push(MinHeap* h, Entry e) {
    if (h->size < h->capacity) {
        h->entries[h->size] = e;
        heap_sift_up(h, h->size);
        h->size++;
    } else if (e.count > h->entries[0].count) {
        h->entries[0] = e;
        heap_sift_down(h, 0);
    }
}

static void format_number(char* buf, uint64_t n) {
    char tmp[32];
    sprintf(tmp, "%lu", n);
    int len = strlen(tmp);
    int commas = (len - 1) / 3;
    int out_len = len + commas;
    buf[out_len] = '\0';
    
    int t = len - 1, o = out_len - 1, d = 0;
    while (t >= 0) {
        if (d == 3) {
            buf[o--] = ',';
            d = 0;
        }
        buf[o--] = tmp[t--];
        d++;
    }
}

int main(int argc, char* argv[]) {
    const char* filename = (argc > 1) ? argv[1] : "book.txt";
    
    #ifdef DEBUG
    debug_init();
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    #endif
    
    printf("Processing: %s\n", filename);
    #ifdef __AVX512BW__
    printf("AVX-512 enabled, CRC32C hash\n");
    #else
    printf("Scalar fallback\n");
    #endif
    
    discover_vcache_cpus();
    if (vcache_cpu_count > 0) {
        printf("V-Cache CCD: %d CPUs\n", vcache_cpu_count);
    }
    printf("\n");
    
    struct timespec total_start;
    clock_gettime(CLOCK_MONOTONIC, &total_start);
    
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    size_t file_size = st.st_size;
    
    TIMING_START(mmap);
    char* data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    TIMING_END(mmap, "mmap");
    
    madvise(data, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
    
    // Initialize tables
    TIMING_START(init);
    for (int i = 0; i < NUM_THREADS; i++) {
        size_t chunk_size = file_size / NUM_THREADS;
        size_t estimated_words = chunk_size / 5;
        size_t estimated_unique = estimated_words / 10;
        tables[i].capacity = next_pow2(estimated_unique * 2);
        if (tables[i].capacity < INITIAL_CAPACITY) tables[i].capacity = INITIAL_CAPACITY;
        
        tables[i].entries = MEMORY_ALLOC_ALIGNED(CACHELINE, tables[i].capacity * sizeof(Entry));
        memset(tables[i].entries, 0, tables[i].capacity * sizeof(Entry));
        tables[i].string_pool = MEMORY_ALLOC_ALIGNED(CACHELINE, STRING_POOL_SIZE);
        tables[i].pool_used = 0;
        tables[i].size = 0;
        tables[i].total_words = 0;
        tables[i].thread_id = i;
        tables[i].malloc_words = NULL;
        tables[i].malloc_count = 0;
        tables[i].malloc_cap = 0;
        
        // Hint for huge pages
        madvise(tables[i].entries, tables[i].capacity * sizeof(Entry), MADV_HUGEPAGE);
        madvise(tables[i].string_pool, STRING_POOL_SIZE, MADV_HUGEPAGE);
    }
    TIMING_END(init, "init");
    
    pthread_barrier_init(&barrier, NULL, NUM_THREADS + 1);
    
    // Setup work units with proper boundaries
    size_t chunk = file_size / NUM_THREADS;
    for (int i = 0; i < NUM_THREADS; i++) {
        units[i].data = data;
        units[i].start = i * chunk;
        units[i].end = (i == NUM_THREADS - 1) ? file_size : (i + 1) * chunk;
        units[i].table = &tables[i];
        units[i].thread_id = i;
        
        // Extend end to next word boundary
        if (i < NUM_THREADS - 1) {
            while (units[i].end < file_size && is_ascii_letter(data[units[i].end])) {
                units[i].end++;
            }
        }
        
        // Check if we need to drop leading letters
        units[i].drop_leading = (units[i].start > 0 && is_ascii_letter(data[units[i].start - 1]));
        
        pthread_create(&threads[i], NULL, worker, &units[i]);
    }
    
    TIMING_START(processing);
    pthread_barrier_wait(&barrier);
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    TIMING_END(processing, "processing");
    
    TIMING_START(merge);
    size_t total_unique, total_words, global_capacity;
    Entry* global = merge_tables(&total_unique, &total_words, &global_capacity);
    TIMING_END(merge, "merge");
    
    // Top-K extraction with FIXED min-heap
    TIMING_START(topk);
    Entry* top_words;
    size_t top_count;
    size_t top_words_bytes = 0;
    
    if (total_unique > TOP_N * 10) {
        MinHeap heap = {
            .entries = MEMORY_ALLOC(TOP_N * sizeof(Entry)),
            .size = 0,
            .capacity = TOP_N
        };
        
        for (size_t i = 0; i < global_capacity; i++) {
            if (global[i].word) {
                heap_push(&heap, global[i]);
            }
        }
        
        qsort(heap.entries, heap.size, sizeof(Entry), cmp_entry);
        top_words = heap.entries;
        top_count = heap.size;
        top_words_bytes = TOP_N * sizeof(Entry);
    } else {
        Entry* words = MEMORY_ALLOC_ALIGNED(CACHELINE, total_unique * sizeof(Entry));
        size_t word_count = 0;
        
        for (size_t i = 0; i < global_capacity && word_count < total_unique; i++) {
            if (global[i].word) {
                words[word_count++] = global[i];
            }
        }
        
        qsort(words, word_count, sizeof(Entry), cmp_entry);
        top_words = words;
        top_count = word_count;
        top_words_bytes = total_unique * sizeof(Entry);
    }
    TIMING_END(topk, "top-k");
    
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double exec_ms = (t_end.tv_sec - total_start.tv_sec) * 1000.0 + 
                     (t_end.tv_nsec - total_start.tv_nsec) / 1000000.0;
    
    munmap(data, file_size);
    close(fd);
    
    char total_str[32], unique_str[32];
    format_number(total_str, total_words);
    format_number(unique_str, total_unique);
    
    printf("=== Top 10 Words ===\n");
    for (int i = 0; i < 10 && i < top_count; i++) {
        char count_str[32];
        format_number(count_str, top_words[i].count);
        printf("%2d. %-15s %9s\n", i + 1, top_words[i].word, count_str);
    }
    
    printf("\nFile size:       %.2f MB\n", file_size / (1024.0 * 1024.0));
    printf("Total words:     %s\n", total_str);
    printf("Unique words:    %s\n", unique_str);
    printf("Execution time:  %.2f ms\n", exec_ms);
    printf("Throughput:      %.2f MB/s\n", (file_size / (1024.0 * 1024.0)) / (exec_ms / 1000.0));
    
    #ifdef DEBUG
    printf("\nDEBUG MODE - Check wordcount_debug.log\n");
    debug_dump_stats();
    fclose(debug_log);
    #endif
    
    // Write output
    char outfile[256];
    snprintf(outfile, sizeof(outfile), "%.*s_c-hopt_results.txt",
             (int)(strrchr(filename, '.') ? strrchr(filename, '.') - filename : strlen(filename)), 
             filename);
    
    FILE* out = fopen(outfile, "w");
    if (out) {
        fprintf(out, "Word Frequency Analysis\n");
        fprintf(out, "File: %s\n", filename);
        fprintf(out, "Time: %.2f ms\n", exec_ms);
        fprintf(out, "Throughput: %.2f MB/s\n\n", (file_size / (1024.0 * 1024.0)) / (exec_ms / 1000.0));
        fprintf(out, "Total: %lu\n", total_words);
        fprintf(out, "Unique: %lu\n\n", total_unique);
        fprintf(out, "Top 100:\n");
        
        for (int i = 0; i < TOP_N && i < top_count; i++) {
            fprintf(out, "%4d  %-15s %9u %6.2f%%\n",
                    i + 1, top_words[i].word, top_words[i].count, 
                    (top_words[i].count * 100.0) / total_words);
        }
        
        fclose(out);
        printf("\nResults: %s\n", outfile);
    }
    
    // Cleanup - ALWAYS free top_words
    for (int i = 0; i < NUM_THREADS; i++) {
        // Free any per-word malloc fallbacks
        for (size_t j = 0; j < tables[i].malloc_count; j++) {
            free(tables[i].malloc_words[j]);
        }
        free(tables[i].malloc_words);
        MEMORY_FREE(tables[i].entries, tables[i].capacity * sizeof(Entry));
        MEMORY_FREE(tables[i].string_pool, STRING_POOL_SIZE);
    }
    MEMORY_FREE(global, global_capacity * sizeof(Entry));
    MEMORY_FREE(top_words, top_words_bytes);
    pthread_barrier_destroy(&barrier);
    
    #ifdef DEBUG
    debug_dump_stats();
    #endif
    
    return 0;
}