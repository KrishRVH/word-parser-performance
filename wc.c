/*
 * wc.c - Word frequency counter
 *
 * Build:
 *   gcc -O2 -std=c11 -Wall -Wextra wc.c -o wc
 *
 * Usage:
 *   ./wc <file>
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
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Constants and Basic Utilities                                             */
/* ------------------------------------------------------------------------- */

enum
{
    INITIAL_CAPACITY = 1024, /* must be power of two */
    MAX_WORD = 256,
    TOP_N = 10
};

_Static_assert((INITIAL_CAPACITY & (INITIAL_CAPACITY - 1)) == 0,
               "INITIAL_CAPACITY must be a power of two");

static const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME = 0x100000001b3ULL;

static inline int is_letter_ascii(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline unsigned char to_lower_ascii(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static void die_errno(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void die_msg(const char *msg)
{
    (void)fputs(msg, stderr);
    (void)fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

/* ------------------------------------------------------------------------- */
/* Hash Map of Word -> Count (Open Addressing)                               */
/* ------------------------------------------------------------------------- */

typedef struct
{
    uint64_t hash;
    char *word;
    size_t count;
} Entry;

typedef struct
{
    Entry *slots;
    size_t capacity;  /* always a power of two */
    size_t mask;      /* capacity - 1 */
    size_t size;      /* number of occupied entries (unique words) */
    size_t threshold; /* capacity * load_factor */
    size_t total;     /* total words (including repeats) */
} HashMap;

static size_t next_power_of_two(size_t n)
{
    if (n < 2)
        return 2;

    n--;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        n |= n >> shift;
    return n + 1;
}

static int hash_map_init(HashMap *map, size_t initial_capacity)
{
    if (!map)
        return -1;

    size_t cap = next_power_of_two(initial_capacity);
    Entry *slots = calloc(cap, sizeof *slots);
    if (!slots)
        return -1;

    map->slots = slots;
    map->capacity = cap;
    map->mask = cap - 1;
    map->size = 0;
    map->total = 0;

    const double load_factor = 0.75;
    map->threshold = (size_t)(cap * load_factor);

    return 0;
}

static void hash_map_destroy(HashMap *map)
{
    if (!map || !map->slots)
        return;

    for (size_t i = 0; i < map->capacity; i++)
    {
        free(map->slots[i].word);
    }
    free(map->slots);
    map->slots = NULL;
    map->capacity = 0;
    map->mask = 0;
    map->size = 0;
    map->total = 0;
    map->threshold = 0;
}

static int hash_map_resize(HashMap *map, size_t new_capacity)
{
    size_t cap = next_power_of_two(new_capacity);
    Entry *slots = calloc(cap, sizeof *slots);
    if (!slots)
        return -1;

    size_t mask = cap - 1;

    for (size_t i = 0; i < map->capacity; i++)
    {
        Entry old = map->slots[i];
        if (!old.word)
            continue;

        size_t idx = old.hash & mask;
        while (slots[idx].word)
            idx = (idx + 1) & mask;
        slots[idx] = old;
    }

    free(map->slots);
    map->slots = slots;
    map->capacity = cap;
    map->mask = mask;

    const double load_factor = 0.75;
    map->threshold = (size_t)(cap * load_factor);

    return 0;
}

static char *dup_word(const char *word, size_t len)
{
    char *s = malloc(len + 1);
    if (!s)
        return NULL;
    memcpy(s, word, len);
    s[len] = '\0';
    return s;
}

static int
hash_map_upsert(HashMap *map, const char *word, size_t len, uint64_t hash)
{
    if (map->size >= map->threshold)
        if (hash_map_resize(map, map->capacity * 2) < 0)
            return -1;

    size_t idx = hash & map->mask;

    for (;;)
    {
        Entry *e = &map->slots[idx];

        if (!e->word)
        {
            e->hash = hash;
            e->count = 1;
            e->word = dup_word(word, len);
            if (!e->word)
                return -1;

            map->size++;
            map->total++;
            return 0;
        }

        if (e->hash == hash && strcmp(e->word, word) == 0)
        {
            e->count++;
            map->total++;
            return 0;
        }

        idx = (idx + 1) & map->mask;
    }
}

/* ------------------------------------------------------------------------- */
/* Word Scanning                                                             */
/* ------------------------------------------------------------------------- */

static int count_words(HashMap *map, const char *data, size_t length)
{
    const char *p = data;
    const char *end = data + length;
    char buf[MAX_WORD];

    while (p < end)
    {
        /* Skip non-letters */
        while (p < end && !is_letter_ascii((unsigned char)*p))
            p++;

        if (p >= end)
            break;

        char *out = buf;
        uint64_t hash = FNV_OFFSET;

        while (p < end && is_letter_ascii((unsigned char)*p))
        {
            unsigned char c = to_lower_ascii((unsigned char)*p);
            if ((size_t)(out - buf) < MAX_WORD - 1)
                *out++ = (char)c;

            hash ^= c;
            hash *= FNV_PRIME;
            p++;
        }

        size_t len = (size_t)(out - buf);
        if (len == 0)
            continue;

        buf[len] = '\0';

        if (hash_map_upsert(map, buf, len, hash) < 0)
            return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Sorting and Output                                                        */
/* ------------------------------------------------------------------------- */

static int cmp_entry_desc(const void *a, const void *b)
{
    const Entry *ea = a;
    const Entry *eb = b;

    if (ea->count < eb->count)
        return 1;
    if (ea->count > eb->count)
        return -1;
    return strcmp(ea->word, eb->word);
}

static Entry *collect_entries(const HashMap *map, size_t *out_count)
{
    if (!map || !out_count)
        return NULL;

    if (map->size == 0)
    {
        *out_count = 0;
        return NULL;
    }

    Entry *dense = malloc(map->size * sizeof *dense);
    if (!dense)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < map->capacity; i++)
    {
        if (!map->slots[i].word)
            continue;
        dense[j++] = map->slots[i];
    }

    *out_count = j;
    return dense;
}

static void print_results(const HashMap *map)
{
    if (!map || map->size == 0)
    {
        puts("No words found.");
        return;
    }

    size_t n_entries = 0;
    Entry *dense = collect_entries(map, &n_entries);
    if (!dense)
        die_msg("out of memory while collecting entries");

    qsort(dense, n_entries, sizeof *dense, cmp_entry_desc);

    printf("\n%7s %-16s %s\n", "Count", "Word", "%");
    printf("------- ---------------- -----\n");

    size_t limit = (n_entries < TOP_N) ? n_entries : TOP_N;
    for (size_t i = 0; i < limit; i++)
    {
        const Entry *e = &dense[i];
        double pct = (map->total == 0)
                             ? 0.0
                             : (100.0 * (double)e->count / (double)map->total);

        printf("%7zu %-16s %.2f\n", e->count, e->word, pct);
    }

    printf("\nTotal: %zu words, %zu unique\n", map->total, map->size);

    free(dense);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        (void)fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[1];
    int fd = -1;
    void *map_addr = MAP_FAILED;
    size_t size = 0;
    HashMap map = { 0 };
    int rc = EXIT_FAILURE;

    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0)
    {
        perror("open");
        goto out;
    }

    if (fstat(fd, &st) < 0)
    {
        perror("fstat");
        goto out;
    }

    if (st.st_size == 0)
    {
        puts("Empty file.");
        rc = EXIT_SUCCESS;
        goto out;
    }

    size = (size_t)st.st_size;

    map_addr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_addr == MAP_FAILED)
    {
        perror("mmap");
        goto out;
    }

    (void)posix_madvise(map_addr, size, POSIX_MADV_SEQUENTIAL);

    if (hash_map_init(&map, INITIAL_CAPACITY) < 0)
    {
        (void)fputs("out of memory initializing hash map\n", stderr);
        goto out;
    }

    if (count_words(&map, map_addr, size) < 0)
    {
        (void)fputs("out of memory while counting words\n", stderr);
        goto out;
    }

    print_results(&map);
    rc = EXIT_SUCCESS;

out:
    hash_map_destroy(&map);

    if (map_addr != MAP_FAILED)
        munmap(map_addr, size);

    if (fd >= 0)
        close(fd);

    return rc;
}
