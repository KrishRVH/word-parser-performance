/*
 * wordcount.c - Word frequency counter
 * Build: gcc -O2 -std=c11 wordcount.c -o wordcount
 * Usage: ./wordcount <filename>
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    HASH_BITS    = 14,              /* 16K buckets */
    HASH_SIZE    = 1u << HASH_BITS,
    MAX_WORD_LEN = 64,
};

typedef struct WordNode WordNode;
struct WordNode {
    char     *word;
    size_t    count;
    WordNode *next;
};

typedef struct {
    const char *word;   /* points into WordTable storage, not owned */
    size_t      count;
} WordCount;

typedef struct {
    WordNode **buckets;
    size_t     nbuckets;
    size_t     total;
    size_t     unique;
} WordTable;

/* --- Hash table --------------------------------------------------------- */

static inline uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static WordTable *word_table_create(void)
{
    WordTable *t = malloc(sizeof *t);
    if (!t)
        return NULL;

    t->buckets = calloc(HASH_SIZE, sizeof *t->buckets);
    if (!t->buckets) {
        free(t);
        return NULL;
    }

    t->nbuckets = HASH_SIZE;
    t->total = t->unique = 0;
    return t;
}

static void word_table_destroy(WordTable *t)
{
    if (!t)
        return;

    for (size_t i = 0; i < t->nbuckets; i++) {
        for (WordNode *n = t->buckets[i], *next; n; n = next) {
            next = n->next;
            free(n->word);
            free(n);
        }
    }
    free(t->buckets);
    free(t);
}

static int word_table_add(WordTable *t, const char *word)
{
    size_t idx = fnv1a(word) & (t->nbuckets - 1u);

    for (WordNode *n = t->buckets[idx]; n; n = n->next) {
        if (strcmp(n->word, word) == 0) {
            n->count++;
            t->total++;
            return 0;
        }
    }

    WordNode *n = malloc(sizeof *n);
    if (!n)
        return -1;

    n->word = strdup(word);
    if (!n->word) {
        free(n);
        return -1;
    }

    n->count = 1;
    n->next = t->buckets[idx];
    t->buckets[idx] = n;
    t->total++;
    t->unique++;
    return 0;
}

static int cmp_by_count_desc(const void *a, const void *b)
{
    const WordCount *wa = a, *wb = b;

    if (wa->count != wb->count)
        return (wa->count < wb->count) ? 1 : -1;

    return strcmp(wa->word, wb->word);
}

static WordCount *word_table_snapshot(const WordTable *t, size_t *out_len)
{
    WordCount *arr = malloc(t->unique * sizeof *arr);
    if (!arr)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < t->nbuckets; i++) {
        for (WordNode *n = t->buckets[i]; n; n = n->next) {
            arr[j].word = n->word;
            arr[j].count = n->count;
            j++;
        }
    }

    qsort(arr, t->unique, sizeof *arr, cmp_by_count_desc);
    *out_len = t->unique;
    return arr;
}

/* --- Tokenizer ---------------------------------------------------------- */

/*
 * Scan forward from p to find the next alphabetic word.
 * - Writes at most (dst_size - 1) lowercased chars into dst.
 * - Always NUL-terminates dst.
 * - Overlong words are truncated; remainder is skipped.
 * - Returns pointer past the word, or NULL if no more words before end.
 */
static const char *next_word(const char *p, const char *end,
                             char *dst, size_t dst_size)
{
    const unsigned char *s = (const unsigned char *)p;
    const unsigned char *e = (const unsigned char *)end;

    while (s < e && !isalpha(*s))
        s++;

    if (s >= e) {
        dst[0] = '\0';
        return NULL;
    }

    size_t i = 0;
    while (s < e && isalpha(*s) && i + 1 < dst_size)
        dst[i++] = (char)tolower(*s++);

    while (s < e && isalpha(*s))
        s++;

    dst[i] = '\0';
    return (const char *)s;
}

static int process_data(WordTable *t, const char *data, size_t len)
{
    const char *p = data;
    const char *end = data + len;
    char word[MAX_WORD_LEN];

    while ((p = next_word(p, end, word, sizeof word))) {
        if (word[0] && word_table_add(t, word) < 0)
            return -1;
    }
    return 0;
}

/* --- Output ------------------------------------------------------------- */

static void print_results(const WordTable *t, const WordCount *words,
                          size_t nwords, size_t file_size)
{
    puts("\n  Rank  Word                 Count      %");
    puts("  ----  ---------------  ---------  -----");

    size_t limit = nwords < 15 ? nwords : 15;
    for (size_t i = 0; i < limit; i++) {
        printf("  %4zu  %-15s  %9zu  %5.2f\n",
               i + 1, words[i].word, words[i].count,
               words[i].count * 100.0 / t->total);
    }

    printf("\n  %zu words, %zu unique (%zu bytes)\n",
           t->total, t->unique, file_size);
}

/* --- Main --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *filename  = argv[1];
    int         fd        = -1;
    void       *map       = MAP_FAILED;
    size_t      file_size = 0;
    WordTable  *table     = NULL;
    WordCount  *words     = NULL;
    struct stat st;
    int         status = EXIT_FAILURE;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open '%s': %s\n", filename, strerror(errno));
        goto out;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Cannot stat '%s': %s\n", filename, strerror(errno));
        goto out;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "File '%s' is empty\n", filename);
        goto out;
    }

    if ((off_t)(size_t)st.st_size != st.st_size) {
        fprintf(stderr, "File '%s' too large for this build\n", filename);
        goto out;
    }

    file_size = (size_t)st.st_size;

    map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "Cannot mmap '%s': %s\n", filename, strerror(errno));
        goto out;
    }

    const char *data = (const char *)map;

    table = word_table_create();
    if (!table) {
        fputs("Memory allocation failed\n", stderr);
        goto out;
    }

    if (process_data(table, data, file_size) < 0) {
        fputs("Processing failed\n", stderr);
        goto out;
    }

    if (table->unique == 0) {
        puts("No words found.");
        status = EXIT_SUCCESS;
        goto out;
    }

    size_t nwords;
    words = word_table_snapshot(table, &nwords);
    if (!words) {
        fputs("Memory allocation failed\n", stderr);
        goto out;
    }

    print_results(table, words, nwords, file_size);
    status = EXIT_SUCCESS;

out:
    free(words);
    word_table_destroy(table);
    if (map != MAP_FAILED)
        munmap(map, file_size);
    if (fd >= 0)
        close(fd);
    return status;
}
