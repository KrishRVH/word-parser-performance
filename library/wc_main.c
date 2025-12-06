/*
** wc_main.c - Command-line interface
**
** Public domain.
**
** DESIGN
**
**   Uses memory-mapped I/O for zero-copy file access, enabling
**   processing of files larger than physical RAM. Platform-specific
**   code is isolated in the os_* functions.
**
**   Error handling follows the goto-cleanup canonical pattern
**   from Linux kernel and SQLite style guides.
**
** Usage: wc [file ...]
** Reads stdin if no files given. Top 10 to stdout, summary to stderr.
*/

#include "wordcount.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOPN 10
#define STDIN_CHUNK 65536

/* --- Overflow-safe arithmetic --- */

static int add_overflows_sz(size_t a, size_t b)
{
    return a > SIZE_MAX - b;
}

/* --- Platform abstraction for memory-mapped files --- */

#ifdef _WIN32

/*
** Windows implementation using CreateFileMapping.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct
{
    void *data;
    size_t size;
    HANDLE hFile;
    HANDLE hMap;
} MappedFile;

/*
** Map Win32 error to errno. Uses _dosmaperr if available (MSVC/UCRT),
** otherwise falls back to EIO.
*/
static void set_errno_from_win32(void)
{
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
    {
        errno = ENOENT;
    }
    else if (err == ERROR_ACCESS_DENIED)
    {
        errno = EACCES;
    }
    else if (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY)
    {
        errno = ENOMEM;
    }
    else
    {
        errno = EIO;
    }
}

static int os_map(MappedFile *mf, const char *path)
{
    LARGE_INTEGER sz;

    memset(mf, 0, sizeof *mf);
    mf->hFile = INVALID_HANDLE_VALUE;
    mf->hMap = NULL;

    mf->hFile = CreateFileA(path,
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    if (mf->hFile == INVALID_HANDLE_VALUE)
    {
        set_errno_from_win32();
        return -1;
    }

    if (!GetFileSizeEx(mf->hFile, &sz))
    {
        set_errno_from_win32();
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    if (sz.QuadPart == 0)
    {
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return 0; /* empty file is ok */
    }

    /* Reject files larger than size_t can represent */
    if (sz.QuadPart < 0 ||
        (unsigned long long)sz.QuadPart > (unsigned long long)SIZE_MAX)
    {
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        errno = EFBIG;
        return -1;
    }

    mf->hMap = CreateFileMappingA(mf->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mf->hMap)
    {
        set_errno_from_win32();
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    mf->data = MapViewOfFile(mf->hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mf->data)
    {
        set_errno_from_win32();
        CloseHandle(mf->hMap);
        CloseHandle(mf->hFile);
        mf->hMap = NULL;
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    mf->size = (size_t)sz.QuadPart;
    return 0;
}

static void os_unmap(MappedFile *mf)
{
    if (mf->data)
        UnmapViewOfFile(mf->data);
    if (mf->hMap)
        CloseHandle(mf->hMap);
    if (mf->hFile != INVALID_HANDLE_VALUE)
        CloseHandle(mf->hFile);
    memset(mf, 0, sizeof *mf);
    mf->hFile = INVALID_HANDLE_VALUE;
}

#else

/*
** POSIX implementation using mmap.
*/
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
    void *data;
    size_t size;
    int fd;
} MappedFile;

static int os_map(MappedFile *mf, const char *path)
{
    struct stat st;
    int saved_errno;

    memset(mf, 0, sizeof *mf);
    mf->fd = -1;

    mf->fd = open(path, O_RDONLY);
    if (mf->fd < 0)
        return -1;

    if (fstat(mf->fd, &st) < 0)
    {
        saved_errno = errno;
        close(mf->fd);
        mf->fd = -1;
        errno = saved_errno;
        return -1;
    }

    if (st.st_size == 0)
    {
        close(mf->fd);
        mf->fd = -1;
        return 0; /* empty file is ok */
    }

    /* Reject files larger than size_t can represent (32-bit builds) */
    if ((off_t)(size_t)st.st_size != st.st_size)
    {
        close(mf->fd);
        mf->fd = -1;
        errno = EFBIG;
        return -1;
    }

    mf->data =
            mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, mf->fd, 0);
    if (mf->data == MAP_FAILED)
    {
        saved_errno = errno;
        mf->data = NULL;
        close(mf->fd);
        mf->fd = -1;
        errno = saved_errno;
        return -1;
    }

#ifdef MADV_SEQUENTIAL
    madvise(mf->data, (size_t)st.st_size, MADV_SEQUENTIAL);
#endif

    mf->size = (size_t)st.st_size;
    return 0;
}

static void os_unmap(MappedFile *mf)
{
    if (mf->data && mf->size > 0)
    {
        munmap(mf->data, mf->size);
    }
    if (mf->fd >= 0)
        close(mf->fd);
    memset(mf, 0, sizeof *mf);
    mf->fd = -1;
}

#endif /* _WIN32 */

/* --- Stdin handling (cannot mmap, must buffer) --- */

static char *read_stdin(size_t *out)
{
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    size_t n;
    int rc = -1;

    *out = 0;

    for (;;)
    {
        /* Guard against overflow before using len + STDIN_CHUNK */
        if (add_overflows_sz(len, (size_t)STDIN_CHUNK))
        {
            errno = ENOMEM;
            goto cleanup;
        }

        if (len + (size_t)STDIN_CHUNK > cap)
        {
            size_t nc;
            char *p;

            if (cap == 0)
            {
                nc = (size_t)STDIN_CHUNK;
            }
            else if (cap > SIZE_MAX / 2)
            {
                errno = ENOMEM;
                goto cleanup;
            }
            else
            {
                nc = cap * 2;
            }

            p = realloc(buf, nc);
            if (!p)
                goto cleanup;
            buf = p;
            cap = nc;
        }

        n = fread(buf + len, 1, STDIN_CHUNK, stdin);
        len += n;

        if (n < STDIN_CHUNK)
        {
            if (ferror(stdin))
                goto cleanup;
            break; /* EOF */
        }
    }

    if (len == 0)
        goto cleanup;

    rc = 0;
    *out = len;

cleanup:
    if (rc < 0)
    {
        free(buf);
        return NULL;
    }
    return buf;
}

/* --- Processing --- */

static int
process_mapped(wc *w, const char *data, size_t size, const char *name)
{
    int rc;

    rc = wc_scan(w, data, size);
    if (rc == WC_NOMEM)
    {
        (void)fprintf(stderr, "wc: %s: out of memory\n", name);
        return -1;
    }
    if (rc != WC_OK)
    {
        (void)fprintf(stderr, "wc: %s: scan error\n", name);
        return -1;
    }

    return 0;
}

static int process_file(wc *w, const char *path)
{
    MappedFile mf;
    int rc = -1;

    memset(&mf, 0, sizeof mf);
#ifdef _WIN32
    mf.hFile = INVALID_HANDLE_VALUE;
#else
    mf.fd = -1;
#endif

    if (os_map(&mf, path) < 0)
    {
        (void)fprintf(stderr, "wc: %s: %s\n", path, strerror(errno));
        goto cleanup;
    }

    if (mf.size == 0)
    {
        rc = 0;
        goto cleanup;
    }

    rc = process_mapped(w, mf.data, mf.size, path);

cleanup:
    os_unmap(&mf);
    return rc;
}

static int process_stdin(wc *w)
{
    char *data = NULL;
    size_t len = 0;
    int rc = -1;

    errno = 0;
    data = read_stdin(&len);
    if (!data)
    {
        if (errno == 0 && len == 0)
        {
            rc = 0;
            goto cleanup;
        }
        if (errno)
        {
            (void)fprintf(stderr, "wc: <stdin>: %s\n", strerror(errno));
        }
        else
        {
            (void)fprintf(stderr, "wc: <stdin>: read error\n");
        }
        goto cleanup;
    }

    rc = process_mapped(w, data, len, "<stdin>");

cleanup:
    free(data);
    return rc;
}

/* --- Output --- */

static void output(const wc *w)
{
    wc_word *words = NULL;
    size_t len = 0;
    size_t i;
    size_t n;
    int rc;

    rc = wc_results(w, &words, &len);
    if (rc == WC_NOMEM)
    {
        (void)fprintf(stderr, "wc: out of memory\n");
        goto cleanup;
    }
    if (rc != WC_OK)
    {
        (void)fprintf(stderr, "wc: error retrieving results\n");
        goto cleanup;
    }

    if (len == 0)
    {
        (void)fprintf(stderr, "No words found.\n");
        goto cleanup;
    }

    n = len < TOPN ? len : TOPN;
    printf("\n%7s  %-20s  %s\n", "Count", "Word", "%");
    printf("-------  --------------------  ------\n");

    for (i = 0; i < n; i++)
    {
        double pct = 100.0 * (double)words[i].count / (double)wc_total(w);
        printf("%7zu  %-20s  %5.2f\n", words[i].count, words[i].word, pct);
    }

    (void)fprintf(
            stderr, "\nTotal: %zu  Unique: %zu\n", wc_total(w), wc_unique(w));

cleanup:
    wc_results_free(words);
}

/* --- Main --- */

int main(int argc, char **argv)
{
    wc *w = NULL;
    int i;
    int err = 0;
    int rc = 1;

    w = wc_open(0);
    if (!w)
    {
        (void)fprintf(stderr, "wc: out of memory\n");
        goto cleanup;
    }

    if (argc < 2)
    {
        if (process_stdin(w) < 0)
            err = 1;
    }
    else
    {
        for (i = 1; i < argc; i++)
        {
            if (process_file(w, argv[i]) < 0)
                err = 1;
        }
    }

    if (wc_unique(w) > 0)
        output(w);

    rc = err ? 1 : 0;

cleanup:
    wc_close(w);
    return rc;
}
