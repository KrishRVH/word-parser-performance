/*
 * wc.c - Word frequency counter
 * Build: gcc -O2 -std=c11 wc.c -o wc
 * Usage: ./wc <filename>
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum
{
    HASH_BITS = 14,
    HASH_SIZE = 1u << HASH_BITS,
    MAX_WORD = 64,
    TOP_N = 10,

    /*
     * Arena sizing: empirically, English text averages ~5 chars/word.
     * With overhead (WordNode + alignment + pointer), budget ~16 bytes
     * per word. file_size/8 approximates total words, so file_size/8 * 16
     * = file_size * 2 would be safe. We use file_size/4 as a conservative
     * middle ground, plus a page for small files.
     */
    ARENA_DIVISOR = 4,
    ARENA_MIN = 4096,
};

_Static_assert((HASH_SIZE & (HASH_SIZE - 1)) == 0,
               "HASH_SIZE must be power of 2");

typedef struct WordNode WordNode;
struct WordNode
{
    WordNode *next;
    size_t count;
    uint32_t hash;
    char word[];
};

typedef struct
{
    char *base;
    size_t used, cap;
} Arena;

typedef struct
{
    WordNode *buckets[HASH_SIZE];
    Arena arena;
    size_t total, unique;
} WordTable;

typedef struct
{
    const char *word;
    size_t count;
} WordCount;

/* --- Arena -------------------------------------------------------------- */

static void *arena_alloc(Arena *a, size_t size, size_t align)
{
    size_t pad = (align - (a->used & (align - 1))) & (align - 1);
    if (a->used + pad + size > a->cap)
        return NULL;
    void *p = a->base + a->used + pad;
    a->used += pad + size;
    return p;
}

/* --- Hash table --------------------------------------------------------- */

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

static int word_table_init(WordTable *t, size_t arena_cap)
{
    memset(t, 0, sizeof *t);
    t->arena.base = mmap(NULL,
                         arena_cap,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);
    if (t->arena.base == MAP_FAILED)
        return -1;
    t->arena.cap = arena_cap;
    return 0;
}

static void word_table_free(WordTable *t)
{
    if (t->arena.base && t->arena.base != MAP_FAILED)
        munmap(t->arena.base, t->arena.cap);
}

static int word_table_add(WordTable *restrict t,
                          const char *restrict word,
                          size_t len,
                          uint32_t hash)
{
    size_t idx = hash & (HASH_SIZE - 1);

    for (WordNode *n = t->buckets[idx]; n; n = n->next)
    {
        if (n->hash == hash && memcmp(n->word, word, len) == 0)
        {
            n->count++;
            t->total++;
            return 0;
        }
    }

    WordNode *n =
            arena_alloc(&t->arena, sizeof *n + len + 1, _Alignof(WordNode));
    if (!n)
        return -1;

    memcpy(n->word, word, len);
    n->word[len] = '\0';
    n->hash = hash;
    n->count = 1;
    n->next = t->buckets[idx];
    t->buckets[idx] = n;
    t->total++;
    t->unique++;
    return 0;
}

static int cmp_count_desc(const void *a, const void *b)
{
    const WordCount *wa = a, *wb = b;
    if (wa->count != wb->count)
        return wa->count < wb->count ? 1 : -1;
    return strcmp(wa->word, wb->word);
}

static WordCount *word_table_sorted(const WordTable *t)
{
    WordCount *arr = malloc(t->unique * sizeof *arr);
    if (!arr)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < HASH_SIZE; i++)
        for (WordNode *n = t->buckets[i]; n; n = n->next)
            arr[j++] = (WordCount){ n->word, n->count };

    qsort(arr, t->unique, sizeof *arr, cmp_count_desc);
    return arr;
}

/* --- Tokenizer ---------------------------------------------------------- */

static inline int is_letter(unsigned char c)
{
    return (c | 32u) - 'a' < 26u;
}

/*
 * Advances *cursor past non-letters, then extracts a word into buf
 * (lowercased), computing FNV-1a hash incrementally. Returns word
 * length, or 0 if no word found before end.
 */
static size_t next_word(const char **cursor,
                        const char *end,
                        char *buf,
                        size_t bufsz,
                        uint32_t *out_hash)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    const unsigned char *e = (const unsigned char *)end;

    while (s < e && !is_letter(*s))
        s++;
    if (s >= e)
    {
        *cursor = (const char *)s;
        return 0;
    }

    uint32_t h = FNV_OFFSET;
    size_t i = 0;

    while (s < e && is_letter(*s))
    {
        unsigned char c = *s | 32u;
        h = (h ^ c) * FNV_PRIME;
        if (i + 1 < bufsz)
            buf[i++] = c;
        s++;
    }

    buf[i] = '\0';
    *cursor = (const char *)s;
    *out_hash = h;
    return i;
}

static int process(WordTable *t, const char *data, size_t len)
{
    const char *p = data, *end = data + len;
    char word[MAX_WORD];
    uint32_t hash;
    size_t wlen;

    while ((wlen = next_word(&p, end, word, sizeof word, &hash)) > 0)
        if (word_table_add(t, word, wlen, hash) < 0)
            return -1;
    return 0;
}

/* --- Output ------------------------------------------------------------- */

static void
print_results(const WordTable *t, const WordCount *words, size_t file_size)
{
    puts("\n=== Top 10 Most Frequent Words ===");
    puts("  Rank  Word                 Count      %");
    puts("  ----  ---------------  ---------  -----");

    size_t n = t->unique < TOP_N ? t->unique : TOP_N;
    for (size_t i = 0; i < n; i++)
        printf("  %4zu  %-15s  %9zu  %5.2f\n",
               i + 1,
               words[i].word,
               words[i].count,
               100.0 * (double)words[i].count / (double)t->total);

    puts("\n=== Statistics ===");
    printf("File size:       %.2f MB\n", (double)file_size / (1024.0 * 1024.0));
    printf("Total words:     %zu\n", t->total);
    printf("Unique words:    %zu\n", t->unique);
}

/* --- Main --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];
    int fd = -1, rc = EXIT_FAILURE;
    void *map = MAP_FAILED;
    size_t size = 0, arena_size = 0;
    WordTable table = { 0 };
    WordCount *sorted = NULL;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0)
        goto err_open;
    if (fstat(fd, &st) < 0)
        goto err_stat;
    if (st.st_size == 0)
    {
        fprintf(stderr, "%s: empty file\n", path);
        goto out;
    }

    size = (size_t)st.st_size;

    /* Hint sequential access pattern to kernel */
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    if ((map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
        goto err_mmap;

    /* Advise sequential read-through for the mapped region */
    (void)posix_madvise(map, size, POSIX_MADV_SEQUENTIAL);

    arena_size = size / ARENA_DIVISOR;
    if (arena_size < ARENA_MIN)
        arena_size = ARENA_MIN;

    if (word_table_init(&table, arena_size) < 0)
        goto err_mem;
    if (process(&table, map, size) < 0)
        goto err_mem;

    if (table.unique == 0)
    {
        puts("No words found.");
        rc = EXIT_SUCCESS;
        goto out;
    }

    if (!(sorted = word_table_sorted(&table)))
        goto err_mem;

    print_results(&table, sorted, size);
    rc = EXIT_SUCCESS;
    goto out;

err_open:
    fprintf(stderr, "open %s: %s\n", path, strerror(errno));
    goto out;
err_stat:
    fprintf(stderr, "stat %s: %s\n", path, strerror(errno));
    goto out;
err_mmap:
    fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
    goto out;
err_mem:
    fputs("out of memory\n", stderr);

out:
    free(sorted);
    word_table_free(&table);
    if (map != MAP_FAILED)
        munmap(map, size);
    if (fd >= 0)
        close(fd);
    return rc;
}
