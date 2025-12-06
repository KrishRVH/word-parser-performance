# Elegant C11: Patterns that impress 30-year veterans

Building a word frequency counter that demonstrates true C mastery requires understanding what separates competent code from code that makes seasoned engineers pause and appreciate. The answer isnâ€™t clever tricksâ€”itâ€™s **economy, clarity, and choosing the right data structures**. Rob Pikeâ€™s Rule 5 captures it best: â€œData dominates. If youâ€™ve chosen the right data structures and organized things well, the algorithms will almost always be self-evident.â€

This report synthesizes guidance from kernel developers, Redisâ€™s creator, SQLiteâ€™s architects, and recognized C experts on what makes C code genuinely elegant.

## What experts actually mean by â€œelegant Câ€

Salvatore Sanfilippo (antirez), creator of Redis, treats code as literature: â€œCode is like a poem; itâ€™s not just something we write to reach some practical result.â€  This philosophy shapes expert Câ€”it should read naturally, with intent visible at every level. The Linux kernel style guide puts it bluntly: â€œC is a Spartan language, and so should your naming be.â€

**The markers of master-level C include:**

- **Function brevity**: SQLite enforces that **90%+ of functions stay under 30 lines**.  The kernel mandates fitting functions on one screen.
- **Shallow nesting**: Maximum 3 levels of indentation. Linus Torvalds: â€œIf you need more than 3 levels of indentation, youâ€™re screwed anyway.â€
- **Spartan naming**: Use `tmp` not `temporaryCounterVariable`. Loop indices are `i`, not `loopIterationIndex`.
- **Comments explain why, never what**: Antirez categorizes comments into 9 types; â€œtrivial commentsâ€ that state the obvious are explicitly forbidden.
- **Data-driven design with function pointers**: Pike calls this â€œthe heart of object-oriented programmingâ€ in Câ€”organizing operations around data types using function pointer tables.

Brian Kernighanâ€™s classic advice remains definitive: â€œWrite clearlyâ€”donâ€™t be too clever. Make it right before you make it faster. Make it clear before you make it faster.â€

## The minimal arena allocator every expert knows

Chris Wellonsâ€™ 7-line arena allocator represents the gold standard for elegant C memory management.  This pattern eliminates individual allocations and their associated bugs entirely:

```c
typedef struct { char *beg; char *end; } arena;

void *alloc(arena *a, ptrdiff_t size, ptrdiff_t align, ptrdiff_t count) {
    ptrdiff_t padding = -(uintptr_t)a->beg & (align - 1);
    ptrdiff_t available = a->end - a->beg - padding;
    if (available < 0 || count > available/size) abort();
    void *p = a->beg + padding;
    a->beg += padding + count*size;
    return memset(p, 0, count*size);
}

#define new(a, t, n)  (t *)alloc(a, sizeof(t), _Alignof(t), n)
```

**Key design decisions experts appreciate:**

The alignment calculation `-(uintptr_t)a->beg & (align - 1)` computes padding in a single expression without conditionals. Using **signed sizes** (`ptrdiff_t`) prevents subtle underflow bugs endemic to `size_t`.  Zero-initialization by default eliminates uninitialized memory bugs at minimal cost.

For word frequency counting, arenas shine: allocate all strings and hash table entries from a single arena, then free everything with one reset. Wellonsâ€™ pass-by-value scratch pattern provides automatic cleanupâ€”pass the arena by value for temporary allocations, and modifications die with the stack frame.

## Hash tables: FNV-1a and linear probing win on elegance

For string keys in a word frequency counter, **FNV-1a** is the expert consensus choice. Its core is one line:

```c
#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME  1099511628211UL

static uint64_t hash_key(const char *key) {
    uint64_t hash = FNV_OFFSET;
    for (const char *p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }
    return hash;
}
```

Ben Hoytâ€™s benchmarks show FNV-1a achieves **1.40 average probe length** on English wordsâ€”excellent distribution with trivial implementation. For hash-flooding protection (e.g., web servers), SipHash is required, but for trusted input like file processing, FNV-1aâ€™s simplicity wins.

**Open addressing with linear probing** is universally preferred for performance-critical hash tables.  All high-performance implementations use it because data stays contiguousâ€”no pointer chasing through scattered linked list nodes. The implementation is remarkably compact:

```c
typedef struct { const char *key; size_t count; } entry;
typedef struct { entry *entries; size_t capacity, length; } ht;

entry *ht_get(ht *t, const char *key) {
    uint64_t h = hash_key(key);
    size_t i = h & (t->capacity - 1);  // Power-of-2 capacity for fast modulo
    while (t->entries[i].key) {
        if (strcmp(key, t->entries[i].key) == 0) return &t->entries[i];
        i = (i + 1) & (t->capacity - 1);
    }
    return NULL;
}
```

Keep load factor below **0.7** and double capacity when exceeded. For a word counter without deletions, tombstones arenâ€™t neededâ€”one fewer complication.

## C11 atomics: simplicity over lock-free heroics

Bruce Dawsonâ€™s warning should guide threading decisions: â€œLock-free programming is the most dangerous hammer in the C++ toolkit, and it is rarely appropriate.â€  Jeff Preshing, the authority on lock-free programming,  built his â€œworldâ€™s simplest lock-free hash tableâ€ by deliberately removing featuresâ€”no delete, fixed size, integer keys only.

**For a word frequency counter, use mutexes unless profiling proves contention.** C11â€™s `<threads.h>` provides portable threading:

```c
#include <threads.h>
static mtx_t table_lock;

void init(void) { mtx_init(&table_lock, mtx_plain); }
void increment_word(const char *word) {
    mtx_lock(&table_lock);
    // ... hash table operation
    mtx_unlock(&table_lock);
}
```

If profiling reveals contention, the elegant solution is **per-bucket locks** (shard the table), not full lock-free implementation. Lock-free code that works on x86 often fails on ARM due to weaker memory orderingâ€”a common trap.

When atomics are warranted, **default to `memory_order_seq_cst`** until measurement proves otherwise. The producer/consumer pattern using acquire/release is the only common case worth optimizing:

```c
// Thread 1 (writer)
atomic_store_explicit(&ready, 1, memory_order_release);

// Thread 2 (reader)  
while (!atomic_load_explicit(&ready, memory_order_acquire)) { }
// Data is now visible
```

## Cross-platform memory mapping without preprocessor spaghetti

The pattern from SQLiteâ€™s VFS and libuv is **file-based separation**: put Unix code in `mmap_unix.c`, Windows code in `mmap_win32.c`, expose a clean API header with no platform includes.  This avoids the nested `#ifdef` hell that makes codebases unmaintainable.

```c
// mmap_portable.h - NO platform headers
typedef struct mapped_file {
    void *data;
    size_t size;
    void *_handle;  // Opaque platform handle
} mapped_file_t;

int mfile_open(mapped_file_t *mf, const char *path, int readonly);
int mfile_close(mapped_file_t *mf);
```

Each platform file implements the same functions, compiled conditionally via the build systemâ€”not inline `#ifdef` blocks.  SQLite takes this further with function pointer tables (the VFS struct) enabling runtime swapping of implementations.

For simple needs, the **mman-win32** library provides POSIX `mmap()` on Windows with minimal overhead. LMDB demonstrates that building an entire database around mmap is viableâ€”zero-copy reads directly from the mapped region.

## Error handling: goto cleanup is canonical

The Linux kernelâ€™s goto cleanup pattern is the expert consensus for C error handling. Itâ€™s not â€œbad practiceâ€â€”itâ€™s the cleanest way to handle multiple resources:

```c
int process_file(const char *path) {
    int ret = -1;
    FILE *f = NULL;
    char *buf = NULL;
    
    f = fopen(path, "r");
    if (!f) goto cleanup;
    
    buf = malloc(4096);
    if (!buf) goto cleanup;
    
    // ... processing
    ret = 0;
    
cleanup:
    free(buf);
    if (f) fclose(f);
    return ret;
}
```

The kernel style guide explains: â€œUnconditional statements are easier to understand and follow. Nesting is reduced. Errors by not updating individual exit points are prevented.â€

**Initialize all pointers to NULL at function start** for safe conditional cleanup. Return 0 for success, negative errno for failure (kernel convention), or use SQLiteâ€™s pattern of integer error codes with a separate error message function.

## Putting it together: structure for a word frequency counter

An elegant implementation combines these patterns:

1. **Memory**: Single arena for all allocationsâ€”strings, hash entries, everything. One `arena_reset()` at program end.
1. **Hash table**: Open addressing, linear probing, FNV-1a hash, power-of-2 capacity with 0.7 load factor limit.
1. **File I/O**: Memory-map the input file for zero-copy access. Platform abstraction via separate implementation files.
1. **Threading**: Start with single-threaded. If needed, shard the hash table with per-shard mutexes.
1. **Error handling**: Goto cleanup pattern, negative return codes.
1. **Style**: 8-character tabs, 80-column lines,  functions under 30 lines, comments explaining non-obvious decisions.

The hallmark of expert C isnâ€™t feature densityâ€”itâ€™s knowing what to leave out. Redis builds with one `make` command.  SQLite ships as a single amalgamation file. Antirezâ€™s kilo editor is under 1000 lines.  An elegant word frequency counter should demonstrate the same restraint: **minimal dependencies, obvious data flow, code that reads like prose**.

## Conclusion

Expert-level C elegance emerges from constraints, not capabilities. The 30-year veterans you want to impress will notice **what you didnâ€™t do**: no unnecessary abstractions, no clever tricks requiring comments, no memory management complexity beyond an arena, no lock-free heroics without profiling data.

Pikeâ€™s rules remain the foundation: data dominates, simple algorithms beat fancy ones, measure before optimizing.   The kernelâ€™s 3-level nesting limit  and SQLiteâ€™s 30-line function guideline  enforce readability mechanically. FNV-1a and linear probing solve hashing with minimal code. Arenas eliminate allocation bugs by making memory management impossible to get wrong.

The experts quoted hereâ€”Torvalds, Pike, Kernighan, Sanfilippoâ€”converge on the same principle: clarity is the highest virtue. Code that requires cleverness to understand is not clever code. A word frequency counter that demonstrates this understanding, with clean data flow and obvious intent at every line, will impress seasoned engineers far more than one with exotic optimizations.

# The craft of genius-level C: patterns from the masters

**What separates a 30-year C veteranâ€™s â€œthis is poetryâ€ reaction from mere professional competence?** After researching the code and philosophies of Fabrice Bellard, Mike Pall, Ken Thompson, Rob Pike, Dan Bernstein, Rich Felker, and Salvatore Sanfilippo (antirez), a coherent picture emerges: genius-level C is characterized by *ruthless simplicity*, *data-centric design*, *single-pass elegance*, and *self-contained minimalism*. These masters donâ€™t write clever codeâ€”they write code so clear it appears inevitable.

The McIlroy vs. Knuth story crystallizes this perfectly. When Jon Bentley asked Donald Knuth to write a literate program for word frequency counting, Knuth produced 10 pages of beautiful, heavily-documented Pascal with custom trie data structures.  Doug McIlroy responded with six lines of shell: `tr -cs A-Za-z '\n' | tr A-Z a-z | sort | uniq -c | sort -rn | sed ${1}q`.  McIlroyâ€™s devastating critique: â€œKnuth has fashioned a sort of **industrial-strength FabergÃ© egg**â€”intricate, wonderfully worked, refined beyond all ordinary desires, a museum piece from the start.â€

-----

## â€œData dominatesâ€: the universal first principle

Rob Pikeâ€™s Rule 5 from â€œNotes on Programming in Câ€ captures what every master believes: â€œIf youâ€™ve chosen the right data structures and organized things well, the algorithms will almost always be self-evident. **Data structures, not algorithms, are central to programming.**â€   Ken Thompson rephrased Rules 3 and 4 as: â€œWhen in doubt, use brute force.â€

Pike lists the only data structures needed for â€œalmost all practical programsâ€: **array, linked list, hash table, binary tree**.  Thatâ€™s it.  Fabrice Bellardâ€™s TinyCCâ€”a complete C compiler in ~100KBâ€”relies on a simple value stack (`vstack`) that pushes expression results as theyâ€™re parsed.  Mike Pallâ€™s LuaJIT uses power-of-two hash tables  with Brentâ€™s variation. Antirezâ€™s Redis dictionary uses chaining with incremental rehashing.  None of these masters invented novel data structures; they chose the *right* simple structure and implemented it perfectly.

For a word frequency counter, this means: **a hash table with chaining, power-of-two sizing, and inline word storage**. Nothing fancier. The genius is in the execution, not the choice.

-----

## Single-pass processing: the Bellard pattern

Fabrice Bellardâ€™s signature technique is **eliminating intermediate representations**. TinyCC generates linked binary code directlyâ€”no AST, no IR, just parser to machine code. QuickJS compiles JavaScript to bytecode without building a syntax tree. This isnâ€™t premature optimization; itâ€™s *architectural simplicity*.

The TinyCC value stack pattern demonstrates this elegantly:

```c
typedef struct SValue {
    int t;      /* type */
    int r;      /* storage: register, constant, stack, or flags */
    union { ... } c;  /* constant value if applicable */
} SValue;
```

When an expression is parsed, its value is pushed onto `vstack`. The function `gv(rc)` generates code to materialize `vtop` into registersâ€”Bellard calls it â€œthe *most important function* of the code generator.â€ No separate analysis pass, no tree traversal. Parse and emit in one motion.

**For word frequency**: Read character-by-character, accumulating into a buffer while simultaneously computing the hash. When a word boundary is detected, lookup/insert into the hash table immediately. No tokenization pass, no separate hashing pass. One loop, one traversal of input.

-----

## The flexible array member: inline storage for small objects

Every master uses the `char str[1]` pattern (now `char str[]` in C99) for storing variable-length data inline with its metadata. Antirezâ€™s SDS strings place the string data immediately after the length header:

```c
struct sdshdr {
    int len;    /* current length */
    int free;   /* available buffer space */
    char buf[]; /* string follows immediately */
};
```

The returned pointer points to `buf`, so `printf("%s", sds_string)` works directly,  but you can always walk backwards to find metadata: `((struct sdshdr *)(s - sizeof(struct sdshdr)))->len`. This is O(1) length lookup  with zero indirection and perfect cache locality.

Bellardâ€™s TinyCC uses identical patterns for TokenSym and other structures. For a word frequency counter:

```c
typedef struct Entry {
    struct Entry *next;
    size_t count;
    size_t hash;
    char word[];  /* word stored inline */
} Entry;

Entry *entry_new(const char *w, size_t len, size_t hash) {
    Entry *e = malloc(sizeof(Entry) + len + 1);
    e->next = NULL; e->count = 1; e->hash = hash;
    memcpy(e->word, w, len);
    e->word[len] = '\0';
    return e;
}
```

**One allocation per word, perfect locality, zero pointer chasing for the common lookup path.**

-----

## Hash table wisdom: simplicity over cleverness

The masters converge on remarkably similar hash table designs:

**Power-of-two sizing with bitmask**: `bucket = hash & (size - 1)` replaces expensive modulo division. Redis, musl, and LuaJIT all use this. Resizing doubles the table, preserving the mask operation.

**Store the hash**: Antirezâ€™s Redis stores the precomputed hash in each entry. On collision chains, compare hashes before stringsâ€”most mismatches are caught by a single integer comparison.

**Simple hash functions**: Bellard uses `h = h * 31 + c`. Antirez uses DJB2 (`h = ((h << 5) + h) + c`). Rich Felker uses bit manipulation for string operations. None use cryptographic hashes for frequency countingâ€”save complexity for when it matters.

**Chaining over open addressing**: Every master prefers chaining for its simplicity.   Open addressing has better cache behavior at high load factors but introduces tombstone complexity and resize pain.  For a small utility, chaining wins.

**Incremental rehashing** (if you need it): Redis uses two hash tables, migrating entries gradually during operations to avoid blocking.  For a word counter processing a finite file, batch rehashing is fine.

-----

## Variable naming: short is beautiful

Rob Pikeâ€™s rule is unambiguous: â€œLength is not a virtue in a name; clarity of expression *is*.â€ Loop indices should be `i`, not `elementIndex`.  Pointers can be `np` (node pointer), `s` (string), `d` (destination). The context makes meaning clear.

Rich Felker in musl: â€œSingle-letter variable names are not frowned upon as long as their scope is reasonably short. For a function less than 20 lines, itâ€™s very reasonable for all local variables to have single-letter names.â€

Pike specifically attacks camelCase: â€œI eschew embedded capital letters in names; to my prose-oriented eyes, they are too awkward to read comfortably. They jangle like bad typography.â€  Plan 9 style: all lowercase, no underscores for locals.

**Function names** follow the opposite rule: be descriptive about what they *return* (not what they do). Pike: â€œProcedure names should reflect what they do; function names should reflect what they return.â€ Bad: `checksize(x)`. Good: `validsize(x)`.

-----

## Comments: the antirez philosophy

Antirez identifies **nine types of comments**â€”six valuable, three harmful:

**Valuable**:

1. **Function comments**: API documentation inline with code
1. **Design comments**: At file top, explain approach and rejected alternatives
1. **Why comments**: Explain reasoning that isnâ€™t obvious from code
1. **Teacher comments**: Embed domain knowledge
1. **Checklist comments**: Remind about related changes
1. **Guide comments**: Lower cognitive load, divide code into scannable sections

**Harmful**:

1. Trivial comments (`i++; /* increment i */`)
1. Debt comments (TODO/FIXMEâ€”should be tracked elsewhere)
1. Backup comments (commented-out codeâ€”use git)

Pike takes a stronger stance: â€œI tend to err on the side of eliminating commentsâ€ because comments arenâ€™t checked by the compiler and clutter code.  The masters agree on this: **good code should be self-documenting through clear structure and naming, with comments reserved for *why*, not *what***.

For a word frequency counter, a good comment style:

```c
/* DESIGN
 * Hash table with chaining. Words stored inline with entries.
 * Single-pass tokenization: read, hash, and insert in one loop.
 * Output sorted by frequency using qsort on pointer array.
 */
```

-----

## Error handling: fail fast, fail loud

The masters split into two camps:

**Plan 9/Unix style**: Functions return -1 on error, 0+ on success. Set `errstr` for details. Check every return value, propagate errors up. This is defensive but readable.

**djb style**: Functions return 0 on failure, 1 on success. Never crash. Never use global errno. Leave data structures unchanged on failure. The stralloc interface embodies this: `stralloc_cats(&sa, "hello")` returns 1 on success, 0 on allocation failure, and leaves `sa` valid either way.

**For small utilities**, both Pike and Bellard favor a simpler pattern: check allocations, and if critical resources fail, just `die("message")` with a clear error. Youâ€™re not writing a libraryâ€”recovery from malloc failure in a word counter is academic.

```c
static void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

Entry *e = malloc(sizeof(Entry) + len + 1);
if (!e) die("out of memory");
```

-----

## Bit manipulation idioms from musl

Rich Felkerâ€™s musl libc demonstrates bit tricks that achieve performance without sacrificing clarity:

```c
#define ONES ((size_t)-1/UCHAR_MAX)         /* 0x0101... */
#define HIGHS (ONES * (UCHAR_MAX/2+1))      /* 0x8080... */
#define HASZERO(x) ((x)-ONES & ~(x) & HIGHS)
```

`HASZERO(x)` detects if any byte in a word is zeroâ€”without branching. This enables word-at-a-time `strlen` that processes 4 or 8 bytes per iteration while remaining portable.

For word frequency, similar tricks can optimize character classification:

```c
/* ASCII letter check: 'A'-'Z' = 65-90, 'a'-'z' = 97-122 */
static inline int is_alpha(int c) {
    return ((unsigned)c | 32) - 'a' < 26;  /* handles both cases */
}

static inline int to_lower(int c) {
    return c | (32 * is_alpha(c));  /* branchless */
}
```

-----

## Code density without obscurity: the djb approach

TweetNaCl fits serious cryptography into ~100 tweets. The techniques:

**Aggressive type abbreviation**:

```c
typedef unsigned char u8;
typedef unsigned long long u64;
typedef i64 gf[16];
```

**Macro-compressed loops**:

```c
#define FOR(i,n) for(i=0;i<n;++i)
```

**Single-line function bodies**:

```c
static u32 L32(u32 x,int c){return(x<<c)|(x>>(32-c));}
```

**Critical distinction**: This density serves *auditing*, not obfuscation. TweetNaCl fits on one printed page, enabling formal verification. Every line is essential. This is the opposite of Knuthâ€™s FabergÃ© eggâ€”itâ€™s a perfectly machined tool where nothing can be removed.

-----

## The kilo pattern: structure for small programs

Antirezâ€™s kilo text editorâ€”a full editor in ~1000 linesâ€”demonstrates how to structure small programs elegantly:

**Single global state struct**:

```c
struct config {
    int cx, cy;           /* cursor position */
    int numrows;
    erow *row;            /* array of row structures */
    char *filename;
    /* ... */
} E;
```

This eliminates parameter-passing overhead and makes state explicit. For a word counter:

```c
struct {
    Entry **buckets;
    size_t size, mask, used;
    char buf[MAXWORD];    /* word accumulator */
    size_t buflen;
} G;
```

**Functional decomposition by concern**: Kilo organizes into terminal handling (~100 lines), row operations (~200), editor ops (~100), file I/O (~50), search (~50), highlighting (~150), input/output (~200), main (~50). Each section is independently understandable.

**Append buffer for output**: Build output in memory, write once:

```c
struct abuf { char *b; int len; };
void abAppend(struct abuf *ab, const char *s, int len);
```

This minimizes syscalls and enables atomic screen updates.

-----

## What makes a 30-year veteran say â€œthis is poetryâ€

Synthesizing across all masters, the hallmarks of genius-level C:

**Inevitability**: The solution feels like the *only* way to solve the problem. No alternatives seem simpler. Every line pulls its weight.

**Transparency**: Understanding happens on first read, not after â€œcleverâ€ analysis. As Pike says: â€œA program is a sort of publication.â€

**Appropriate abstraction**: Not zero abstraction (thatâ€™s assembly) or maximal abstraction (thatâ€™s enterprise Java). Just enough structure to manage complexity, no more.

**Density without compression artifacts**: Short code that remains clear. TweetNaCl is dense but every expert can follow it. Contrast with obfuscated C contest entriesâ€”dense but deliberately opaque.

**Self-containment**: No external dependencies to understand. Bellardâ€™s TCC includes its own assembler, linker, preprocessor. Antirezâ€™s kilo uses raw VT100 escapes instead of ncurses.

**Data-centric organization**: The data structures are the design document. Once you understand `SValue` in TCC or `sds` in Redis, the code becomes obvious.

-----

## Concrete patterns for a word frequency counter

Based on this research, hereâ€™s how to write a word frequency counter that would make these masters nod:

**Architecture**:

- Single C file, ~200-400 lines
- One global state struct
- Hash table with chaining, power-of-two size
- Words stored inline via flexible array members
- Single-pass input processing: read, lowercase, hash, insert
- qsort on pointer array for frequency sorting
- Direct output with minimal buffering

**Style**:

- Short variable names: `s`, `h`, `e`, `n`
- Functions named for returns: `entry_new`, `table_lookup`, `table_grow`
- Guide comments between sections
- One â€œDESIGNâ€ comment at top explaining approach
- Check malloc, die on failure
- No headers except standard library

**Data structures**:

```c
typedef struct Entry {
    struct Entry *next;
    size_t count, hash;
    char word[];
} Entry;

typedef struct {
    Entry **buckets;
    size_t size, mask, used;
} Table;
```

**The loop**:

```c
size_t h = SEED;
int len = 0;
while ((c = getchar()) != EOF) {
    if (is_alpha(c)) {
        c = to_lower(c);
        buf[len++] = c;
        h = h * 31 + c;
    } else if (len > 0) {
        table_insert(&T, buf, len, h);
        len = 0; h = SEED;
    }
}
```

This embodies the McIlroy spiritâ€”not by piping to shell utilities, but by achieving the same conceptual simplicity in C: one pass, one purpose, no unnecessary structure.

-----

## The mastersâ€™ final lesson

The McIlroy-Knuth story ends with McIlroyâ€™s observation: â€œVery few people can obtain the virtuoso services of Knuthâ€¦ But old UNIX hands know instinctively how to solve this one in a jiffy.â€

The genius of these C masters isnâ€™t extraordinary clevernessâ€”itâ€™s extraordinary *taste*. They know which complexity to embrace (careful data structure design, bit manipulation where it matters, precise memory layout) and which to reject (unnecessary abstraction, defensive over-engineering, premature generalization).

A word frequency counter written in this tradition wonâ€™t be a museum piece. Itâ€™ll be a sharp toolâ€”fit for purpose, understandable at a glance, and finished in an afternoon rather than a week. Thatâ€™s the hacker elegance the masters demonstrate: solving the problem in front of you with the minimum mechanism that does the job correctly.