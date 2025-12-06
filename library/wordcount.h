/*
** wordcount.h - Word frequency counter
**
** Public domain.
**
** STABILITY
**
**   API is stable. Functions will not be removed or change signature.
**   Separate wc objects may be used from different threads.
**   A single wc object must not be shared without synchronization.
**
** RETURN VALUES
**
**   Functions returning int use: WC_OK (0) for success, WC_ERROR (1)
**   for bad arguments or corrupt state, WC_NOMEM (2) for allocation
**   failure. Query functions (wc_total, wc_unique) return 0 on NULL.
**
**   Use wc_errstr() to get a human-readable description of any error
**   code. The returned string is static and must not be freed.
**
** CASE HANDLING
**
**   wc_add()  - case-sensitive: "Hello" and "hello" are distinct
**   wc_scan() - normalizes to lowercase: "Hello" becomes "hello"
**
** WORD DETECTION
**
**   Only ASCII letters (A-Z, a-z) are recognized as word characters.
**   All other bytes (including UTF-8 multibyte sequences) are treated
**   as word separators. This library assumes an ASCII-compatible
**   execution character set (true for ASCII, UTF-8, ISO-8859-*).
**
** WORD LENGTH
**
**   Both functions truncate words exceeding max_word. The hash is
**   computed only over stored characters, ensuring truncated forms
**   of different words collide correctly.
**
** MEMORY CONFIGURATION
**
**   Define WC_MALLOC and WC_FREE before including this header to
**   redirect memory allocation (e.g., to a custom arena or debug
**   allocator). Defaults to stdlib malloc/free.
**
**   WC_REALLOC may also be defined for client code; the core library
**   currently uses only WC_MALLOC and WC_FREE internally.
**
**   For finer control, wc_open_ex() accepts a wc_limits struct that
**   can bound total internal allocations for a wc instance and tune
**   the initial hash table capacity and arena block size. This makes
**   it practical to use on very small systems with fixed memory
**   budgets.
**
** BUILD CONFIGURATION
**
**   WC_OMIT_ASSERT  - Define to disable internal assertions (smaller
**                     code, but less safety checking in debug builds).
**
**   WC_STACK_BUFFER - Define as 0 to heap-allocate scan buffers
**                     instead of using stack. Useful for constrained
**                     stack environments. Default is 1 (use stack).
**
** PORTABILITY
**
**   Requires C99. Uses only types guaranteed by C99 (no optional
**   exact-width types). Works on any hosted implementation with
**   8-bit chars and ASCII-compatible character encoding.
*/
#ifndef WORDCOUNT_H
#define WORDCOUNT_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
** Version information. The version number is encoded as:
**   (MAJOR * 1000000) + (MINOR * 1000) + PATCH
*/
#define WC_VERSION "4.1.0"
#define WC_VERSION_NUMBER 4001000

/*
** Result codes for int-returning functions.
*/
#define WC_OK 0    /* Success */
#define WC_ERROR 1 /* Generic error (bad args, corrupt state) */
#define WC_NOMEM 2 /* Memory allocation failed */

/*
** Memory allocator configuration. Define these before including
** wordcount.h to use a custom allocator.
*/
#ifndef WC_MALLOC
#define WC_MALLOC(n) malloc(n)
#endif
#ifndef WC_REALLOC
#define WC_REALLOC(p, n) realloc((p), (n))
#endif
#ifndef WC_FREE
#define WC_FREE(p) free(p)
#endif

/*
** Stack buffer configuration. Set to 0 for heap allocation.
*/
#ifndef WC_STACK_BUFFER
#define WC_STACK_BUFFER 1
#endif

/*
** Opaque word counter handle.
*/
typedef struct wc wc;

/*
** Default sizing for initial hash table capacity and arena block
** size. These can be overridden at compile time by defining
** WC_DEFAULT_INIT_CAP and/or WC_DEFAULT_BLOCK_SZ before including
** this header. If not defined, they are derived from SIZE_MAX.
*/
#ifndef WC_DEFAULT_INIT_CAP
#ifndef WC_DEFAULT_BLOCK_SZ
#include <stdint.h> /* for SIZE_MAX */
#if SIZE_MAX <= 65535u
#define WC_DEFAULT_INIT_CAP 128u
#define WC_DEFAULT_BLOCK_SZ 1024u
#elif SIZE_MAX <= 4294967295u
#define WC_DEFAULT_INIT_CAP 1024u
#define WC_DEFAULT_BLOCK_SZ 16384u
#else
#define WC_DEFAULT_INIT_CAP 4096u
#define WC_DEFAULT_BLOCK_SZ 65536u
#endif
#endif
#endif

/*
** Optional per-instance memory and sizing limits.
**
**   max_bytes:
**     Hard cap on total internal allocations for this wc object.
**     The following pools are counted against this limit:
**       - the hash table (Slot array and its growth)
**       - the arena blocks used for word storage
**       - the optional heap scan buffer when WC_STACK_BUFFER==0
**
**     The wc handle itself (the struct wc) and any arrays returned
**     by wc_results() are NOT counted, since their lifetime and
**     ownership are under the caller's control. 0 = unlimited.
**
**   init_cap:
**     Initial hash table capacity (number of slots). Must be > 0.
**     Rounded up to a power of two internally. 0 = library default
**     chosen from WC_DEFAULT_INIT_CAP based on platform.
**
**   block_size:
**     Arena block size in bytes. Acts as the typical allocation
**     quantum for word storage. 0 = library default chosen from
**     WC_DEFAULT_BLOCK_SZ based on platform.
**
** On small systems, set max_bytes to a fixed budget and leave the
** others at 0 to let the library derive conservative values. On
** larger systems, you can tune init_cap/block_size directly.
*/
typedef struct wc_limits
{
    size_t max_bytes;
    size_t init_cap;
    size_t block_size;
} wc_limits;

/*
** Result entry returned by wc_results().
*/
typedef struct wc_word
{
    const char *word;
    size_t count;
} wc_word;

/*
** Create a new word counter with optional limits.
**
**   max_word: Maximum word length to store. 0 = default (64).
**             Clamped to range [4, 1024].
**
**   limits:   Optional pointer to a wc_limits struct. May be NULL.
**
** Returns NULL on allocation failure or if the supplied limits are
** impossible to satisfy (e.g., max_bytes too small for even minimal
** internal structures).
*/
wc *wc_open_ex(size_t max_word, const wc_limits *limits);

/*
** Create a new word counter with default limits (no explicit memory
** cap, platform-tuned defaults for table and arena sizes).
**
**   max_word: Maximum word length to store. 0 = default (64).
**             Clamped to range [4, 1024].
**
** Returns NULL on allocation failure.
*/
wc *wc_open(size_t max_word);

/*
** Destroy a word counter. NULL-safe.
*/
void wc_close(wc *w);

/*
** Add a single word (case-sensitive, truncates at max_word).
** Empty strings are ignored. Returns WC_OK, WC_ERROR, or WC_NOMEM.
*/
int wc_add(wc *restrict w, const char *restrict word);

/*
** Scan text for words (lowercases, truncates at max_word).
** Non-alphabetic characters are word separators.
** Returns WC_OK, WC_ERROR, or WC_NOMEM.
*/
int wc_scan(wc *restrict w, const char *restrict text, size_t len);

/*
** Query total word count. Returns 0 if w is NULL.
*/
size_t wc_total(const wc *w);

/*
** Query unique word count. Returns 0 if w is NULL.
*/
size_t wc_unique(const wc *w);

/*
** Get sorted results (by count desc, then alphabetically).
**
**   out: Receives pointer to array (caller must free via wc_results_free)
**   n:   Receives array length
**
** Returns WC_OK, WC_ERROR (bad args), or WC_NOMEM.
** On empty results, *out=NULL and *n=0 with WC_OK return.
**
** Note: The temporary results array is allocated via WC_MALLOC and
** is not counted against max_bytes in wc_limits, since its lifetime
** is entirely under the caller's control.
*/
int wc_results(const wc *restrict w,
               wc_word **restrict out,
               size_t *restrict n);

/*
** Free results array. NULL-safe.
*/
void wc_results_free(wc_word *r);

/*
** Return human-readable error description.
** The returned string is static and must not be freed.
*/
const char *wc_errstr(int rc);

/*
** Return version string.
*/
const char *wc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* WORDCOUNT_H */
