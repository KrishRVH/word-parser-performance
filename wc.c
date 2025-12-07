/*
 * wc.c - word frequency counter
 *
 * DESIGN
 *
 * Hash table with chaining. Words stored inline via flexible array member.
 * Single pass over mmap'd input: lowercase, hash, and insert in one loop.
 * Output sorted by frequency, then alphabetically for ties.
 *
 * Build: cc -O2 -std=c11 wc.c -o wc
 * Usage: ./wc <file>
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct E E;
struct E {
    E *next;
    size_t cnt, h;
    char w[];
};

static struct {
    E **tab;
    size_t cap, n, tot;
    char *mem;
    size_t len;
    int fd;
} G;

/* --- util --- */

static void die(const char *s)
{
    (void)fprintf(stderr, "wc: %s\n", s);
    exit(1);
}

static inline int alpha(unsigned c)
{
    return (c | 32) - 'a' < 26;
}

/* --- table --- */

static void grow(void)
{
    size_t newcap = G.cap ? G.cap * 2 : 4096;
    E **newtab = calloc(newcap, sizeof(E *));
    if (!newtab)
        die("out of memory");

    for (size_t i = 0; i < G.cap; i++) {
        E *e = G.tab[i];
        while (e) {
            E *next = e->next;
            size_t idx = e->h & (newcap - 1);
            e->next = newtab[idx];
            newtab[idx] = e;
            e = next;
        }
    }
    free(G.tab);
    G.tab = newtab;
    G.cap = newcap;
}

static void add(const char *w, size_t len, size_t h)
{
    if (G.n >= G.cap * 7 / 10)
        grow();

    size_t idx = h & (G.cap - 1);
    for (E *e = G.tab[idx]; e; e = e->next) {
        if (e->h == h && !memcmp(e->w, w, len) && !e->w[len]) {
            e->cnt++;
            G.tot++;
            return;
        }
    }

    E *e = malloc(sizeof(E) + len + 1);
    if (!e)
        die("out of memory");
    memcpy(e->w, w, len);
    e->w[len] = '\0';
    e->h = h;
    e->cnt = 1;
    e->next = G.tab[idx];
    G.tab[idx] = e;
    G.n++;
    G.tot++;
}

/* --- scan --- */

static void scan(void)
{
    const unsigned char *s = (const unsigned char *)G.mem;
    const unsigned char *end = s + G.len;
    char buf[256];

    while (s < end) {
        while (s < end && !alpha(*s))
            s++;
        if (s >= end)
            break;

        size_t h = 5381u;
        size_t n = 0;
        while (s < end && alpha(*s)) {
            unsigned c = *s++ | 32;
            if (n < sizeof(buf) - 1)
                buf[n++] = c;
            h = ((h << 5) + h) + c;
        }
        add(buf, n, h);
    }
}

/* --- output --- */

static int cmp(const void *a, const void *b)
{
    const E *x = *(const E **)a;
    const E *y = *(const E **)b;
    if (x->cnt != y->cnt)
        return x->cnt < y->cnt ? 1 : -1;
    return strcmp(x->w, y->w);
}

static void output(void)
{
    E **arr = malloc(G.n * sizeof(E *));
    if (!arr)
        die("out of memory");

    size_t j = 0;
    for (size_t i = 0; i < G.cap; i++)
        for (E *e = G.tab[i]; e; e = e->next)
            arr[j++] = e;

    qsort(arr, G.n, sizeof(E *), cmp);

    size_t top = G.n < 10 ? G.n : 10;
    printf("\n%7s  %-20s  %s\n", "count", "word", "%%");
    printf("-------  --------------------  ------\n");
    for (size_t i = 0; i < top; i++) {
        E *e = arr[i];
        printf("%7zu  %-20s  %5.2f\n", e->cnt, e->w, 100.0 * e->cnt / G.tot);
    }
    printf("\ntotal: %zu words, %zu unique\n", G.tot, G.n);
    free(arr);
}

/* --- main --- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    G.fd = open(argv[1], O_RDONLY);
    if (G.fd < 0)
        die("cannot open file");

    struct stat st;
    if (fstat(G.fd, &st) < 0)
        die("cannot stat file");
    if (st.st_size == 0) {
        puts("empty file");
        return 0;
    }

    G.len = st.st_size;
    G.mem = mmap(NULL, G.len, PROT_READ, MAP_PRIVATE, G.fd, 0);
    if (G.mem == MAP_FAILED)
        die("cannot mmap file");

    scan();
    if (G.n > 0)
        output();

    munmap(G.mem, G.len);
    close(G.fd);
    return 0;
}
