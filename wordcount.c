// wordcount.c - Word frequency counter
// Build: gcc -O3 -march=native -std=c11 wordcount.c -o wordcount_c
// Usage: ./wordcount_c [filename]

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum {
    HASH_BITS       = 14,
    HASH_SIZE       = 1u << HASH_BITS,
    HASH_MASK       = HASH_SIZE - 1u,
    MAX_WORD_LEN    = 100,
    READ_BUF_SIZE   = 8192,
    TOP_WORDS       = 100,
};

typedef struct WordNode WordNode;
struct WordNode {
    char *word;
    size_t count;
    WordNode *next;
};

typedef struct {
    const char *word;
    size_t count;
} WordCount;

typedef struct {
    WordNode **buckets;
    size_t total;
    size_t unique;
} WordTable;

static char *str_dup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p)
        memcpy(p, s, len);
    return p;
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static WordTable *table_create(void)
{
    WordTable *t = malloc(sizeof *t);
    if (!t)
        return NULL;

    t->buckets = calloc(HASH_SIZE, sizeof *t->buckets);
    if (!t->buckets) {
        free(t);
        return NULL;
    }

    t->total = 0;
    t->unique = 0;
    return t;
}

static void table_destroy(WordTable *t)
{
    if (!t)
        return;

    for (size_t i = 0; i < HASH_SIZE; i++) {
        WordNode *n = t->buckets[i];
        while (n) {
            WordNode *next = n->next;
            free(n->word);
            free(n);
            n = next;
        }
    }
    free(t->buckets);
    free(t);
}

static int table_insert(WordTable *t, const char *word)
{
    uint32_t idx = fnv1a(word) & HASH_MASK;

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

    n->word = str_dup(word);
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

static size_t extract_word(char *restrict dst, const char *restrict src, size_t pos)
{
    while (src[pos] && !isalpha((unsigned char)src[pos]))
        pos++;

    size_t i = 0;
    while (src[pos] && isalpha((unsigned char)src[pos]) && i < MAX_WORD_LEN - 1)
        dst[i++] = (char)tolower((unsigned char)src[pos++]);

    /* If truncated, skip remainder of the word */
    while (src[pos] && isalpha((unsigned char)src[pos]))
        pos++;

    dst[i] = '\0';
    return pos;
}

static int cmp_by_count_desc(const void *a, const void *b)
{
    const WordCount *wa = a;
    const WordCount *wb = b;

    if (wa->count < wb->count) return 1;
    if (wa->count > wb->count) return -1;
    return strcmp(wa->word, wb->word);
}

static WordCount *table_to_sorted_array(const WordTable *t, size_t *out_len)
{
    WordCount *arr = malloc(t->unique * sizeof *arr);
    if (!arr)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < HASH_SIZE; i++) {
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

static int process_file(WordTable *t, FILE *fp)
{
    char buf[READ_BUF_SIZE];
    char word[MAX_WORD_LEN];

    while (fgets(buf, sizeof buf, fp)) {
        size_t pos = 0;

        while (buf[pos]) {
            pos = extract_word(word, buf, pos);
            if (word[0] && table_insert(t, word) < 0)
                return -1;
        }
    }
    return ferror(fp) ? -1 : 0;
}

static double get_file_size_mb(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_size / (1024.0 * 1024.0) : 0.0;
}

static void fmt_thousands(char *buf, size_t buflen, size_t n)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof tmp, "%zu", n);
    int commas = (len - 1) / 3;
    int outlen = len + commas;

    if ((size_t)outlen >= buflen) {
        snprintf(buf, buflen, "%zu", n);
        return;
    }

    buf[outlen] = '\0';
    int src = len - 1;
    int dst = outlen - 1;
    int digits = 0;

    while (src >= 0) {
        if (digits == 3) {
            buf[dst--] = ',';
            digits = 0;
        }
        buf[dst--] = tmp[src--];
        digits++;
    }
}

static void print_results(const WordTable *t, const WordCount *words,
                          size_t nwords, double elapsed_ms, double size_mb)
{
    char total_str[32], unique_str[32];
    fmt_thousands(total_str, sizeof total_str, t->total);
    fmt_thousands(unique_str, sizeof unique_str, t->unique);

    puts("\n=== Top 10 Most Frequent Words ===");
    for (size_t i = 0; i < 10 && i < nwords; i++) {
        char count_str[32];
        fmt_thousands(count_str, sizeof count_str, words[i].count);
        printf("%2zu. %-15s %9s\n", i + 1, words[i].word, count_str);
    }

    puts("\n=== Statistics ===");
    printf("File size:       %.2f MB\n", size_mb);
    printf("Total words:     %s\n", total_str);
    printf("Unique words:    %s\n", unique_str);
    printf("Execution time:  %.2f ms\n", elapsed_ms);
    printf("Hash table size: %zu buckets\n", (size_t)HASH_SIZE);

    printf("Compiler:        ");
#if defined(__clang__)
    printf("Clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    printf("GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    puts("Unknown");
#endif
}

static int write_results(const char *infile, const WordTable *t,
                         const WordCount *words, size_t nwords, double elapsed_ms)
{
    static const char suffix[] = "_c_results.txt";
    char outpath[256];

    const char *dot = strrchr(infile, '.');
    size_t baselen = dot ? (size_t)(dot - infile) : strlen(infile);

    if (baselen + sizeof suffix > sizeof outpath)
        baselen = sizeof outpath - sizeof suffix;

    snprintf(outpath, sizeof outpath, "%.*s%s", (int)baselen, infile, suffix);

    FILE *fp = fopen(outpath, "w");
    if (!fp) {
        fprintf(stderr, "Warning: cannot write '%s': %s\n", outpath, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    fprintf(fp, "Word Frequency Analysis - C Implementation\n");
    fprintf(fp, "Input file: %s\n", infile);
    fprintf(fp, "Generated: %s", ctime(&now));
    fprintf(fp, "Execution time: %.2f ms\n\n", elapsed_ms);
    fprintf(fp, "Total words: %zu\n", t->total);
    fprintf(fp, "Unique words: %zu\n\n", t->unique);
    fprintf(fp, "Top %d Most Frequent Words:\n", TOP_WORDS);
    fprintf(fp, "%-4s  %-15s %9s %10s\n", "Rank", "Word", "Count", "Percentage");
    fprintf(fp, "----  --------------- --------- ----------\n");

    size_t limit = (nwords < TOP_WORDS) ? nwords : TOP_WORDS;
    for (size_t i = 0; i < limit; i++) {
        double pct = (words[i].count * 100.0) / (double)t->total;
        fprintf(fp, "%4zu  %-15s %9zu %9.2f%%\n",
                i + 1, words[i].word, words[i].count, pct);
    }

    fclose(fp);
    printf("\nResults written to: %s\n", outpath);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *filename = (argc > 1) ? argv[1] : "book.txt";

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filename, strerror(errno));
        fprintf(stderr, "Usage: %s [filename]\n\n", argv[0]);
        fprintf(stderr, "To create a test file:\n");
        fprintf(stderr, "  curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt\n");
        return EXIT_FAILURE;
    }

    printf("Processing file: %s\n", filename);

    WordTable *table = table_create();
    if (!table) {
        fputs("Error: memory allocation failed\n", stderr);
        fclose(fp);
        return EXIT_FAILURE;
    }

    clock_t t0 = clock();
    int err = process_file(table, fp);
    clock_t t1 = clock();
    fclose(fp);

    if (err) {
        fputs("Error: failed to process file\n", stderr);
        table_destroy(table);
        return EXIT_FAILURE;
    }

    if (table->unique == 0) {
        puts("No words found.");
        table_destroy(table);
        return EXIT_SUCCESS;
    }

    size_t nwords;
    WordCount *words = table_to_sorted_array(table, &nwords);
    if (!words) {
        fputs("Error: memory allocation failed\n", stderr);
        table_destroy(table);
        return EXIT_FAILURE;
    }

    double elapsed_ms = ((double)(t1 - t0) / CLOCKS_PER_SEC) * 1000.0;
    double size_mb = get_file_size_mb(filename);

    print_results(table, words, nwords, elapsed_ms, size_mb);
    write_results(filename, table, words, nwords, elapsed_ms);

    free(words);
    table_destroy(table);
    return EXIT_SUCCESS;
}