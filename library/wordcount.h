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
** CASE HANDLING
**
**   wc_add()  - case-sensitive: "Hello" and "hello" are distinct
**   wc_scan() - normalizes to lowercase: "Hello" becomes "hello"
**
** WORD DETECTION
**
**   Only ASCII letters (A-Z, a-z) are recognized as word characters.
**   All other bytes (including UTF-8 multibyte sequences) are treated
**   as word separators.
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
*/
#ifndef WORDCOUNT_H
#define WORDCOUNT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C"
{
#endif
/*
** Version information. The version number is encoded as:
**   (MAJOR * 1000000) + (MINOR * 1000) + PATCH
*/
#define WC_VERSION "3.4.0"
#define WC_VERSION_NUMBER 3004000
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
** Opaque word counter handle.
*/
typedef struct wc wc;
/*
** Result entry returned by wc_results().
*/
typedef struct wc_word
{
    const char *word;
    size_t count;
} wc_word;
/*
** Create a new word counter.
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
int wc_add(wc *w, const char *word);
/*
** Scan text for words (lowercases, truncates at max_word).
** Non-alphabetic characters are word separators.
** Returns WC_OK, WC_ERROR, or WC_NOMEM.
*/
int wc_scan(wc *w, const char *text, size_t len);
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
*/
int wc_results(const wc *w, wc_word **out, size_t *n);
/*
** Free results array. NULL-safe.
*/
void wc_results_free(wc_word *r);
/*
** Return version string.
*/
const char *wc_version(void);
#ifdef __cplusplus
}
#endif
#endif /* WORDCOUNT_H */
