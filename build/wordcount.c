/*
 * wordcount.c - Word frequency counter
 * Build: gcc -O2 -std=c11 wordcount.c -o wordcount
 * Usage: ./wordcount <filename>
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
};

_Static_assert((HASH_SIZE & (HASH_SIZE - 1)) == 0,
               "HASH_SIZE must be power of 2");

typedef struct WordNode WordNode;
struct WordNode
{
    WordNode *next;
    size_t count;
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

static inline uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static int word_table_init(WordTable *t, size_t arena_cap)
{
    memset(t, 0, sizeof *t);
    if (!(t->arena.base = malloc(arena_cap)))
        return -1;
    t->arena.cap = arena_cap;
    return 0;
}

static void word_table_free(WordTable *t)
{
    free(t->arena.base);
}

static int word_table_add(WordTable *restrict t, const char *restrict word)
{
    size_t idx = fnv1a(word) & (HASH_SIZE - 1);

    for (WordNode *n = t->buckets[idx]; n; n = n->next)
    {
        if (strcmp(n->word, word) == 0)
        {
            n->count++;
            t->total++;
            return 0;
        }
    }

    size_t len = strlen(word);
    WordNode *n =
            arena_alloc(&t->arena, sizeof *n + len + 1, _Alignof(WordNode));
    if (!n)
        return -1;

    memcpy(n->word, word, len + 1);
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

static WordCount *word_table_sorted(const WordTable *t, size_t *out_n)
{
    WordCount *arr = malloc(t->unique * sizeof *arr);
    if (!arr)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < HASH_SIZE; i++)
        for (WordNode *n = t->buckets[i]; n; n = n->next)
            arr[j++] = (WordCount){ n->word, n->count };

    qsort(arr, t->unique, sizeof *arr, cmp_count_desc);
    *out_n = t->unique;
    return arr;
}

/* --- Tokenizer ---------------------------------------------------------- */

static inline int is_letter(unsigned char c)
{
    return (c | 32u) - 'a' < 26u;
}

static const char *next_word(const char *restrict p,
                             const char *restrict end,
                             char *restrict buf,
                             size_t bufsz)
{
    const unsigned char *s = (const unsigned char *)p;
    const unsigned char *e = (const unsigned char *)end;

    while (s < e && !is_letter(*s))
        s++;
    if (s >= e)
        return NULL;

    size_t i = 0;
    while (s < e && is_letter(*s))
    {
        if (i + 1 < bufsz)
            buf[i++] = *s | 32u;
        s++;
    }
    buf[i] = '\0';
    return (const char *)s;
}

static int process(WordTable *t, const char *data, size_t len)
{
    const char *p = data, *end = data + len;
    char word[MAX_WORD];

    while ((p = next_word(p, end, word, sizeof word)))
        if (word_table_add(t, word) < 0)
            return -1;
    return 0;
}

/* --- Output ------------------------------------------------------------- */

static void print_results(const WordTable *t,
                          const WordCount *words,
                          size_t nwords,
                          size_t file_size)
{
    puts("\n=== Top 10 Most Frequent Words ===");
    puts("  Rank  Word                 Count      %");
    puts("  ----  ---------------  ---------  -----");

    size_t n = nwords < TOP_N ? nwords : TOP_N;
    for (size_t i = 0; i < n; i++)
        printf("  %4zu  %-15s  %9zu  %5.2f\n",
               i + 1,
               words[i].word,
               words[i].count,
               100.0 * words[i].count / t->total);

    puts("\n=== Statistics ===");
    printf("File size:       %.2f MB\n", file_size / (1024.0 * 1024.0));
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
    size_t size = 0;
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
    if ((map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
        goto err_mmap;
    if (word_table_init(&table, size / 8 + 4096) < 0)
        goto err_mem;
    if (process(&table, map, size) < 0)
        goto err_mem;

    if (table.unique == 0)
    {
        puts("No words found.");
        rc = EXIT_SUCCESS;
        goto out;
    }

    if (!(sorted = word_table_sorted(&table, &(size_t){ 0 })))
        goto err_mem;

    print_results(&table, sorted, table.unique, size);
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