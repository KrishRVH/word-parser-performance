/**
 * @file wc_main.c
 * @brief CLI wrapper for the wordcount library.
 *
 * A minimal, portable command-line word frequency counter.
 * Demonstrates proper usage of the wordcount library.
 *
 * Usage: wc [file...]
 *
 * If no files are specified, reads from stdin.
 * Outputs word frequencies in descending order.
 */

#include "wordcount.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*===========================================================================
 * Configuration
 *===========================================================================*/

enum {
    /** Buffer size for reading input. */
    READ_BUFFER_SIZE = 65536,

    /** Number of top words to display (0 = all). */
    TOP_N = 0,

    /** Exit code for success. */
    EXIT_OK = 0,

    /** Exit code for runtime error. */
    EXIT_ERROR = 2
};

/*===========================================================================
 * Error handling
 *===========================================================================*/

/**
 * @brief Print an error message to stderr.
 *
 * @param context  Context string (e.g., filename), or NULL.
 * @param message  Error message.
 */
static void report_error(const char* context, const char* message)
{
    if (context != NULL && context[0] != '\0') {
        (void)fprintf(stderr, "wc: %s: %s\n", context, message);
    }
    else {
        (void)fprintf(stderr, "wc: %s\n", message);
    }
}

/**
 * @brief Convert wc_status to human-readable string.
 */
static const char* status_string(wc_status st)
{
    switch (st) {
        case WC_OK:
            return "success";
        case WC_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case WC_ERR_OUT_OF_MEMORY:
            return "out of memory";
        default:
            return "unknown error";
    }
}

/*===========================================================================
 * File processing
 *===========================================================================*/

/**
 * @brief Read an entire file into a dynamically allocated buffer.
 *
 * @param fp        Open file handle.
 * @param out_data  Output: pointer to allocated buffer (caller must free).
 * @param out_len   Output: number of bytes read.
 * @return          true on success, false on error.
 *
 * @note On success with empty file, *out_data is NULL and *out_len is 0.
 */
static bool read_entire_file(FILE* fp, char** out_data, size_t* out_len)
{
    char* buffer = NULL;
    size_t capacity = 0;
    size_t length = 0;

    for (;;) {
        /* Grow buffer as needed. */
        if (length + READ_BUFFER_SIZE > capacity) {
            const size_t new_capacity =
                    (capacity == 0) ? READ_BUFFER_SIZE : capacity * 2;

            char* new_buffer = realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                return false;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }

        const size_t nread = fread(buffer + length, 1, READ_BUFFER_SIZE, fp);
        length += nread;

        if (nread < READ_BUFFER_SIZE) {
            if (ferror(fp)) {
                free(buffer);
                return false;
            }
            break; /* EOF reached. */
        }
    }

    *out_data = buffer;
    *out_len = length;
    return true;
}

/**
 * @brief Process a single file and add words to the table.
 *
 * Reads the entire file into memory, then processes it as a single unit.
 * This ensures correct word counting even for words that would otherwise
 * be split across read boundaries.
 *
 * @param t         Word-count table.
 * @param fp        Open file handle.
 * @param filename  Filename for error messages.
 * @return          WC_OK on success, error status on failure.
 */
static wc_status process_file(wc_table* t, FILE* fp, const char* filename)
{
    char* data = NULL;
    size_t len = 0;

    if (!read_entire_file(fp, &data, &len)) {
        /*
         * Could be OOM or I/O error. For simplicity, report generically.
         * A production CLI might distinguish these cases.
         */
        report_error(filename, "failed to read file");
        return WC_ERR_OUT_OF_MEMORY;
    }

    wc_status result = WC_OK;

    if (len > 0) {
        result = wc_process_text(t, data, len);
        if (result != WC_OK) {
            report_error(filename, status_string(result));
        }
    }

    free(data);
    return result;
}

/*===========================================================================
 * Output formatting
 *===========================================================================*/

/**
 * @brief Print word frequencies to stdout.
 *
 * @param entries  Array of word-count entries.
 * @param count    Number of entries.
 * @param limit    Maximum entries to print (0 = all).
 */
static void print_results(const wc_entry* entries, size_t count, size_t limit)
{
    const size_t to_print = (limit > 0 && limit < count) ? limit : count;

    for (size_t i = 0; i < to_print; ++i) {
        (void)printf("%7zu %s\n", entries[i].count, entries[i].word);
    }
}

/**
 * @brief Print summary statistics to stderr.
 *
 * @param t  Word-count table.
 */
static void print_summary(const wc_table* t)
{
    (void)fprintf(stderr,
                  "\nTotal words: %zu, Unique words: %zu\n",
                  wc_total_words(t),
                  wc_unique_words(t));
}

/*===========================================================================
 * Main entry point
 *===========================================================================*/

int main(int argc, char* argv[])
{
    int exit_code = EXIT_OK;

    /* Create word-count table with default configuration. */
    wc_table* table = wc_create(NULL);
    if (table == NULL) {
        report_error(NULL, "failed to create word table");
        return EXIT_ERROR;
    }

    if (argc < 2) {
        /* No arguments: read from stdin. */
        const wc_status st = process_file(table, stdin, "<stdin>");
        if (st != WC_OK) {
            exit_code = EXIT_ERROR;
        }
    }
    else {
        /* Process each file argument. */
        for (int i = 1; i < argc; ++i) {
            const char* filename = argv[i];

            FILE* fp = fopen(filename, "rb");
            if (fp == NULL) {
                report_error(filename, strerror(errno));
                exit_code = EXIT_ERROR;
                continue;
            }

            const wc_status st = process_file(table, fp, filename);
            (void)fclose(fp);

            if (st != WC_OK) {
                exit_code = EXIT_ERROR;
            }
        }
    }

    /* Output results if any words were processed. */
    if (wc_unique_words(table) > 0) {
        wc_entry* entries = NULL;
        size_t count = 0;

        const wc_status st = wc_snapshot(table, &entries, &count);
        if (st == WC_OK) {
            print_results(entries, count, TOP_N);
            wc_free_snapshot(entries);
        }
        else {
            report_error(NULL, status_string(st));
            exit_code = EXIT_ERROR;
        }

        print_summary(table);
    }

    wc_destroy(table);

    return exit_code;
}
