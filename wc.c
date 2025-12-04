/*
 * wc.c - High-performance word frequency counter
 * Build: gcc -O3 -std=c11 -march=native -Wall -Wextra wc.c -o wc
 * Note:  Linux/POSIX specific (mmap, madvise)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --- Tuning & Constants ------------------------------------------------- */

#define HASH_LOAD_FACTOR 0.75
#define PAGE_SIZE 4096u
#define ARENA_BLOCK_SIZE (1u << 22) /* 4MB chunks */
#define FNV_OFFSET 0xcbf29ce484222325UL
#define FNV_PRIME 0x100000001b3UL

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* --- Data Types --------------------------------------------------------- */

typedef struct
{
    uint64_t hash;
    char *word;
    size_t count;
} Entry;

typedef struct Block
{
    struct Block *prev;
    char *ptr;
    char *end;
    alignas(64) char data[];
} Block;

typedef struct
{
    Block *current;
} Arena;

typedef struct
{
    Entry *slots;
    size_t mask;
    size_t count;
    size_t threshold;
    Arena *arena;
} Map;

/* --- Globals (Lookup Table) --------------------------------------------- */

static uint8_t g_props[256]; /* 0 = non-alpha, >0 = lowercase char */

static void init_lut(void)
{
    memset(g_props, 0, sizeof(g_props));
    for (int i = 0; i < 256; i++)
    {
        unsigned char c = (unsigned char)i;
        if (c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        if (c >= 'a' && c <= 'z')
            g_props[i] = c;
    }
}

/* --- mmap Helper -------------------------------------------------------- */

static void *xmap(size_t bytes)
{
    void *p = mmap(NULL,
                   bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (p == MAP_FAILED)
    {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    return p;
}

/* --- Arena Allocator ---------------------------------------------------- */

static void *arena_alloc(Arena *a, size_t size)
{
    size = (size + 7u) & ~7u; /* 8-byte alignment */

    Block *b = a->current;
    if (unlikely(!b || b->ptr + size > b->end))
    {
        size_t want = size + sizeof(Block);
        size_t block_sz = want < ARENA_BLOCK_SIZE ? ARENA_BLOCK_SIZE : want;
        block_sz = (block_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        b = xmap(block_sz);
        b->prev = a->current;
        b->ptr = b->data;
        b->end = (char *)b + block_sz;
        a->current = b;
    }

    void *ptr = b->ptr;
    b->ptr += size;
    return ptr;
}

/* --- Hash Map (Open Addressing / Linear Probing) ------------------------ */

static void map_resize(Map *map);

static void map_upsert(Map *restrict map,
                       uint64_t hash,
                       const char *restrict str,
                       size_t len)
{
    if (unlikely(map->count >= map->threshold))
        map_resize(map);

    size_t idx = hash & map->mask;

    for (;;)
    {
        Entry *e = &map->slots[idx];

        if (!e->word)
        {
            e->hash = hash;
            e->count = 1;
            e->word = arena_alloc(map->arena, len + 1);
            memcpy(e->word, str, len);
            e->word[len] = '\0';
            map->count++;
            return;
        }

        if (e->hash == hash && strcmp(e->word, str) == 0)
        {
            e->count++;
            return;
        }

        idx = (idx + 1) & map->mask;
    }
}

static void map_resize(Map *map)
{
    size_t new_cap = map->slots ? (map->mask + 1) * 2 : 1024;
    size_t bytes = new_cap * sizeof(Entry);

    Entry *new_slots = xmap(bytes);
    memset(new_slots, 0, bytes);

    size_t new_mask = new_cap - 1;

    if (map->slots)
    {
        for (size_t i = 0; i <= map->mask; i++)
        {
            Entry *old = &map->slots[i];
            if (!old->word)
                continue;

            size_t idx = old->hash & new_mask;
            while (new_slots[idx].word)
            {
                idx = (idx + 1) & new_mask;
            }
            new_slots[idx] = *old;
        }
        munmap(map->slots, (map->mask + 1) * sizeof(Entry));
    }

    map->slots = new_slots;
    map->mask = new_mask;
    map->threshold = (size_t)(new_cap * HASH_LOAD_FACTOR);
}

/* --- Sorting Comparator ------------------------------------------------- */

static int cmp_desc(const void *a, const void *b)
{
    const Entry *ea = a;
    const Entry *eb = b;

    if (ea->count < eb->count)
        return 1;
    if (ea->count > eb->count)
        return -1;
    return strcmp(ea->word, eb->word);
}

/* --- Core --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        (void)fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    init_lut();

    /* 1. Map File */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) || !st.st_size)
    {
        close(fd);
        return 0;
    }

    int flags = MAP_PRIVATE | MAP_POPULATE;
    char *data = mmap(NULL, (size_t)st.st_size, PROT_READ, flags, fd, 0);
    if (data == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return 1;
    }

    madvise(data, (size_t)st.st_size, MADV_SEQUENTIAL);

    /* 2. Process */
    Arena arena = (Arena){ 0 };
    Map map = { .arena = &arena };

    char *ptr = data;
    char *end = data + st.st_size;
    char buf[256];

    while (ptr < end)
    {
        /* Skip non-alpha quickly */
        while (ptr < end && !g_props[(uint8_t)*ptr])
        {
            ptr++;
        }
        if (ptr == end)
            break;

        char *out = buf;
        uint64_t hash = FNV_OFFSET;

        while (ptr < end)
        {
            uint8_t c = g_props[(uint8_t)*ptr];
            if (!c)
                break;

            if (likely((size_t)(out - buf) < sizeof buf - 1))
            {
                *out++ = (char)c;
            }
            hash = (hash ^ c) * FNV_PRIME;
            ptr++;
        }

        size_t len = (size_t)(out - buf);
        *out = '\0';

        if (len)
            map_upsert(&map, hash, buf, len);
    }

    /* 3. Sort & Print */
    Entry *dense = NULL;
    size_t total_words = 0;

    if (map.count)
    {
        dense = xmap(map.count * sizeof(Entry));

        size_t j = 0;
        for (size_t i = 0; i <= map.mask; i++)
        {
            if (!map.slots[i].word)
                continue;
            dense[j] = map.slots[i];
            total_words += dense[j].count;
            j++;
        }

        qsort(dense, map.count, sizeof(Entry), cmp_desc);

        printf("\n%7s %-16s %s\n", "Count", "Word", "%");
        printf("------- ---------------- -----\n");

        size_t limit = map.count < 10 ? map.count : 10;
        for (size_t i = 0; i < limit; i++)
        {
            printf("%7zu %-16s %.2f\n",
                   dense[i].count,
                   dense[i].word,
                   (100.0 * dense[i].count) / (double)total_words);
        }
    }

    printf("\nTotal: %zu words, %zu unique\n", total_words, map.count);

    printf("\nTotal: %zu words, %zu unique\n", total_words, map.count);

    /* Cleanup */
    if (dense)
    {
        munmap(dense, map.count * sizeof(Entry));
    }
    if (map.slots)
    {
        munmap(map.slots, (map.mask + 1) * sizeof(Entry));
    }

    munmap(data, (size_t)st.st_size);
    close(fd);

    while (arena.current)
    {
        Block *prev = arena.current->prev;
        size_t sz = (size_t)(arena.current->end - (char *)arena.current);
        munmap(arena.current, sz);
        arena.current = prev;
    }

    return 0;
}
