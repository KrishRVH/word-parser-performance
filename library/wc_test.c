/*
** wc_test.c - Test suite
**
** Public domain.
**
** DESIGN
**
**   Comprehensive test coverage including:
**   - Happy path functionality
**   - Edge cases and boundary conditions
**   - OOM injection at every allocation point (SQLite-style torture test)
**   - Regression tests for known bugs
**
** Build:
**   cc -O0 -g wordcount.c wc_test.c -o wc_test
**   cc -O0 -g -DWC_TEST_OOM wordcount.c wc_test.c -o wc_test_oom
**
** OOM HARNESS PORTABILITY NOTE
**
**   The OOM build interposes malloc/realloc using glibc-specific
**   __libc_malloc/__libc_realloc symbols. This is non-portable and
**   technically undefined behavior per strict C99, but is exactly
**   the pattern SQLite and other robust C libraries use in their
**   development test harnesses.
**
**   For portable OOM testing, compile wordcount.c with:
**     -DWC_MALLOC=my_malloc -DWC_FREE=my_free
**   and implement interposition in your test wrapper.
*/

#include "wordcount.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run, g_pass, g_fail;

#define TEST(name)                \
    do                            \
    {                             \
        g_run++;                  \
        printf("  %-45s ", name); \
        (void)fflush(stdout);     \
    } while (0)

#define PASS()            \
    do                    \
    {                     \
        g_pass++;         \
        printf("[OK]\n"); \
    } while (0)
#define FAIL(m)                   \
    do                            \
    {                             \
        g_fail++;                 \
        printf("[FAIL] %s\n", m); \
    } while (0)
#define ASSERT(c)     \
    do                \
    {                 \
        if (!(c))     \
        {             \
            FAIL(#c); \
            return 1; \
        }             \
    } while (0)

/* --- OOM injection framework (glibc-specific) --- */

#ifdef WC_TEST_OOM

static int oom_target = 0;
static int oom_count = 0;
static int oom_active = 0;

static void oom_reset(void)
{
    oom_target = 0;
    oom_count = 0;
    oom_active = 0;
}

static void oom_arm(int n)
{
    oom_target = n;
    oom_count = 0;
    oom_active = 1;
}

static int oom_check(void)
{
    if (!oom_active)
        return 0;
    oom_count++;
    if (oom_count == oom_target)
    {
        oom_active = 0;
        return 1;
    }
    return 0;
}

void *malloc(size_t n)
{
    extern void *__libc_malloc(size_t);
    if (oom_check())
    {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_malloc(n);
}

void *calloc(size_t nm, size_t sz)
{
    /* Overflow-safe multiplication */
    if (nm != 0 && sz > SIZE_MAX / nm)
    {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(nm * sz);
    if (p)
        memset(p, 0, nm * sz);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    extern void *__libc_realloc(void *, size_t);
    if (oom_check())
    {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_realloc(ptr, n);
}

#else

#if defined(__GNUC__) || defined(__clang__)
#define OOM_UNUSED __attribute__((unused))
#else
#define OOM_UNUSED
#endif

OOM_UNUSED static void oom_reset(void)
{
    /* Stub for non-OOM builds */
}

OOM_UNUSED static void oom_arm(int n)
{
    (void)n;
}
#endif

/* ================================================================
** LIFECYCLE TESTS
** ================================================================ */

static int test_open_close(void)
{
    wc *w;
    TEST("open and close");
    w = wc_open(0);
    ASSERT(w != NULL);
    ASSERT(wc_total(w) == 0);
    ASSERT(wc_unique(w) == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_close_null(void)
{
    TEST("close NULL");
    wc_close(NULL);
    PASS();
    return 0;
}

static int test_max_word_clamp(void)
{
    wc *w;
    TEST("max_word clamping");
    w = wc_open(1);
    ASSERT(w != NULL);
    wc_close(w);
    w = wc_open(9999);
    ASSERT(w != NULL);
    wc_close(w);
    PASS();
    return 0;
}

/* ================================================================
** wc_add TESTS
** ================================================================ */

static int test_add_single(void)
{
    wc *w;
    TEST("add single");
    w = wc_open(0);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_total(w) == 1);
    ASSERT(wc_unique(w) == 1);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_dup(void)
{
    wc *w;
    TEST("add duplicate");
    w = wc_open(0);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_total(w) == 3);
    ASSERT(wc_unique(w) == 1);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_multi(void)
{
    wc *w;
    TEST("add multiple");
    w = wc_open(0);
    ASSERT(wc_add(w, "apple") == WC_OK);
    ASSERT(wc_add(w, "banana") == WC_OK);
    ASSERT(wc_add(w, "cherry") == WC_OK);
    ASSERT(wc_total(w) == 3);
    ASSERT(wc_unique(w) == 3);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_empty(void)
{
    wc *w;
    TEST("add empty string");
    w = wc_open(0);
    ASSERT(wc_add(w, "") == WC_OK);
    ASSERT(wc_total(w) == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_null(void)
{
    wc *w;
    TEST("add NULL args");
    ASSERT(wc_add(NULL, "x") == WC_ERROR);
    w = wc_open(0);
    ASSERT(wc_add(w, NULL) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_trunc(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    TEST("add truncation");
    w = wc_open(4);
    ASSERT(wc_add(w, "abcdefghij") == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "abcd") == 0);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_trunc_collision(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    TEST("add truncation collision");
    w = wc_open(4);
    ASSERT(wc_add(w, "testing") == WC_OK);
    ASSERT(wc_add(w, "tested") == WC_OK);
    ASSERT(wc_add(w, "tester") == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "test") == 0);
    ASSERT(r[0].count == 3);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

/* ================================================================
** wc_scan TESTS
** ================================================================ */

static int test_scan_simple(void)
{
    wc *w;
    const char *t = "Hello World";
    TEST("scan simple");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_total(w) == 2);
    ASSERT(wc_unique(w) == 2);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_case(void)
{
    wc *w;
    const char *t = "Hello HELLO hello HeLLo";
    TEST("scan case folding");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_total(w) == 4);
    ASSERT(wc_unique(w) == 1);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_punct(void)
{
    wc *w;
    const char *t = "hello, world! how's it going?";
    TEST("scan punctuation");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_total(w) == 6);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_numbers(void)
{
    wc *w;
    const char *t = "abc123def 456 ghi";
    TEST("scan numbers");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_unique(w) == 3);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_empty(void)
{
    wc *w;
    TEST("scan empty");
    w = wc_open(0);
    ASSERT(wc_scan(w, "", 0) == WC_OK);
    ASSERT(wc_scan(w, NULL, 0) == WC_OK);
    ASSERT(wc_total(w) == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_nowords(void)
{
    wc *w;
    const char *t = "12345!@#$%";
    TEST("scan no words");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_total(w) == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_null(void)
{
    wc *w;
    TEST("scan NULL args");
    ASSERT(wc_scan(NULL, "x", 1) == WC_ERROR);
    w = wc_open(0);
    ASSERT(wc_scan(w, NULL, 100) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_trunc(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    const char *t = "abcdefghij";
    TEST("scan truncation");
    w = wc_open(4);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "abcd") == 0);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_trunc_collision(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    const char *t = "internationalization internationally international";
    TEST("scan truncation collision");
    w = wc_open(8);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "internat") == 0);
    ASSERT(r[0].count == 3);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_binary(void)
{
    wc *w;
    const char t[] = "hello\0world\0test";
    TEST("scan with embedded NUL");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, sizeof(t) - 1) == WC_OK);
    ASSERT(wc_total(w) == 3);
    ASSERT(wc_unique(w) == 3);
    wc_close(w);
    PASS();
    return 0;
}

/* ================================================================
** wc_results TESTS
** ================================================================ */

static int test_results_sorted(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    const char *t = "apple banana apple cherry apple banana";
    TEST("results sorted");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 3);
    ASSERT(strcmp(r[0].word, "apple") == 0 && r[0].count == 3);
    ASSERT(strcmp(r[1].word, "banana") == 0 && r[1].count == 2);
    ASSERT(strcmp(r[2].word, "cherry") == 0 && r[2].count == 1);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_alpha(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    const char *t = "zebra apple mango";
    TEST("results alpha tiebreak");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(strcmp(r[0].word, "apple") == 0);
    ASSERT(strcmp(r[1].word, "mango") == 0);
    ASSERT(strcmp(r[2].word, "zebra") == 0);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_empty(void)
{
    wc *w;
    wc_word *r = (wc_word *)1;
    size_t n = 999;
    TEST("results empty");
    w = wc_open(0);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(r == NULL && n == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_null(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    TEST("results NULL args");
    w = wc_open(0);
    ASSERT(wc_results(NULL, &r, &n) == WC_ERROR);
    ASSERT(wc_results(w, NULL, &n) == WC_ERROR);
    ASSERT(wc_results(w, &r, NULL) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_free_null(void)
{
    TEST("results_free NULL");
    wc_results_free(NULL);
    PASS();
    return 0;
}

/* ================================================================
** QUERY TESTS
** ================================================================ */

static int test_query_null(void)
{
    TEST("query NULL");
    ASSERT(wc_total(NULL) == 0);
    ASSERT(wc_unique(NULL) == 0);
    PASS();
    return 0;
}

static int test_version(void)
{
    const char *v;
    TEST("version");
    v = wc_version();
    ASSERT(v && strlen(v) > 0);
    ASSERT(strcmp(v, WC_VERSION) == 0);
    PASS();
    return 0;
}

/* ================================================================
** STRESS TESTS
** ================================================================ */

static int test_many_unique(void)
{
    wc *w;
    char word[32];
    size_t i;
    size_t n = 10000;
    TEST("many unique");
    w = wc_open(0);
    for (i = 0; i < n; i++)
    {
        (void)snprintf(word, sizeof word, "word%zu", i);
        ASSERT(wc_add(w, word) == WC_OK);
    }
    ASSERT(wc_total(w) == n);
    ASSERT(wc_unique(w) == n);
    wc_close(w);
    PASS();
    return 0;
}

static int test_many_dup(void)
{
    wc *w;
    size_t i;
    size_t n = 100000;
    TEST("many duplicates");
    w = wc_open(0);
    for (i = 0; i < n; i++)
        ASSERT(wc_add(w, "same") == WC_OK);
    ASSERT(wc_total(w) == n);
    ASSERT(wc_unique(w) == 1);
    wc_close(w);
    PASS();
    return 0;
}

static int test_growth(void)
{
    wc *w;
    char word[32];
    size_t i;
    size_t n = 5000;
    wc_word *r;
    size_t len;
    TEST("table growth");
    w = wc_open(0);
    for (i = 0; i < n; i++)
    {
        (void)snprintf(word, sizeof word, "w%zu", i);
        ASSERT(wc_add(w, word) == WC_OK);
        ASSERT(wc_add(w, word) == WC_OK);
    }
    ASSERT(wc_unique(w) == n);
    ASSERT(wc_results(w, &r, &len) == WC_OK);
    for (i = 0; i < len; i++)
        ASSERT(r[i].count == 2);
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_arena_blocks(void)
{
    wc *w;
    wc_word *r;
    size_t len;
    size_t i;
    size_t n = 50000;
    char word[32];
    TEST("arena block chain");
    w = wc_open(0);
    for (i = 0; i < n; i++)
    {
        (void)snprintf(word, sizeof word, "word%05zu", i);
        ASSERT(wc_add(w, word) == WC_OK);
    }
    ASSERT(wc_results(w, &r, &len) == WC_OK);
    ASSERT(len == n);
    for (i = 0; i < len; i++)
    {
        ASSERT(r[i].word != NULL);
        ASSERT(strlen(r[i].word) > 0);
    }
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

/* ================================================================
** OOM INJECTION TESTS (SQLite-style torture testing)
** ================================================================ */

#ifdef WC_TEST_OOM

static int test_oom_open(void)
{
    wc *w;
    int i;
    TEST("OOM in wc_open");
    for (i = 1; i <= 10; i++)
    {
        oom_arm(i);
        w = wc_open(0);
        if (w)
            wc_close(w);
        oom_reset();
    }
    w = wc_open(0);
    ASSERT(w != NULL);
    wc_close(w);
    PASS();
    return 0;
}

static int test_oom_add(void)
{
    wc *w;
    int i, rc;
    TEST("OOM in wc_add");
    for (i = 1; i <= 20; i++)
    {
        w = wc_open(0);
        ASSERT(w != NULL);
        oom_arm(i);
        rc = wc_add(w, "testword");
        oom_reset();
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_oom_scan(void)
{
    wc *w;
    const char *t = "the quick brown fox jumps over the lazy dog";
    int i, rc;
    TEST("OOM in wc_scan");
    for (i = 1; i <= 30; i++)
    {
        w = wc_open(0);
        ASSERT(w != NULL);
        oom_arm(i);
        rc = wc_scan(w, t, strlen(t));
        oom_reset();
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_oom_results(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    int i, rc;
    TEST("OOM in wc_results");
    for (i = 1; i <= 10; i++)
    {
        w = wc_open(0);
        ASSERT(wc_add(w, "hello") == WC_OK);
        ASSERT(wc_add(w, "world") == WC_OK);
        oom_arm(i);
        rc = wc_results(w, &r, &n);
        oom_reset();
        if (rc == WC_OK)
            wc_results_free(r);
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_oom_growth(void)
{
    wc *w;
    char word[32];
    size_t i;
    int rc;
    TEST("OOM during table growth");
    w = wc_open(0);
    for (i = 0; i < 5000; i++)
    {
        snprintf(word, sizeof word, "word%zu", i);
        if (i == 3000)
            oom_arm(5);
        rc = wc_add(w, word);
        if (rc == WC_NOMEM)
            break;
    }
    oom_reset();
    ASSERT(wc_unique(w) > 0);
    ASSERT(wc_add(w, "recovery") == WC_OK);
    wc_close(w);
    PASS();
    return 0;
}

static int test_oom_torture(void)
{
    const char *text = "alpha beta gamma delta epsilon alpha beta gamma";
    int i, max_allocs = 50;
    TEST("OOM torture (all injection points)");
    for (i = 1; i <= max_allocs; i++)
    {
        wc *w;
        wc_word *r = NULL;
        size_t n = 0;
        int rc;

        oom_arm(i);
        w = wc_open(0);
        if (!w)
        {
            oom_reset();
            continue;
        }

        rc = wc_scan(w, text, strlen(text));
        if (rc == WC_NOMEM)
        {
            wc_close(w);
            oom_reset();
            continue;
        }

        rc = wc_results(w, &r, &n);
        if (rc == WC_OK)
            wc_results_free(r);

        wc_close(w);
        oom_reset();
    }
    PASS();
    return 0;
}

#endif /* WC_TEST_OOM */

/* ================================================================
** MAIN
** ================================================================ */

int main(void)
{
    printf("\n=== Wordcount Tests (v%s) ===\n\n", wc_version());

    printf("Lifecycle:\n");
    test_open_close();
    test_close_null();
    test_max_word_clamp();

    printf("\nwc_add:\n");
    test_add_single();
    test_add_dup();
    test_add_multi();
    test_add_empty();
    test_add_null();
    test_add_trunc();
    test_add_trunc_collision();

    printf("\nwc_scan:\n");
    test_scan_simple();
    test_scan_case();
    test_scan_punct();
    test_scan_numbers();
    test_scan_empty();
    test_scan_nowords();
    test_scan_null();
    test_scan_trunc();
    test_scan_trunc_collision();
    test_scan_binary();

    printf("\nwc_results:\n");
    test_results_sorted();
    test_results_alpha();
    test_results_empty();
    test_results_null();
    test_results_free_null();

    printf("\nQueries:\n");
    test_query_null();
    test_version();

    printf("\nStress:\n");
    test_many_unique();
    test_many_dup();
    test_growth();
    test_arena_blocks();

#ifdef WC_TEST_OOM
    printf("\nOOM Injection (glibc-specific):\n");
    test_oom_open();
    test_oom_add();
    test_oom_scan();
    test_oom_results();
    test_oom_growth();
    test_oom_torture();
#else
    printf("\nOOM: skipped (build with -DWC_TEST_OOM on glibc)\n");
#endif

    printf("\n=== %d/%d passed", g_pass, g_run);
    if (g_fail)
        printf(", %d FAILED", g_fail);
    printf(" ===\n\n");

    return g_fail ? 1 : 0;
}
