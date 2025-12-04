/**
 * @file wc_main.c
 * @brief Command-line wrapper for the wordcount library.
 *
 * A small, portable word-frequency tool intended as a reference example
 * for how to build a clean C99 CLI around a reusable library.
 *
 * Usage:
 *   wc [file ...]
 *
 * Behavior:
 *   - If one or more file names are given, each file is opened in binary
 *     mode ("rb"), read in its entirety into memory, and processed.
 *   - If no files are specified, stdin is read instead.
 *   - The program then prints all words and their counts to stdout,
 *     sorted by:
 *         1. descending count,
 *         2. ascending lexicographic order for ties.
 *   - Finally a summary (total and unique word counts) is printed to
 *     stderr.
 *
 * Notes:
 *   - Files are read completely into memory before processing. This
 *     keeps the example simple and correct with respect to word
 *     boundaries (no chunk-splitting across calls).
 *   - For very large inputs, a streaming-aware caller would be more
 *     appropriate; see the library documentation for details.
 *
 * Build example (GCC or Clang):
 *
 *   gcc -std=c99 -Wall -Wextra -pedantic \
 *       wordcount.c wc_main.c -o wc
 */

#include "wordcount.h"

#include <errno.h>  /* errno */
#include <stdio.h>  /* FILE, fopen, fclose, fread, printf, fprintf */
#include <stdlib.h> /* malloc, realloc, free, EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h> /* strerror */

/*===========================================================================
 * Configuration
 *===========================================================================*/

/*
 * Size of each read chunk when loading a file or stdin.
 * A power-of-two buffer size is a reasonable default for most systems.
 */
enum
{
    WC_READ_CHUNK_SIZE = 65536
};

/*
 * How many of the most frequent words to print.
 * A value of 0 means "print all".
 */
enum
{
    WC_TOP_N = 0
};

/*
 * Application-specific exit codes.
 * EXIT_FAILURE is used for generic failures; EXIT_SUCCESS for success.
 * EXIT_ERROR is separated only to make the intent explicit.
 */
enum
{
    WC_EXIT_OK = 0,
    WC_EXIT_ERROR = 2
};

/*===========================================================================
 * Error reporting helpers
 *===========================================================================*/

/**
 * @brief Print an error message to stderr in a consistent format.
 *
 * @param context  Optional context string (filename, etc.), or NULL.
 * @param message  NUL-terminated error message string.
 */
static void wc_report_error(const char *context, const char *message)
{
    if (context != NULL && context[0] != '\0')
    {
        (void)fprintf(stderr, "wc: %s: %s\n", context, message);
    }
    else
    {
        (void)fprintf(stderr, "wc: %s\n", message);
    }
}

/**
 * @brief Convert a wc_status code to a human-readable string.
 *
 * The returned pointer refers to a static string and must not be freed.
 */
static const char *wc_status_message(wc_status st)
{
    switch (st)
    {
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
 * File / stream processing
 *===========================================================================*/

/**
 * @brief Read an entire stream into a dynamically allocated buffer.
 *
 * @param fp        Open FILE* to read from (must not be NULL).
 * @param out_data  Output: pointer to allocated buffer, or NULL on success
 *                  with an empty stream.
 * @param out_len   Output: number of bytes read.
 *
 * @return 1 on success, 0 on error.
 *
 * On success:
 *   - If at least one byte was read, *out_data points to a buffer of at
 *     least *out_len bytes; the caller must free it with free().
 *   - If the stream is empty, *out_data is set to NULL and *out_len is 0.
 *
 * On failure:
 *   - Any allocated buffer is freed.
 *   - *out_data is set to NULL and *out_len is set to 0.
 *   - errno is left as set by the failing standard library call.
 */
static int wc_read_entire_stream(FILE *fp, char **out_data, size_t *out_len)
{
    char *buffer = NULL;
    size_t capacity = 0U;
    size_t length = 0U;

    for (;;)
    {
        /* Grow the buffer if there is not enough room for the next chunk. */
        if (length + WC_READ_CHUNK_SIZE > capacity)
        {
            size_t new_capacity;

            if (capacity == 0U)
            {
                new_capacity = WC_READ_CHUNK_SIZE;
            }
            else
            {
                new_capacity = capacity * 2U;
            }

            /* Very simple overflow check when doubling. */
            if (new_capacity < capacity)
            {
                free(buffer);
                *out_data = NULL;
                *out_len = 0U;
                errno = ENOMEM;
                return 0;
            }

            char *new_buffer = (char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL)
            {
                free(buffer);
                *out_data = NULL;
                *out_len = 0U;
                /* errno is assumed to be set by realloc implementation. */
                return 0;
            }

            buffer = new_buffer;
            capacity = new_capacity;
        }

        /* Read up to WC_READ_CHUNK_SIZE bytes at a time. */
        const size_t nread = fread(buffer + length, 1U, WC_READ_CHUNK_SIZE, fp);
        length += nread;

        if (nread < (size_t)WC_READ_CHUNK_SIZE)
        {
            if (ferror(fp))
            {
                free(buffer);
                *out_data = NULL;
                *out_len = 0U;
                /* errno is set by the underlying I/O error. */
                return 0;
            }
            /* feof(fp) is true here: end-of-file reached. */
            break;
        }
    }

    if (length == 0U)
    {
        free(buffer);
        *out_data = NULL;
        *out_len = 0U;
    }
    else
    {
        *out_data = buffer;
        *out_len = length;
    }

    return 1;
}

/**
 * @brief Process a single stream and add words to the table.
 *
 * Reads the entire stream into memory, then processes it as a single unit
 * with wc_process_text(), ensuring that words split across chunk
 * boundaries are handled correctly.
 *
 * @param t         Word-count table (must not be NULL).
 * @param fp        Open FILE* to read from (must not be NULL).
 * @param label     Context label for error messages (e.g., filename).
 *
 * @return WC_OK on success, or a wc_status error code on failure.
 */
static wc_status wc_process_stream(wc_table *t, FILE *fp, const char *label)
{
    char *data = NULL;
    size_t len = 0U;

    if (!wc_read_entire_stream(fp, &data, &len))
    {
        /*
         * Could be I/O error or out-of-memory. We report strerror(errno)
         * for the caller's benefit, but always translate it to a generic
         * wc_status for simplicity.
         */
        wc_report_error(label, strerror(errno));
        return WC_ERR_OUT_OF_MEMORY;
    }

    wc_status result = WC_OK;

    if (len > 0U)
    {
        result = wc_process_text(t, data, len);
        if (result != WC_OK)
        {
            wc_report_error(label, wc_status_message(result));
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
 * @param entries  Array of word-count entries (may be NULL if count is 0).
 * @param count    Number of entries in the array.
 * @param limit    Maximum number of entries to print (0 = print all).
 */
static void
wc_print_results(const wc_entry *entries, size_t count, size_t limit)
{
    size_t to_print = count;

    if (limit > 0U && limit < count)
    {
        to_print = limit;
    }

    for (size_t i = 0U; i < to_print; ++i)
    {
        (void)printf("%7zu %s\n", entries[i].count, entries[i].word);
    }
}

/**
 * @brief Print summary statistics to stderr.
 *
 * @param t  Word-count table (must not be NULL).
 */
static void wc_print_summary(const wc_table *t)
{
    (void)fprintf(stderr,
                  "\nTotal words: %zu, Unique words: %zu\n",
                  wc_total_words(t),
                  wc_unique_words(t));
}

/*===========================================================================
 * Program entry point
 *===========================================================================*/

int main(int argc, char *argv[])
{
    int exit_code = WC_EXIT_OK;

    /* Create word-count table with default configuration. */
    wc_table *table = wc_create(NULL);
    if (table == NULL)
    {
        wc_report_error(NULL, "failed to create word table (out of memory)");
        return WC_EXIT_ERROR;
    }

    if (argc < 2)
    {
        /* No file arguments: process stdin. */
        const wc_status st = wc_process_stream(table, stdin, "<stdin>");
        if (st != WC_OK)
        {
            exit_code = WC_EXIT_ERROR;
        }
    }
    else
    {
        /* Process each file specified on the command line. */
        for (int i = 1; i < argc; ++i)
        {
            const char *filename = argv[i];

            FILE *fp = fopen(filename, "rb");
            if (fp == NULL)
            {
                wc_report_error(filename, strerror(errno));
                exit_code = WC_EXIT_ERROR;
                continue;
            }

            const wc_status st = wc_process_stream(table, fp, filename);
            (void)fclose(fp);

            if (st != WC_OK)
            {
                exit_code = WC_EXIT_ERROR;
            }
        }
    }

    /* If any words were processed, print results and a summary. */
    if (wc_unique_words(table) > 0U)
    {
        wc_entry *entries = NULL;
        size_t count = 0U;

        const wc_status st = wc_snapshot(table, &entries, &count);
        if (st == WC_OK)
        {
            wc_print_results(entries, count, WC_TOP_N);
            wc_free_snapshot(entries);
        }
        else
        {
            wc_report_error(NULL, wc_status_message(st));
            exit_code = WC_EXIT_ERROR;
        }

        wc_print_summary(table);
    }

    wc_destroy(table);

    return exit_code;
}
