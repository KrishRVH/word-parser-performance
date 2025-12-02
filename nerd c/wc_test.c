/**
 * @file wc_test.c
 * @brief Unit tests for the wordcount library.
 *
 * A minimal test harness demonstrating library usage and verifying
 * correctness. Uses assert() for simplicity; production code would
 * use a proper testing framework.
 *
 * Compile: gcc -std=c11 -Wall -Wextra -O2 wordcount.c wc_test.c -o wc_test
 * Run:     ./wc_test
 */

#include "wordcount.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*===========================================================================
 * Test utilities
 *===========================================================================*/

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                 \
    do                             \
    {                              \
        ++tests_run;               \
        printf("  %-50s ", #name); \
        fflush(stdout);            \
    } while (0)

#define PASS()              \
    do                      \
    {                       \
        ++tests_passed;     \
        printf("[PASS]\n"); \
    } while (0)

#define FAIL(msg)                   \
    do                              \
    {                               \
        printf("[FAIL] %s\n", msg); \
    } while (0)

/*===========================================================================
 * Test cases
 *===========================================================================*/

static void test_create_destroy(void)
{
    TEST(create_destroy_null_config);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);
        assert(wc_total_words(t) == 0);
        assert(wc_unique_words(t) == 0);
        wc_destroy(t);
        PASS();
    }

    TEST(create_with_config);
    {
        wc_config cfg = { .initial_capacity = 128, .max_word_length = 32 };
        wc_table *t = wc_create(&cfg);
        assert(t != NULL);
        wc_destroy(t);
        PASS();
    }

    TEST(destroy_null_safe);
    {
        wc_destroy(NULL); /* Should not crash. */
        PASS();
    }
}

static void test_add_word(void)
{
    TEST(add_single_word);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_add_word(t, "hello");
        assert(st == WC_OK);
        assert(wc_total_words(t) == 1);
        assert(wc_unique_words(t) == 1);

        wc_destroy(t);
        PASS();
    }

    TEST(add_duplicate_word);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        assert(wc_add_word(t, "hello") == WC_OK);
        assert(wc_add_word(t, "hello") == WC_OK);
        assert(wc_add_word(t, "hello") == WC_OK);

        assert(wc_total_words(t) == 3);
        assert(wc_unique_words(t) == 1);

        wc_destroy(t);
        PASS();
    }

    TEST(add_multiple_words);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        assert(wc_add_word(t, "apple") == WC_OK);
        assert(wc_add_word(t, "banana") == WC_OK);
        assert(wc_add_word(t, "cherry") == WC_OK);
        assert(wc_add_word(t, "apple") == WC_OK);

        assert(wc_total_words(t) == 4);
        assert(wc_unique_words(t) == 3);

        wc_destroy(t);
        PASS();
    }

    TEST(add_word_null_table);
    {
        wc_status st = wc_add_word(NULL, "hello");
        assert(st == WC_ERR_INVALID_ARGUMENT);
        PASS();
    }

    TEST(add_word_null_word);
    {
        wc_table *t = wc_create(NULL);
        wc_status st = wc_add_word(t, NULL);
        assert(st == WC_ERR_INVALID_ARGUMENT);
        wc_destroy(t);
        PASS();
    }
}

static void test_process_text(void)
{
    TEST(process_simple_text);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "Hello, World!";
        wc_status st = wc_process_text(t, text, strlen(text));

        assert(st == WC_OK);
        assert(wc_total_words(t) == 2);
        assert(wc_unique_words(t) == 2);

        wc_destroy(t);
        PASS();
    }

    TEST(process_case_insensitive);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "Hello HELLO hello HeLLo";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        assert(wc_total_words(t) == 4);
        assert(wc_unique_words(t) == 1); /* All "hello" variants. */

        wc_destroy(t);
        PASS();
    }

    TEST(process_empty_text);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_process_text(t, "", 0);
        assert(st == WC_OK);
        assert(wc_total_words(t) == 0);

        st = wc_process_text(t, NULL, 0);
        assert(st == WC_OK);

        wc_destroy(t);
        PASS();
    }

    TEST(process_punctuation_only);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "!@#$%^&*()123456789";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        assert(wc_total_words(t) == 0);
        assert(wc_unique_words(t) == 0);

        wc_destroy(t);
        PASS();
    }

    TEST(process_null_data_nonzero_len);
    {
        wc_table *t = wc_create(NULL);
        wc_status st = wc_process_text(t, NULL, 100);
        assert(st == WC_ERR_INVALID_ARGUMENT);
        wc_destroy(t);
        PASS();
    }
}

static void test_snapshot(void)
{
    TEST(snapshot_sorted_by_count);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "apple banana apple cherry apple banana";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries = NULL;
        size_t count = 0;
        wc_status st = wc_snapshot(t, &entries, &count);

        assert(st == WC_OK);
        assert(count == 3);
        assert(entries != NULL);

        /* Should be sorted: apple(3), banana(2), cherry(1). */
        assert(strcmp(entries[0].word, "apple") == 0);
        assert(entries[0].count == 3);

        assert(strcmp(entries[1].word, "banana") == 0);
        assert(entries[1].count == 2);

        assert(strcmp(entries[2].word, "cherry") == 0);
        assert(entries[2].count == 1);

        wc_free_snapshot(entries);
        wc_destroy(t);
        PASS();
    }

    TEST(snapshot_ties_sorted_alphabetically);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "zebra apple mango";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries = NULL;
        size_t count = 0;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);

        /* All have count 1, should be sorted alphabetically. */
        assert(strcmp(entries[0].word, "apple") == 0);
        assert(strcmp(entries[1].word, "mango") == 0);
        assert(strcmp(entries[2].word, "zebra") == 0);

        wc_free_snapshot(entries);
        wc_destroy(t);
        PASS();
    }

    TEST(snapshot_empty_table);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_entry *entries = (wc_entry *)0xDEADBEEF; /* Sentinel. */
        size_t count = 999;
        wc_status st = wc_snapshot(t, &entries, &count);

        assert(st == WC_OK);
        assert(entries == NULL);
        assert(count == 0);

        wc_destroy(t);
        PASS();
    }

    TEST(free_snapshot_null_safe);
    {
        wc_free_snapshot(NULL); /* Should not crash. */
        PASS();
    }
}

static void test_query_functions(void)
{
    TEST(total_words_null_table);
    {
        assert(wc_total_words(NULL) == 0);
        PASS();
    }

    TEST(unique_words_null_table);
    {
        assert(wc_unique_words(NULL) == 0);
        PASS();
    }
}

static void test_version(void)
{
    TEST(version_string);
    {
        const char *version = wc_version();
        assert(version != NULL);
        assert(strlen(version) > 0);
        /* Should look like "X.Y.Z" */
        assert(strchr(version, '.') != NULL);
        PASS();
    }
}

static void test_stress(void)
{
    TEST(many_unique_words);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        char word[16];
        const int n = 10000;

        for (int i = 0; i < n; ++i)
        {
            snprintf(word, sizeof(word), "word%d", i);
            wc_status st = wc_add_word(t, word);
            assert(st == WC_OK);
        }

        assert(wc_total_words(t) == (size_t)n);
        assert(wc_unique_words(t) == (size_t)n);

        wc_destroy(t);
        PASS();
    }

    TEST(many_duplicates);
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const int n = 100000;
        for (int i = 0; i < n; ++i)
        {
            assert(wc_add_word(t, "same") == WC_OK);
        }

        assert(wc_total_words(t) == (size_t)n);
        assert(wc_unique_words(t) == 1);

        wc_entry *entries = NULL;
        size_t count = 0;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);

        assert(count == 1);
        assert(entries[0].count == (size_t)n);

        wc_free_snapshot(entries);
        wc_destroy(t);
        PASS();
    }

    TEST(word_truncation);
    {
        /* Test that words longer than max_word_length are truncated. */
        wc_config cfg = { .max_word_length = 8 }; /* 7 chars + NUL */
        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        const char *text = "abcdefghijk"; /* 11 letters */
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        /* Should be truncated to "abcdefg" (7 chars). */
        wc_entry *entries = NULL;
        size_t count = 0;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);
        assert(count == 1);
        assert(strcmp(entries[0].word, "abcdefg") == 0);
        assert(entries[0].count == 1);

        wc_free_snapshot(entries);
        wc_destroy(t);
        PASS();
    }

    TEST(snapshot_idempotence);
    {
        /* Verify that calling snapshot twice yields same results. */
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "apple banana cherry apple";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries1 = NULL;
        size_t count1 = 0;
        assert(wc_snapshot(t, &entries1, &count1) == WC_OK);

        wc_entry *entries2 = NULL;
        size_t count2 = 0;
        assert(wc_snapshot(t, &entries2, &count2) == WC_OK);

        assert(count1 == count2);
        for (size_t i = 0; i < count1; ++i)
        {
            assert(strcmp(entries1[i].word, entries2[i].word) == 0);
            assert(entries1[i].count == entries2[i].count);
        }

        wc_free_snapshot(entries1);
        wc_free_snapshot(entries2);
        wc_destroy(t);
        PASS();
    }

    TEST(large_initial_capacity);
    {
        /* Test that large initial_capacity works without crash. */
        wc_config cfg = { .initial_capacity =
                                  1000000 }; /* 1M buckets requested */
        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        assert(wc_add_word(t, "test") == WC_OK);
        assert(wc_total_words(t) == 1);

        wc_destroy(t);
        PASS();
    }

    TEST(small_initial_capacity_clamped);
    {
        /* Test that small initial_capacity is clamped to default. */
        wc_config cfg = { .initial_capacity = 4 }; /* Tiny, should be clamped */
        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        /* Should still work normally (clamped to default 16384). */
        const char *text = "hello world";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);
        assert(wc_unique_words(t) == 2);

        wc_destroy(t);
        PASS();
    }
}

/*===========================================================================
 * Main
 *===========================================================================*/

int main(void)
{
    printf("\n=== Wordcount Library Tests ===\n\n");

    printf("Lifecycle tests:\n");
    test_create_destroy();

    printf("\nWord insertion tests:\n");
    test_add_word();

    printf("\nText processing tests:\n");
    test_process_text();

    printf("\nSnapshot tests:\n");
    test_snapshot();

    printf("\nQuery function tests:\n");
    test_query_functions();

    printf("\nVersion tests:\n");
    test_version();

    printf("\nStress tests:\n");
    test_stress();

    printf("\n=== Results: %d/%d tests passed ===\n\n",
           tests_passed,
           tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
