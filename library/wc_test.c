/**
 * @file wc_test.c
 * @brief Unit tests for the wordcount library.
 */

#include "wordcount.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*===========================================================================
 * Simple test harness
 *===========================================================================*/

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_CASE(name)                   \
    do                                    \
    {                                     \
        ++g_tests_run;                    \
        (void)printf("  %-50s ", (name)); \
        (void)fflush(stdout);             \
    } while (0)

#define TEST_PASS()               \
    do                            \
    {                             \
        ++g_tests_passed;         \
        (void)printf("[PASS]\n"); \
    } while (0)

#define TEST_FAIL(msg)                      \
    do                                      \
    {                                       \
        (void)printf("[FAIL] %s\n", (msg)); \
    } while (0)

/*===========================================================================
 * Test cases
 *===========================================================================*/

static void test_create_destroy(void)
{
    TEST_CASE("create_destroy_null_config");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);
        assert(wc_total_words(t) == 0U);
        assert(wc_unique_words(t) == 0U);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("create_with_explicit_config");
    {
        wc_config cfg;
        cfg.initial_capacity = 128U;
        cfg.max_word_length = 32U;

        wc_table *t = wc_create(&cfg);
        assert(t != NULL);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("destroy_null_is_safe");
    {
        wc_destroy(NULL);
        TEST_PASS();
    }
}

static void test_add_word(void)
{
    TEST_CASE("add_single_word");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_add_word(t, "hello");
        assert(st == WC_OK);
        assert(wc_total_words(t) == 1U);
        assert(wc_unique_words(t) == 1U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("add_duplicate_word");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        assert(wc_add_word(t, "hello") == WC_OK);
        assert(wc_add_word(t, "hello") == WC_OK);
        assert(wc_add_word(t, "hello") == WC_OK);

        assert(wc_total_words(t) == 3U);
        assert(wc_unique_words(t) == 1U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("add_multiple_distinct_words");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        assert(wc_add_word(t, "apple") == WC_OK);
        assert(wc_add_word(t, "banana") == WC_OK);
        assert(wc_add_word(t, "cherry") == WC_OK);
        assert(wc_add_word(t, "apple") == WC_OK);

        assert(wc_total_words(t) == 4U);
        assert(wc_unique_words(t) == 3U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("add_word_null_table");
    {
        wc_status st = wc_add_word(NULL, "hello");
        assert(st == WC_ERR_INVALID_ARGUMENT);
        TEST_PASS();
    }

    TEST_CASE("add_word_null_string");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_add_word(t, NULL);
        assert(st == WC_ERR_INVALID_ARGUMENT);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("add_empty_string_is_noop");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        assert(wc_add_word(t, "") == WC_OK);
        assert(wc_total_words(t) == 0U);
        assert(wc_unique_words(t) == 0U);

        wc_destroy(t);
        TEST_PASS();
    }
}

static void test_process_text(void)
{
    TEST_CASE("process_simple_text");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "Hello, World!";
        wc_status st = wc_process_text(t, text, strlen(text));
        assert(st == WC_OK);

        assert(wc_total_words(t) == 2U);
        assert(wc_unique_words(t) == 2U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("process_case_insensitive");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "Hello HELLO hello HeLLo";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        assert(wc_total_words(t) == 4U);
        assert(wc_unique_words(t) == 1U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("process_empty_text");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_process_text(t, "", 0U);
        assert(st == WC_OK);
        assert(wc_total_words(t) == 0U);
        assert(wc_unique_words(t) == 0U);

        st = wc_process_text(t, NULL, 0U);
        assert(st == WC_OK);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("process_punctuation_only");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "!@#$%^&*()123456789";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        assert(wc_total_words(t) == 0U);
        assert(wc_unique_words(t) == 0U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("process_null_data_nonzero_len");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_status st = wc_process_text(t, NULL, 100U);
        assert(st == WC_ERR_INVALID_ARGUMENT);

        wc_destroy(t);
        TEST_PASS();
    }
}

static void test_snapshot(void)
{
    TEST_CASE("snapshot_sorted_by_count");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "apple banana apple cherry apple banana";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries = NULL;
        size_t count = 0U;
        wc_status st = wc_snapshot(t, &entries, &count);

        assert(st == WC_OK);
        assert(entries != NULL);
        assert(count == 3U);

        assert(strcmp(entries[0].word, "apple") == 0);
        assert(entries[0].count == 3U);

        assert(strcmp(entries[1].word, "banana") == 0);
        assert(entries[1].count == 2U);

        assert(strcmp(entries[2].word, "cherry") == 0);
        assert(entries[2].count == 1U);

        wc_free_snapshot(entries);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("snapshot_ties_sorted_alphabetically");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "zebra apple mango";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries = NULL;
        size_t count = 0U;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);

        assert(entries != NULL);
        assert(count == 3U);

        assert(strcmp(entries[0].word, "apple") == 0);
        assert(strcmp(entries[1].word, "mango") == 0);
        assert(strcmp(entries[2].word, "zebra") == 0);

        wc_free_snapshot(entries);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("snapshot_empty_table");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        wc_entry *entries = (wc_entry *)1;
        size_t count = 123U;

        wc_status st = wc_snapshot(t, &entries, &count);
        assert(st == WC_OK);
        assert(entries == NULL);
        assert(count == 0U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("free_snapshot_null_is_safe");
    {
        wc_free_snapshot(NULL);
        TEST_PASS();
    }
}

static void test_query_functions(void)
{
    TEST_CASE("total_words_null_table");
    {
        assert(wc_total_words(NULL) == 0U);
        TEST_PASS();
    }

    TEST_CASE("unique_words_null_table");
    {
        assert(wc_unique_words(NULL) == 0U);
        TEST_PASS();
    }
}

static void test_version(void)
{
    TEST_CASE("version_string_format");
    {
        const char *version = wc_version();
        assert(version != NULL);
        assert(strlen(version) > 0U);
        assert(strchr(version, '.') != NULL);
        TEST_PASS();
    }
}

static void test_stress(void)
{
    TEST_CASE("many_unique_words");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        char word[32];
        const int n = 10000;

        for (int i = 0; i < n; ++i)
        {
            int written = snprintf(word, sizeof(word), "word%d", i);
            assert(written > 0);
            assert((size_t)written < sizeof(word));

            wc_status st = wc_add_word(t, word);
            assert(st == WC_OK);
        }

        assert(wc_total_words(t) == (size_t)n);
        assert(wc_unique_words(t) == (size_t)n);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("many_duplicates");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const int n = 100000;

        for (int i = 0; i < n; ++i)
        {
            assert(wc_add_word(t, "same") == WC_OK);
        }

        assert(wc_total_words(t) == (size_t)n);
        assert(wc_unique_words(t) == 1U);

        wc_entry *entries = NULL;
        size_t count = 0U;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);

        assert(count == 1U);
        assert(entries[0].count == (size_t)n);

        wc_free_snapshot(entries);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("word_truncation_in_process_text");
    {
        wc_config cfg;
        cfg.initial_capacity = 0U;
        cfg.max_word_length = 8U; /* 7 chars + NUL */

        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        const char *text = "abcdefghijk";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries = NULL;
        size_t count = 0U;
        assert(wc_snapshot(t, &entries, &count) == WC_OK);

        assert(count == 1U);
        assert(strcmp(entries[0].word, "abcdefg") == 0);
        assert(entries[0].count == 1U);

        wc_free_snapshot(entries);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("snapshot_idempotence");
    {
        wc_table *t = wc_create(NULL);
        assert(t != NULL);

        const char *text = "apple banana cherry apple";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);

        wc_entry *entries1 = NULL;
        size_t count1 = 0U;
        assert(wc_snapshot(t, &entries1, &count1) == WC_OK);

        wc_entry *entries2 = NULL;
        size_t count2 = 0U;
        assert(wc_snapshot(t, &entries2, &count2) == WC_OK);

        assert(count1 == count2);

        for (size_t i = 0U; i < count1; ++i)
        {
            assert(strcmp(entries1[i].word, entries2[i].word) == 0);
            assert(entries1[i].count == entries2[i].count);
        }

        wc_free_snapshot(entries1);
        wc_free_snapshot(entries2);
        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("large_initial_capacity");
    {
        wc_config cfg;
        cfg.initial_capacity = 1000000U;
        cfg.max_word_length = 0U;

        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        assert(wc_add_word(t, "test") == WC_OK);
        assert(wc_total_words(t) == 1U);
        assert(wc_unique_words(t) == 1U);

        wc_destroy(t);
        TEST_PASS();
    }

    TEST_CASE("small_initial_capacity_clamped");
    {
        wc_config cfg;
        cfg.initial_capacity = 4U;
        cfg.max_word_length = 0U;

        wc_table *t = wc_create(&cfg);
        assert(t != NULL);

        const char *text = "hello world";
        assert(wc_process_text(t, text, strlen(text)) == WC_OK);
        assert(wc_unique_words(t) == 2U);

        wc_destroy(t);
        TEST_PASS();
    }
}

/*===========================================================================
 * main
 *===========================================================================*/

int main(void)
{
    (void)printf("\n=== Wordcount Library Tests ===\n\n");

    (void)printf("Lifecycle tests:\n");
    test_create_destroy();

    (void)printf("\nWord insertion tests:\n");
    test_add_word();

    (void)printf("\nText processing tests:\n");
    test_process_text();

    (void)printf("\nSnapshot tests:\n");
    test_snapshot();

    (void)printf("\nQuery function tests:\n");
    test_query_functions();

    (void)printf("\nVersion tests:\n");
    test_version();

    (void)printf("\nStress tests:\n");
    test_stress();

    (void)printf("\n=== Results: %d/%d tests passed ===\n\n",
                 g_tests_passed,
                 g_tests_run);

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
