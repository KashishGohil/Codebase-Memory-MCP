#include "cbm.h"
#include "arena.h" // CBMArena, cbm_arena_init/alloc/strdup/destroy
#include "helpers.h"
#include "lang_specs.h"
#include "extract_unified.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/perl_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/cs_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/rust_lsp.h"
#include "preprocessor.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"  // cbm_fopen — crash-supervisor per-file marker write
#include "foundation/hash_table.h" // CBMHashTable — crash-supervisor quarantine set
#include "tree_sitter/api.h" // TSParser, TSNode, TSTree, TSInput, TSLanguage, TSPoint, TSParseOptions, TSParseState
#include "foundation/constants.h"
#include "mimalloc.h" // mi_malloc/mi_calloc/mi_realloc/mi_free/mi_usable_size — bind 3rd-party allocators (#424)
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include "sqlite3.h" // sqlite3_mem_methods, sqlite3_config, SQLITE_CONFIG_MALLOC — bind sqlite to mimalloc
#endif
#include <stdint.h> // uint32_t, uint64_t, int64_t
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h> // struct timespec, CLOCK_MONOTONIC

// Atomic counters for profiling parse vs extraction time (nanoseconds).
// Accessed from multiple threads; using _Atomic for safe accumulation.
#include <stdatomic.h>
static _Atomic uint64_t total_parse_ns = 0;
static _Atomic uint64_t total_extract_ns = 0;
static _Atomic uint64_t total_lsp_ns = 0;
static _Atomic uint64_t total_preprocess_ns = 0;
static _Atomic uint64_t total_files_preprocessed = 0;
static _Atomic uint64_t total_files = 0;

// C/C++ preprocessor #define macros are extracted as Macro nodes (#375). On a
// macro-dense codebase (e.g. the Linux kernel: ~2.4M macros, 49% of all nodes)
// this is the dominant extraction cost, so it is gated to the full/advanced
// index modes. Default ON to preserve behavior for direct callers/tests; the
// pipeline sets it from the index mode before extraction. Set once pre-extract,
// read-only during, so a relaxed atomic is sufficient.
static _Atomic int g_extract_macros = 1;
void cbm_set_macro_extraction(int enabled) {
    atomic_store_explicit(&g_extract_macros, enabled ? 1 : 0, memory_order_relaxed);
}
int cbm_macro_extraction_enabled(void) {
    return atomic_load_explicit(&g_extract_macros, memory_order_relaxed);
}

#define NSEC_PER_SEC 1000000000ULL
#define USEC_TO_NSEC 1000ULL
/* Use compat.h's cbm_clock_gettime which accepts CLOCK_MONOTONIC (value
 * varies by platform: 1 on Linux/Windows, 6 on macOS). We pass the
 * platform value via the compat.h fallback. */
#if defined(CLOCK_MONOTONIC)
#define CBM_CLOCK_MONO CLOCK_MONOTONIC
#elif defined(__APPLE__)
#define CBM_CLOCK_MONO 6
#else
#define CBM_CLOCK_MONO 1
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    cbm_clock_gettime(CBM_CLOCK_MONO, &ts);
    return ((uint64_t)ts.tv_sec * NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

// cbm_get_profile returns accumulated parse/extract times and file count.
void cbm_get_profile(cbm_profile_out_t out) {
    *out.parse_ns = atomic_load(&total_parse_ns);
    *out.extract_ns = atomic_load(&total_extract_ns);
    *out.files = atomic_load(&total_files);
}

uint64_t cbm_get_lsp_ns(void) {
    return atomic_load(&total_lsp_ns);
}

uint64_t cbm_get_preprocess_ns(void) {
    return atomic_load(&total_preprocess_ns);
}

uint64_t cbm_get_files_preprocessed(void) {
    return atomic_load(&total_files_preprocessed);
}

// cbm_reset_profile zeros the profiling counters.
void cbm_reset_profile(void) {
    atomic_store(&total_parse_ns, 0);
    atomic_store(&total_extract_ns, 0);
    atomic_store(&total_lsp_ns, 0);
    atomic_store(&total_preprocess_ns, 0);
    atomic_store(&total_files_preprocessed, 0);
    atomic_store(&total_files, 0);
}

// --- Growable array push functions ---

#define GROW_ARRAY(arr, arena)                                                                   \
    do {                                                                                         \
        if ((arr)->count >= (arr)->cap) {                                                        \
            int new_cap = (arr)->cap == 0 ? CBM_SZ_32 : (arr)->cap * PAIR_LEN;                   \
            void *new_items = cbm_arena_alloc((arena), (size_t)new_cap * sizeof(*(arr)->items)); \
            if (!new_items)                                                                      \
                return;                                                                          \
            if ((arr)->items && (arr)->count > 0) {                                              \
                memcpy(new_items, (arr)->items, (size_t)(arr)->count * sizeof(*(arr)->items));   \
            }                                                                                    \
            (arr)->items = new_items;                                                            \
            (arr)->cap = new_cap;                                                                \
        }                                                                                        \
    } while (0)

void cbm_defs_push(CBMDefArray *arr, CBMArena *a, CBMDefinition def) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = def;
}

void cbm_calls_push(CBMCallArray *arr, CBMArena *a, CBMCall call) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = call;
}

void cbm_imports_push(CBMImportArray *arr, CBMArena *a, CBMImport imp) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = imp;
}

void cbm_usages_push(CBMUsageArray *arr, CBMArena *a, CBMUsage usage) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = usage;
}

void cbm_throws_push(CBMThrowArray *arr, CBMArena *a, CBMThrow thr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = thr;
}

void cbm_rw_push(CBMRWArray *arr, CBMArena *a, CBMReadWrite rw) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rw;
}

static bool def_label_is_preprocessed_callable(const char *label) {
    return label && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0);
}

static bool cbm_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool cbm_plain_identifier(const char *s) {
    if (!s || (!isalpha((unsigned char)s[0]) && s[0] != '_')) {
        return false;
    }
    for (const char *p = s + 1; *p; p++) {
        if (!cbm_ident_char(*p)) {
            return false;
        }
    }
    return true;
}

static const char *cbm_last_qn_segment(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *dot = strrchr(qn, '.');
    return dot ? dot + 1 : qn;
}

static bool cbm_ident_at(const char *source, int source_len, int pos, const char *ident,
                         int ident_len) {
    if (pos < 0 || ident_len <= 0 || pos + ident_len > source_len) {
        return false;
    }
    if (pos > 0 && cbm_ident_char(source[pos - 1])) {
        return false;
    }
    if (pos + ident_len < source_len && cbm_ident_char(source[pos + ident_len])) {
        return false;
    }
    return strncmp(source + pos, ident, (size_t)ident_len) == 0;
}

static bool cbm_scope_before_name_matches(const char *source, int name_pos, const char *scope) {
    if (!scope || !scope[0]) {
        return true;
    }
    int p = name_pos;
    while (p > 0 && isspace((unsigned char)source[p - 1])) {
        p--;
    }
    if (p < 2 || source[p - 1] != ':' || source[p - 2] != ':') {
        return false;
    }
    p -= 2;
    while (p > 0 && isspace((unsigned char)source[p - 1])) {
        p--;
    }
    int end = p;
    while (p > 0 && cbm_ident_char(source[p - 1])) {
        p--;
    }
    size_t len = (size_t)(end - p);
    return len == strlen(scope) && strncmp(source + p, scope, len) == 0;
}

static int cbm_find_matching_paren(const char *source, int source_len, int open_pos) {
    int depth = 0;
    for (int i = open_pos; i < source_len; i++) {
        if (source[i] == '(') {
            depth++;
        } else if (source[i] == ')') {
            if (--depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

static int cbm_find_definition_open_brace(const char *source, int source_len, int after_paren) {
    for (int i = after_paren + 1; i < source_len; i++) {
        if (source[i] == '{') {
            return i;
        }
        if (source[i] == ';') {
            return -1; /* declaration, not definition */
        }
    }
    return -1;
}

static uint32_t cbm_line_for_byte(const char *source, int byte_pos) {
    uint32_t line = 1;
    for (int i = 0; i < byte_pos; i++) {
        if (source[i] == '\n') {
            line++;
        }
    }
    return line;
}

static bool cbm_source_line_bounds(const char *source, int source_len, uint32_t start_line,
                                   uint32_t end_line, int *out_start, int *out_end) {
    if (!source || source_len < 0 || start_line == 0 || end_line < start_line || !out_start ||
        !out_end) {
        return false;
    }
    uint32_t line = 1;
    int line_start = 0;
    int span_start = -1;
    int span_end = -1;
    for (int i = 0; i <= source_len; i++) {
        if (i < source_len && source[i] != '\n') {
            continue;
        }
        if (line == start_line) {
            span_start = line_start;
        }
        if (line == end_line) {
            span_end = i;
            break;
        }
        line++;
        line_start = i + 1;
    }
    if (span_start < 0 || span_end < span_start) {
        return false;
    }
    *out_start = span_start;
    *out_end = span_end;
    return true;
}

static bool cbm_name_context_allows_definition(const char *source, int line_start, int name_pos) {
    int p = name_pos;
    while (p > line_start && isspace((unsigned char)source[p - 1])) {
        p--;
    }
    if (p == line_start) {
        return true;
    }
    switch (source[p - 1]) {
    case '(':
    case ',':
    case '=':
    case '!':
    case '?':
    case '[':
    case '.':
        return false;
    default:
        return true;
    }
}

static bool cbm_original_span_contains_callable_def(const CBMDefinition *def, const char *source,
                                                    int source_len) {
    if (!def || !source || source_len <= 0 || !cbm_plain_identifier(def->name)) {
        return false;
    }
    int span_start = 0;
    int span_end = 0;
    if (!cbm_source_line_bounds(source, source_len, def->start_line, def->end_line, &span_start,
                                &span_end)) {
        return false;
    }
    const char *scope = NULL;
    if (def->label && strcmp(def->label, "Method") == 0) {
        scope = cbm_last_qn_segment(def->parent_class);
    }
    int name_len = (int)strlen(def->name);
    for (int i = span_start; i + name_len <= span_end; i++) {
        if (!cbm_ident_at(source, source_len, i, def->name, name_len)) {
            continue;
        }
        if (!cbm_scope_before_name_matches(source, i, scope)) {
            continue;
        }
        if (!cbm_name_context_allows_definition(source, span_start, i)) {
            continue;
        }
        int p = i + name_len;
        while (p < span_end && isspace((unsigned char)source[p])) {
            p++;
        }
        if (p >= span_end || source[p] != '(') {
            continue;
        }
        int close_paren = cbm_find_matching_paren(source, span_end, p);
        if (close_paren < 0) {
            continue;
        }
        return cbm_find_definition_open_brace(source, span_end, close_paren) >= 0;
    }
    return false;
}

static bool cbm_remap_preprocessed_callable_lines(CBMDefinition *def,
                                                  const CBMPreprocessedSource *pp,
                                                  const char *original_source,
                                                  int original_source_len) {
    if (!def || !pp || !pp->original_line_by_expanded_line || !pp->belongs_to_main_file ||
        def->start_line == 0 || def->end_line < def->start_line ||
        def->end_line > (uint32_t)pp->expanded_line_count) {
        return false;
    }
    uint32_t original_start = pp->original_line_by_expanded_line[def->start_line];
    uint32_t original_end = pp->original_line_by_expanded_line[def->end_line];
    if (!original_start || !original_end || original_end < original_start) {
        return false;
    }
    for (uint32_t line = def->start_line; line <= def->end_line; line++) {
        if (!pp->belongs_to_main_file[line] || !pp->original_line_by_expanded_line[line]) {
            return false;
        }
    }
    def->start_line = original_start;
    def->end_line = original_end;
    def->lines = (int)(def->end_line - def->start_line + 1);
    return cbm_original_span_contains_callable_def(def, original_source, original_source_len);
}

typedef struct {
    uint32_t start_line;
    uint32_t end_line;
    const char *label;
    const char *name;
    const char *parent_class;
} CBMRecoveredCallable;

typedef struct {
    CBMRecoveredCallable *items;
    int count;
    int cap;
} CBMRecoveredCallableArray;

static bool cbm_recovered_callables_push(CBMArena *arena, CBMRecoveredCallableArray *arr,
                                         const CBMDefinition *def) {
    if (!arena || !arr || !def) {
        return false;
    }
    if (arr->count >= arr->cap) {
        int new_cap = arr->cap ? arr->cap * 2 : 16;
        CBMRecoveredCallable *items =
            (CBMRecoveredCallable *)cbm_arena_alloc(arena, sizeof(*items) * (size_t)new_cap);
        if (!items) {
            return false;
        }
        if (arr->items && arr->count > 0) {
            memcpy(items, arr->items, sizeof(*items) * (size_t)arr->count);
        }
        arr->items = items;
        arr->cap = new_cap;
    }
    arr->items[arr->count++] = (CBMRecoveredCallable){
        .start_line = def->start_line,
        .end_line = def->end_line < def->start_line ? def->start_line : def->end_line,
        .label = def->label,
        .name = def->name,
        .parent_class = def->parent_class,
    };
    return true;
}

static int result_find_same_def_identity(const CBMFileResult *result, const CBMDefinition *def) {
    if (!result || !def || !def->label || !def->qualified_name) {
        return -1;
    }
    for (int i = 0; i < result->defs.count; i++) {
        const CBMDefinition *cur = &result->defs.items[i];
        if (!cur->label || !cur->qualified_name) {
            continue;
        }
        if (strcmp(cur->label, def->label) == 0 &&
            strcmp(cur->qualified_name, def->qualified_name) == 0) {
            return i;
        }
    }
    return -1;
}

static void merge_missing_preprocessed_callables(CBMFileResult *dst, const CBMFileResult *src,
                                                 CBMArena *arena,
                                                 const CBMPreprocessedSource *preprocessed,
                                                 const char *original_source,
                                                 int original_source_len,
                                                 CBMRecoveredCallableArray *recovered) {
    if (!dst || !src) {
        return;
    }
    for (int i = 0; i < src->defs.count; i++) {
        const CBMDefinition *def = &src->defs.items[i];
        if (!def->qualified_name || !def->name) {
            continue;
        }
        if (!def_label_is_preprocessed_callable(def->label)) {
            continue;
        }
        CBMDefinition remapped = *def;
        if (!cbm_remap_preprocessed_callable_lines(&remapped, preprocessed, original_source,
                                                   original_source_len)) {
            continue;
        }
        /* Successful expanded-source remaps are parse-coverage evidence. Raw
         * definitions remain primary; the final defs array is fill-only below. */
        if (!cbm_recovered_callables_push(arena, recovered, &remapped)) {
            continue;
        }
        if (result_find_same_def_identity(dst, &remapped) < 0) {
            cbm_defs_push(&dst->defs, arena, remapped);
        }
    }
}

void cbm_typerefs_push(CBMTypeRefArray *arr, CBMArena *a, CBMTypeRef tr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = tr;
}

void cbm_envaccess_push(CBMEnvAccessArray *arr, CBMArena *a, CBMEnvAccess ea) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ea;
}

void cbm_typeassign_push(CBMTypeAssignArray *arr, CBMArena *a, CBMTypeAssign ta) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ta;
}

void cbm_stringref_push(CBMStringRefArray *arr, CBMArena *a, CBMStringRef sr) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = sr;
}

void cbm_infrabinding_push(CBMInfraBindingArray *arr, CBMArena *a, CBMInfraBinding ib) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ib;
}

void cbm_impltrait_push(CBMImplTraitArray *arr, CBMArena *a, CBMImplTrait it) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = it;
}

void cbm_resolvedcall_push(CBMResolvedCallArray *arr, CBMArena *a, CBMResolvedCall rc) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = rc;
}

void cbm_channels_push(CBMChannelArray *arr, CBMArena *a, CBMChannel ch) {
    GROW_ARRAY(arr, a);
    arr->items[arr->count++] = ch;
}

// --- String input reader (for parse_with_options) ---

typedef struct {
    const char *string;
    uint32_t length;
} CBMStringInput;

static const char *cbm_string_read(void *payload, uint32_t byte, TSPoint point,
                                   uint32_t *bytes_read) {
    (void)point;
    CBMStringInput *self = (CBMStringInput *)payload;
    if (byte >= self->length) {
        *bytes_read = 0;
        return "";
    }
    *bytes_read = self->length - byte;
    return self->string + byte;
}

// --- Parse timeout callback ---

static bool cbm_timeout_cb(TSParseState *state) {
    uint64_t deadline = *(uint64_t *)state->payload;
    return now_ns() > deadline;
}

// --- Thread-local parser pool ---
// TSParser is not thread-safe, but can be reused across files on the same thread.
// We keep one parser per thread, and just switch language as needed.
// This avoids ~70K ts_parser_new()/ts_parser_delete() cycles on large repos.

static CBM_TLS TSParser *tl_parser = NULL;
static CBM_TLS CBMLanguage tl_parser_lang = CBM_LANG_COUNT; // invalid sentinel

// Get or create a thread-local parser configured for the given language.
static TSParser *get_thread_parser(const TSLanguage *ts_lang, CBMLanguage lang) {
    if (!tl_parser) {
        tl_parser = ts_parser_new();
        if (!tl_parser) {
            return NULL;
        }
        tl_parser_lang = CBM_LANG_COUNT;
    }
    if (tl_parser_lang != lang) {
        ts_parser_set_language(tl_parser, ts_lang);
        tl_parser_lang = lang;
    }
    return tl_parser;
}

// --- Allocator binding (defense-in-depth, #424) ---

/* Bind tree-sitter and sqlite3 to mimalloc explicitly so a correct
 * binary does NOT depend on the fragile MI_OVERRIDE symbol override. Under
 * MI_OVERRIDE=1 — particularly the Windows static-MinGW link with
 * --allow-multiple-definition — `malloc`/`free` can resolve to DIFFERENT
 * allocators (mimalloc vs the CRT) inside third-party libs, so a block
 * allocated by mimalloc gets freed by the CRT (or vice-versa), corrupting the
 * heap freelist (#424). Binding each library through one explicit allocator
 * eliminates that mismatch class generically, on every platform.
 *
 * Guarded to the production build (CBM_BIND_TS_ALLOCATOR=1, which CFLAGS_PROD
 * defines alongside MI_OVERRIDE=1). The test build is CRT + ASan, where binding
 * to mimalloc would mismatch ASan/CRT frees — there these binds compile to
 * no-ops and the build stays unchanged. */

#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
#include <assert.h>

/* sqlite3 mem methods backed by mimalloc. sqlite's xMalloc/xRealloc/xSize use
 * `int` sizes; wrap with size_t casts. xRoundup rounds to an 8-byte boundary
 * (sqlite requires 8-byte-aligned roundup, and mimalloc honors that alignment).
 * Field order matches struct sqlite3_mem_methods exactly:
 * xMalloc, xFree, xRealloc, xSize, xRoundup, xInit, xShutdown, pAppData. */
static void *cbm_sqlite_malloc(int n) {
    return mi_malloc((size_t)n);
}
static void cbm_sqlite_free(void *p) {
    mi_free(p);
}
static void *cbm_sqlite_realloc(void *p, int n) {
    return mi_realloc(p, (size_t)n);
}
static int cbm_sqlite_size(void *p) {
    return (int)mi_usable_size(p);
}
static int cbm_sqlite_roundup(int n) {
    return (n + 7) & ~7; /* round up to 8-byte boundary */
}
static int cbm_sqlite_meminit(void *appdata) {
    (void)appdata;
    return SQLITE_OK;
}
static void cbm_sqlite_memshutdown(void *appdata) {
    (void)appdata;
}
#endif /* CBM_BIND_TS_ALLOCATOR */

void cbm_alloc_init(void) {
#if defined(CBM_BIND_TS_ALLOCATOR) && CBM_BIND_TS_ALLOCATOR
    static int alloc_bound = 0; /* single-threaded startup; plain int is fine */
    if (alloc_bound) {
        return;
    }
    alloc_bound = 1;

    /* tree-sitter runtime (was previously bound in cbm_init; consolidated here). */
    ts_set_allocator(mi_malloc, mi_calloc, mi_realloc, mi_free);

    /* sqlite3. SQLITE_CONFIG_MALLOC MUST run before sqlite3_initialize / the
     * first sqlite3_open* — otherwise sqlite3_config returns SQLITE_MISUSE
     * silently and the binding is ignored. cbm_alloc_init() runs as the very
     * first statement of main(), before cbm_mcp_server_new → cbm_store_open*. */
    static sqlite3_mem_methods cbm_sqlite_mem = {
        cbm_sqlite_malloc,      /* xMalloc */
        cbm_sqlite_free,        /* xFree */
        cbm_sqlite_realloc,     /* xRealloc */
        cbm_sqlite_size,        /* xSize */
        cbm_sqlite_roundup,     /* xRoundup */
        cbm_sqlite_meminit,     /* xInit */
        cbm_sqlite_memshutdown, /* xShutdown */
        NULL,                   /* pAppData */
    };
    int sqlite_rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &cbm_sqlite_mem);
    assert(sqlite_rc == SQLITE_OK && "SQLITE_CONFIG_MALLOC must run before sqlite3_initialize");
    (void)sqlite_rc;
#endif /* CBM_BIND_TS_ALLOCATOR */
}

// --- Init/Shutdown ---

static int cbm_initialized = 0;

int cbm_init(void) {
    if (cbm_initialized) {
        return 0;
    }
    enum { CBM_INIT_DONE = 1 };
    cbm_initialized = CBM_INIT_DONE;
    /* Defense-in-depth allocator binds (idempotent). main() calls cbm_alloc_init
     * first; this covers non-main entry points (pipeline passes call cbm_init).
     * For sqlite the SQLITE_CONFIG_MALLOC bind only takes effect if it runs
     * before sqlite initializes — main() guarantees that ordering; here it is a
     * best-effort idempotent re-assert for paths that never hit main(). */
    cbm_alloc_init();
    return 0;
}

void cbm_reset_thread_parser(void) {
    // Release parser's internal slab-allocated subtrees (stack, cached token).
    // Must be called BEFORE cbm_slab_reset_thread() to avoid corrupting
    // live slab chunks that the parser still references.
    if (tl_parser) {
        ts_parser_reset(tl_parser);
    }
}

void cbm_destroy_thread_parser(void) {
    // Full cleanup: delete the parser. Call on worker thread exit.
    if (tl_parser) {
        ts_parser_delete(tl_parser);
        tl_parser = NULL;
        tl_parser_lang = CBM_LANG_COUNT;
    }
}

void cbm_shutdown(void) {
    // Clean up thread-local parser for the calling thread.
    // Note: other threads' TLS parsers are freed when those threads exit.
    cbm_destroy_thread_parser();
    cbm_initialized = 0;
}

// --- Bottleneck call-name classification (language-agnostic heuristics) ---

// Case-insensitive equality for short callee names.
static bool name_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool name_in_set(const char *name, const char *const *set) {
    for (const char *const *s = set; *s; s++) {
        if (name_ieq(name, *s)) {
            return true;
        }
    }
    return false;
}

// Linear-scan / membership calls: a hit inside a loop is the textbook hidden
// O(n^2) (cf. Olivo et al., PLDI'15) that syntactic loop-depth alone misses.
static bool is_linear_scan_name(const char *n) {
    static const char *const set[] = {"find",    "indexof",   "contains", "includes", "search",
                                      "lookup",  "strstr",    "strchr",   "strrchr",  "memchr",
                                      "find_if", "findindex", "count",    "index",    NULL};
    return name_in_set(n, set);
}

// Allocation / growable-append calls: repeated inside a loop is the classic
// accidental reallocation / string-concat O(n^2). Names are deliberately
// conservative; meaningless in some languages → simply never matches there.
static bool is_alloc_name(const char *n) {
    static const char *const set[] = {"malloc",  "calloc",    "realloc",      "strdup", "strndup",
                                      "append",  "push_back", "emplace_back", "concat", "strcat",
                                      "strncat", "push",      "pushback",     NULL};
    return name_in_set(n, set);
}

// Extract the receiver identifier from a def's receiver text — Go's
// "(s *Store)" / "(s Store)" → "s". Stores the identifier start in *out and
// returns its length; returns 0 for unnamed receivers ("(*Store)", "(Store)"),
// where no second token follows the identifier (a lone token is the TYPE, not
// a name — such methods have no receiver variable to call through anyway).
static size_t receiver_ident(const char *recv_text, const char **out) {
    const char *p = recv_text;
    if (*p == '(') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *start = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
           *p == '_') {
        p++;
    }
    size_t len = (size_t)(p - start);
    if (len == 0) {
        return 0; // "(*Store)": leading '*', no identifier
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == ')' || *p == '\0') {
        return 0; // "(Store)": single token is the type, receiver unnamed
    }
    *out = start;
    return len;
}

// Whether a callee expression targets the same instance/class as the enclosing
// def, i.e. counts as genuine self-recursion rather than a same-named call on a
// different receiver. callee_name may be bare ("recur") or qualified
// ("self.recur", "this.recur", "super().save", "axios.get", "self.obj.recur").
//
// Bare names have no receiver → assume self-call (free function calling itself
// by bare name; preserves prior behavior). Qualified names: the receiver chain
// is everything before the LAST '.', and the WHOLE chain must name the same
// object — self/this/cls/@self, or the enclosing def's own receiver identifier
// (Go: `s` in `func (s *Store) save()`, from CBMDefinition.receiver). Matching
// the whole chain (not its first segment) keeps self.obj.recur() out: it
// targets self's FIELD obj, a different object. super() is the parent class and
// any other receiver (axios, console, ...) a different target. See #599.
static bool is_self_receiver(const char *callee_name, const char *def_receiver) {
    if (!callee_name || !callee_name[0]) {
        return false;
    }
    const char *dot = strrchr(callee_name, '.');
    if (!dot) {
        return true; // bare name → self-recursion candidate
    }
    size_t rlen = (size_t)(dot - callee_name);
    static const char *const self_receivers[] = {"self", "this", "cls", "@self", NULL};
    for (int i = 0; self_receivers[i]; i++) {
        size_t sl = strlen(self_receivers[i]);
        if (rlen == sl && strncmp(callee_name, self_receivers[i], sl) == 0) {
            return true;
        }
    }
    if (def_receiver) {
        const char *rid = NULL;
        size_t ril = receiver_ident(def_receiver, &rid);
        if (ril > 0 && ril == rlen && strncmp(callee_name, rid, ril) == 0) {
            return true; // call through the enclosing method's own receiver
        }
    }
    return false; // super() / axios / console / self.obj / any other receiver
}

// Count parameters from a signature string like "(int a, Foo* b, cb (*)(int,int))".
// Fallback for languages where param_names isn't populated (e.g. C keeps only the
// signature text). Counts commas at the top paren level; treats "()"/"(void)" as 0.
// Approximate by design (a structural smell, not an exact arity).
static int count_params_from_signature(const char *sig) {
    if (!sig) {
        return 0;
    }
    const char *p = sig;
    while (*p && *p != '(') {
        p++;
    }
    if (*p != '(') {
        return 0;
    }
    p++;
    const char *list = p;
    int depth = 0;
    int commas = 0;
    bool any = false;
    for (; *p; p++) {
        char ch = *p;
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if (ch == ')') {
            if (depth == 0) {
                break;
            }
            depth--;
        } else if (ch == ']' || ch == '}' || ch == '>') {
            if (depth > 0) {
                depth--;
            }
        } else if (ch == ',' && depth == 0) {
            commas++;
        } else if (!isspace((unsigned char)ch)) {
            any = true;
        }
    }
    if (!any) {
        return 0; /* "()" */
    }
    if (commas == 0) {
        while (*list == ' ' || *list == '\t') {
            list++;
        }
        if (strncmp(list, "void", 4) == 0 &&
            (list[4] == ')' || list[4] == ' ' || list[4] == '\0')) {
            return 0; /* C "(void)" */
        }
    }
    return commas + 1;
}

// --- Main extraction function ---

/* Test-only deterministic fault injection for the crash/hang supervisor tests.
 * Gated entirely behind env vars that are never set in production; a matching
 * rel_path either aborts (a fault signal the supervisor classifies as a crash)
 * or spins forever (an external-scanner infinite loop the quiet-timeout kills).
 * This gives an honest guard — green iff the supervisor actually contains a real
 * fault — instead of a fixture that may stop faulting once a root cause is fixed. */
/* Crash-supervisor per-file marker JOURNAL (Stage 3c skip-and-continue,
 * parallel-safe). Recovery re-runs are PARALLEL (there are no sequential
 * production runs), so a single overwrite-style marker would race across
 * workers and — worse — go stale during non-extract phases, blaming
 * whatever file was extracted LAST (that mis-quarantined four innocent
 * ms-typescript fixtures, one 15-minute retry at a time). Instead every
 * worker APPENDS one short line per event: "S <rel_path>" when it STARTS
 * work on a file, "D <rel_path>" when it finishes it. A single short
 * append of one line is atomic in practice on every target platform, and
 * the parent discards a torn final line by design. The parent's suspect
 * set after a crash/hang = files with an S but no D — exactly the
 * in-flight set; a file is only quarantined after appearing in the
 * suspect set of TWO CONSECUTIVE failed runs, so a stale or merely
 * unlucky in-flight file is never quarantined alone. The env var is set
 * solely by the supervisor during recovery — a no-op on normal runs. */
static void cbm_index_mark(const char *rel_path, char event) {
    const char *mf = getenv("CBM_INDEX_MARKER_FILE");
    if (!mf || !mf[0] || !rel_path || !rel_path[0]) {
        return;
    }
    FILE *f = cbm_fopen(mf, "ab");
    if (f) {
        (void)fprintf(f, "%c %s\n", event, rel_path);
        (void)fclose(f);
    }
}

void cbm_index_mark_start(const char *rel_path) {
    cbm_index_mark(rel_path, 'S');
}

void cbm_index_mark_done(const char *rel_path) {
    cbm_index_mark(rel_path, 'D');
}

/* ── Crash-quarantine set (Stage 3c skip-and-continue) ──────────────────────
 * After a crash the supervisor re-runs the worker single-threaded, passing
 * CBM_INDEX_QUARANTINE_FILE — a newline-delimited list of repo-relative paths
 * that already crashed the indexer and MUST NOT be extracted again. Owned here,
 * next to the other env-driven extract hooks (marker + fault injector), so the
 * single hard guard lives at the one choke point every pass funnels through
 * (cbm_extract_file): whether a pass re-extracts from disk on a cache miss
 * (sequential pass_calls/usages/semantic) or extracts fresh, a quarantined file
 * short-circuits to an empty result and never reaches the parser/crash. The
 * pipeline extract loops separately REPORT the skip as phase="crash" via
 * cbm_index_is_quarantined() so the crasher surfaces in the response skipped[].
 * Loaded once, lazily; read-only after load (safe for the parallel workers,
 * though recovery runs single-threaded). Unset env ⇒ empty set ⇒ cheap no-op. */
static CBMHashTable *g_quarantine_set = NULL;
enum { CBM_QSET_UNINIT = 0, CBM_QSET_INITING = 1, CBM_QSET_INITED = 2 };
static atomic_int g_quarantine_state = CBM_QSET_UNINIT;

static void cbm_quarantine_load(void) {
    const char *qf = getenv("CBM_INDEX_QUARANTINE_FILE");
    if (!qf || !qf[0]) {
        return; /* normal path: empty set */
    }
    FILE *f = cbm_fopen(qf, "rb");
    if (!f) {
        return;
    }
    CBMHashTable *set = cbm_ht_create(16);
    if (!set) {
        (void)fclose(f);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        /* Line format: "path\tphase" where phase is "crash" or "hang". A bare
         * "path" line (no tab) is tolerated and defaults to phase "crash" for
         * backward compatibility with older quarantine files. */
        char *tab = strchr(line, '\t');
        const char *phase = "crash";
        if (tab) {
            *tab = '\0';
            if (tab[1]) {
                phase = tab + 1;
            }
        }
        if (line[0] == '\0') {
            continue; /* empty path (line began with a tab) — skip */
        }
        /* The table borrows the key + value pointers, so dup both. Intentionally
         * never freed: the set lives for the whole (short-lived worker) process.
         * The value stores the phase so cbm_index_quarantine_phase() can report
         * "crash" vs "hang"; membership (cbm_index_is_quarantined) is value != NULL. */
        char *key = cbm_strdup(line);
        char *pval = cbm_strdup(phase);
        if (key && pval) {
            cbm_ht_set(set, key, (void *)pval);
        }
    }
    (void)fclose(f);
    g_quarantine_set = set;
}

bool cbm_index_is_quarantined(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return false;
    }
    int state = atomic_load(&g_quarantine_state);
    if (state != CBM_QSET_INITED) {
        /* First caller wins the CAS and loads; racers spin until INITED.
         * Same once-init pattern as cbm_ui_log_init (http_server.c). */
        state = CBM_QSET_UNINIT;
        if (atomic_compare_exchange_strong(&g_quarantine_state, &state, CBM_QSET_INITING)) {
            cbm_quarantine_load();
            atomic_store(&g_quarantine_state, CBM_QSET_INITED);
        } else {
            while (atomic_load(&g_quarantine_state) != CBM_QSET_INITED) {
                cbm_usleep(1000); /* 1ms */
            }
        }
    }
    return g_quarantine_set && cbm_ht_has(g_quarantine_set, rel_path);
}

const char *cbm_index_quarantine_phase(const char *rel_path) {
    /* cbm_index_is_quarantined drives the lazy once-load and returns true only
     * when the set is loaded and holds rel_path — so on true, g_quarantine_set is
     * non-NULL and the stored value is the phase string ("crash"/"hang"). */
    if (!cbm_index_is_quarantined(rel_path)) {
        return NULL;
    }
    return (const char *)cbm_ht_get(g_quarantine_set, rel_path);
}

static void cbm_test_fault_inject(const char *rel_path) {
    if (!rel_path || !rel_path[0]) {
        return;
    }
    const char *crash_on = getenv("CBM_TEST_CRASH_ON");
    if (crash_on && crash_on[0] && strstr(rel_path, crash_on)) {
        abort(); /* SIGABRT → WIFSIGNALED → classified as a crash */
    }
    const char *hang_on = getenv("CBM_TEST_HANG_ON");
    if (hang_on && hang_on[0] && strstr(rel_path, hang_on)) {
        for (;;) {
            /* Busy-spin: the supervisor's quiet-timeout kills + reports us. */
        }
    }
}

/* Pre-parse nesting guard for pathologically nested input. tree-sitter's GLR
 * parser recurses once per nesting level inside stack_node_add_link
 * (vendored ts_runtime/src/stack.c) while merging ambiguous parse-stack heads.
 * The Perl grammar is genuinely ambiguous for `f(...)` (function call vs.
 * bareword), so a deeply nested call chain `f(f(f(...)))` drives that recursion
 * as deep as the nesting and overflows a small (1 MB Windows) stack *during the
 * parse* — before any of the LSP walk-depth guards can fire. Unambiguous
 * grammars (C/Java/Python) keep a single stack head and don't hit this, which is
 * why only Perl crashed on the Windows/ARM CI runners.
 *
 * This is a workaround: the proper fix is bounding the GLR stack-merge recursion
 * inside the vendored tree-sitter runtime, tracked upstream as #913. Remove this
 * guard once that lands.
 *
 * cbm_source_nesting_exceeds scans the raw bytes for the maximum bracket-nesting
 * depth and returns true as soon as it passes the cap (early-exit, O(n)). Real
 * source never nests brackets this deep, so a file that does is skipped as a
 * parse error (zero edges — graceful degradation, never a crash). Brackets in
 * strings/comments are counted too: the only consequence of a false positive is
 * skipping one absurd file, so string-awareness is not worth the cost. */
#define CBM_PERL_MAX_PARSE_NESTING 128

static bool cbm_source_nesting_exceeds(const char *source, int source_len, int cap) {
    int depth = 0;
    for (int i = 0; i < source_len; i++) {
        char c = source[i];
        if (c == '(' || c == '[' || c == '{') {
            if (++depth > cap) {
                return true;
            }
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            depth--;
        }
    }
    return false;
}

static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines, const char **include_paths);

/* Best-effort parse-coverage collection (#963). Walks only the has_error paths
 * of the tree and records the 1-based line ranges of the TOP-MOST ERROR/MISSING
 * nodes (does not descend into an error subtree — one range per failed region).
 * Bounded by CBM_MAX_ERROR_REGIONS so pathological input can't blow up the
 * output. The ranges mark where constructs were dropped; they are a detection
 * aid, never a completeness proof. */
#define CBM_MAX_ERROR_REGIONS 64
typedef struct {
    uint32_t starts[CBM_MAX_ERROR_REGIONS];
    uint32_t ends[CBM_MAX_ERROR_REGIONS];
    int count;
} cbm_error_regions_t;

static void cbm_error_regions_push(cbm_error_regions_t *acc, TSNode n) {
    if (acc->count >= CBM_MAX_ERROR_REGIONS) {
        return;
    }
    acc->starts[acc->count] = ts_node_start_point(n).row + 1;
    acc->ends[acc->count] = ts_node_end_point(n).row + 1;
    acc->count++;
}

static void cbm_collect_error_regions(TSNode n, cbm_error_regions_t *acc) {
    if (acc->count >= CBM_MAX_ERROR_REGIONS) {
        return;
    }
    uint32_t k = ts_node_child_count(n);
    for (uint32_t i = 0; i < k && acc->count < CBM_MAX_ERROR_REGIONS; i++) {
        TSNode c = ts_node_child(n, i);
        if (ts_node_is_missing(c) || strcmp(ts_node_type(c), "ERROR") == 0) {
            cbm_error_regions_push(acc, c); /* top-most region; do not descend */
        } else if (ts_node_has_error(c)) {
            cbm_collect_error_regions(c, acc);
        }
    }
}

/* Recovery subtraction (#963): tree-sitter error recovery plus the
 * ERROR-descending def walker often still extract constructs INSIDE a failed
 * region (verified: a function in an #ifdef-split ERROR region and even a
 * `def broken(:` both came back as defs). A region whose every line is
 * covered by definitions that START inside it is definitely recovered — its
 * constructs ARE in the graph — so flagging it would be a false miss.
 * Container defs (Module/Package) are ignored: a file-spanning Module node is
 * not evidence the region's constructs survived. Conservative: partially
 * covered regions stay flagged. */
static bool cbm_region_is_recovered(uint32_t rs, uint32_t re, const CBMDefArray *defs) {
    if (!defs || defs->count <= 0) {
        return false;
    }
    uint32_t covered_to = rs - 1;
    while (covered_to < re) {
        uint32_t next_line = covered_to + 1;
        uint32_t best_end = covered_to;
        for (int i = 0; i < defs->count; i++) {
            const CBMDefinition *d = &defs->items[i];
            if (!d->label || strcmp(d->label, "Module") == 0 || strcmp(d->label, "Package") == 0) {
                continue;
            }
            if (d->start_line < rs || d->start_line > re) {
                continue; /* recovery evidence must originate inside the region */
            }
            uint32_t end = d->end_line < d->start_line ? d->start_line : d->end_line;
            if (d->start_line <= next_line && end > best_end) {
                best_end = end;
            }
        }
        if (best_end < next_line) {
            return false;
        }
        covered_to = best_end;
    }
    return true;
}

static bool cbm_lang_uses_c_preprocessor(CBMLanguage language) {
    return language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA;
}

static bool cbm_line_blank_or_preproc(const char *source, int start, int end, bool *is_preproc) {
    int p = start;
    while (p < end && isspace((unsigned char)source[p])) {
        p++;
    }
    bool preproc = p < end && source[p] == '#';
    if (is_preproc) {
        *is_preproc = preproc;
    }
    return p >= end || preproc;
}

static int cbm_line_start_for_byte(const char *source, int byte_pos) {
    while (byte_pos > 0 && source[byte_pos - 1] != '\n') {
        byte_pos--;
    }
    return byte_pos;
}

static bool cbm_recovered_callable_in_region(const CBMRecoveredCallable *rec, uint32_t rs,
                                             uint32_t re) {
    return rec && rec->name && rec->label && rec->start_line >= rs && rec->start_line <= re;
}

static bool cbm_line_in_recovered_callables(uint32_t line,
                                            const CBMRecoveredCallableArray *recovered, uint32_t rs,
                                            uint32_t re) {
    if (!recovered) {
        return false;
    }
    for (int i = 0; i < recovered->count; i++) {
        const CBMRecoveredCallable *rec = &recovered->items[i];
        if (!cbm_recovered_callable_in_region(rec, rs, re)) {
            continue;
        }
        uint32_t end = rec->end_line < rec->start_line ? rec->start_line : rec->end_line;
        if (line >= rec->start_line && line <= end) {
            return true;
        }
    }
    return false;
}

static bool cbm_recovered_alt_signature_covers_line(const char *source, int source_len,
                                                    uint32_t line, uint32_t rs, uint32_t re,
                                                    const CBMRecoveredCallableArray *recovered) {
    if (!source || source_len <= 0 || !recovered) {
        return false;
    }
    int region_start = 0;
    int region_end = 0;
    if (!cbm_source_line_bounds(source, source_len, rs, re, &region_start, &region_end)) {
        return false;
    }
    for (int i = 0; i < recovered->count; i++) {
        const CBMRecoveredCallable *rec = &recovered->items[i];
        if (!cbm_recovered_callable_in_region(rec, rs, re) || !cbm_plain_identifier(rec->name)) {
            continue;
        }
        const char *scope = NULL;
        if (strcmp(rec->label, "Method") == 0) {
            scope = cbm_last_qn_segment(rec->parent_class);
        }
        int name_len = (int)strlen(rec->name);
        for (int pos = region_start; pos + name_len <= region_end; pos++) {
            if (!cbm_ident_at(source, source_len, pos, rec->name, name_len) ||
                !cbm_scope_before_name_matches(source, pos, scope)) {
                continue;
            }
            int sig_line_start = cbm_line_start_for_byte(source, pos);
            if (!cbm_name_context_allows_definition(source, sig_line_start, pos)) {
                continue;
            }
            int p = pos + name_len;
            while (p < region_end && isspace((unsigned char)source[p])) {
                p++;
            }
            if (p >= region_end || source[p] != '(') {
                continue;
            }
            int close_paren = cbm_find_matching_paren(source, region_end, p);
            if (close_paren < 0) {
                continue;
            }
            int open_brace = cbm_find_definition_open_brace(source, region_end, close_paren);
            if (open_brace < 0) {
                continue;
            }
            uint32_t sig_start_line = cbm_line_for_byte(source, pos);
            uint32_t sig_open_line = cbm_line_for_byte(source, open_brace);
            if (line >= sig_start_line && line <= sig_open_line) {
                return true;
            }
        }
    }
    return false;
}

/* C-family preprocessor recovery: only successfully remapped expanded-AST
 * callables can justify clearing raw ERROR lines. Raw definitions still
 * participate in the generic recovery subtraction above, but this preprocessor
 * branch must not treat arbitrary raw defs as evidence. */
static bool cbm_region_is_recovered_with_cpp_preproc(uint32_t rs, uint32_t re,
                                                     const CBMRecoveredCallableArray *recovered,
                                                     const char *source, int source_len,
                                                     CBMLanguage language) {
    if (!cbm_lang_uses_c_preprocessor(language) || !source || source_len <= 0 || !recovered ||
        recovered->count <= 0) {
        return false;
    }

    bool saw_preproc = false;
    bool saw_recovered = false;
    int line_start = 0;
    uint32_t line = 1;
    for (int p = 0; p <= source_len; p++) {
        if (p < source_len && source[p] != '\n') {
            continue;
        }
        if (line >= rs && line <= re) {
            int line_end = p;
            bool is_preproc = false;
            if (cbm_line_blank_or_preproc(source, line_start, line_end, &is_preproc)) {
                saw_preproc = saw_preproc || is_preproc;
            } else if (cbm_line_in_recovered_callables(line, recovered, rs, re)) {
                saw_recovered = true;
            } else if (!cbm_recovered_alt_signature_covers_line(source, source_len, line, rs, re,
                                                                recovered)) {
                return false;
            }
        }
        line++;
        line_start = p + 1;
    }
    return saw_preproc && saw_recovered;
}

static void cbm_subtract_recovered_regions(cbm_error_regions_t *regs, const CBMDefArray *defs,
                                           const CBMRecoveredCallableArray *recovered,
                                           const char *source, int source_len,
                                           CBMLanguage language) {
    int kept = 0;
    for (int i = 0; i < regs->count; i++) {
        if (!cbm_region_is_recovered(regs->starts[i], regs->ends[i], defs) &&
            !cbm_region_is_recovered_with_cpp_preproc(regs->starts[i], regs->ends[i], recovered,
                                                      source, source_len, language)) {
            regs->starts[kept] = regs->starts[i];
            regs->ends[kept] = regs->ends[i];
            kept++;
        }
    }
    regs->count = kept;
}

/* Serialize collected regions as "start-end,start-end,..." into the arena. */
static const char *cbm_error_ranges_str(CBMArena *a, const cbm_error_regions_t *regs) {
    if (regs->count <= 0) {
        return NULL;
    }
    enum { RANGE_MAX = 24 }; /* "4294967295-4294967295," */
    char *buf = (char *)cbm_arena_alloc(a, (size_t)regs->count * RANGE_MAX);
    if (!buf) {
        return NULL;
    }
    size_t off = 0;
    for (int i = 0; i < regs->count; i++) {
        off += (size_t)snprintf(buf + off, RANGE_MAX, "%s%u-%u", i ? "," : "", regs->starts[i],
                                regs->ends[i]);
    }
    return buf;
}

/* Public entry: run the extraction and journal completion. The DONE mark on
 * every ordinary return (including error/timeout results) tells the crash
 * supervisor this file did NOT kill the worker — only a file whose S has no
 * D is a crash/hang suspect. */
CBMFileResult *cbm_extract_file(const char *source, int source_len, CBMLanguage language,
                                const char *project, const char *rel_path, int64_t timeout_micros,
                                const char **extra_defines, const char **include_paths) {
    CBMFileResult *r = cbm_extract_file_impl(source, source_len, language, project, rel_path,
                                             timeout_micros, extra_defines, include_paths);
    cbm_index_mark_done(rel_path);
    return r;
}

static CBMFileResult *cbm_extract_file_impl(const char *source, int source_len,
                                            CBMLanguage language, const char *project,
                                            const char *rel_path, int64_t timeout_micros,
                                            const char **extra_defines,
                                            const char **include_paths) {
    // Allocate result on heap (arena inside for all string data)
    enum { SINGLE = 1 };
    CBMFileResult *result = (CBMFileResult *)calloc(SINGLE, sizeof(CBMFileResult));
    if (!result) {
        return NULL;
    }

    cbm_arena_init(&result->arena);
    CBMArena *a = &result->arena;

    /* Crash-quarantine hard guard (Stage 3c): a file the supervisor pinned as a
     * crasher must NEVER be parsed again. Return a clean empty result BEFORE the
     * marker write and fault injector so no pass (including sequential re-extract
     * passes that miss the result cache) can crash on it. The pipeline extract
     * loops separately record it as a phase="crash" skip. Checked before the
     * marker so quarantined files never overwrite it — the marker keeps pointing
     * at the real (non-quarantined) file being processed when a crash hits. */
    if (cbm_index_is_quarantined(rel_path)) {
        return result;
    }

    cbm_index_mark_start(rel_path);
    cbm_test_fault_inject(rel_path);

    // Get language spec
    const CBMLangSpec *spec = cbm_lang_spec(language);
    if (!spec) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "unsupported language");
        return result;
    }

    // Get tree-sitter language
    const TSLanguage *ts_lang = cbm_ts_language(language);
    if (!ts_lang) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "no tree-sitter grammar");
        return result;
    }

    // Skip pathologically nested Perl before tree-sitter's recursive GLR stack
    // merge overflows a small stack during the parse (see
    // cbm_source_nesting_exceeds). Scoped to Perl: its ambiguous call grammar is
    // the only one that drives that recursion to the nesting depth.
    if (language == CBM_LANG_PERL &&
        cbm_source_nesting_exceeds(source, source_len, CBM_PERL_MAX_PARSE_NESTING)) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "perl source nesting too deep; skipped");
        return result;
    }

    // Get thread-local parser (reused across files on same thread)
    TSParser *parser = get_thread_parser(ts_lang, language);
    if (!parser) {
        result->has_error = true;
        result->error_msg = cbm_arena_strdup(a, "parser alloc failed");
        return result;
    }

    // Reset parser state from any previous parse (cancellation flags etc.)
    ts_parser_reset(parser);

    uint64_t t0 = now_ns();

    // Build string input + timeout options for parse_with_options
    CBMStringInput str_input = {source, (uint32_t)source_len};
    TSInput ts_input = {
        &str_input,
        cbm_string_read,
        TSInputEncodingUTF8,
        NULL,
    };

    TSParseOptions opts = {0};
    uint64_t deadline_ns = 0; // cppcheck-suppress unreadVariable
    if (timeout_micros > 0) {
        deadline_ns = t0 + ((uint64_t)timeout_micros * USEC_TO_NSEC);
        opts.payload = &deadline_ns;
        opts.progress_callback = cbm_timeout_cb;
    }

    TSTree *tree = ts_parser_parse_with_options(parser, NULL, ts_input, opts);
    uint64_t t1 = now_ns();

    if (!tree) {
        result->has_error = true;
        result->error_msg =
            cbm_arena_strdup(a, timeout_micros > 0 ? "parse timeout" : "parse failed");
        return result;
    }

    TSNode root = ts_tree_root_node(tree);

    // Compute module QN. Java/Go derive the module from the CONTAINING
    // DIRECTORY (package semantics) rather than baking the filename stem in,
    // so def QNs, the LSP caller_qn, and the textual calls-enclosing QN all
    // agree (e.g. Outer.java -> module "proj", not "proj.Outer"). Other
    // languages are unchanged.
    result->module_qn = cbm_fqn_module_source_lang(a, project, rel_path, language);
    result->is_test_file = cbm_is_test_file(rel_path, language);

    // Build extraction context
    CBMExtractCtx ctx = {
        .arena = a,
        .result = result,
        .source = source,
        .source_len = source_len,
        .language = language,
        .project = project,
        .rel_path = rel_path,
        .module_qn = result->module_qn,
        .root = root,
    };

    // Run extractors: defs + imports use separate walks (unique recursion patterns),
    // then a single unified cursor walk handles the remaining 7 extractors.
    cbm_extract_definitions(&ctx);
    cbm_extract_imports(&ctx);
    cbm_extract_unified(&ctx);

    // Channel detection (Socket.IO / EventEmitter) — JS/TS only.
    cbm_extract_channels(&ctx);

    // K8s / Kustomize semantic pass (additional structured extraction for YAML-based infra files).
    if (ctx.language == CBM_LANG_KUSTOMIZE || ctx.language == CBM_LANG_K8S) {
        cbm_extract_k8s(&ctx);
    }

    // LSP type-aware call/usage resolution (per-file). Runs in every mode;
    // refines the tree-sitter + textual-resolution graph with type info.
    uint64_t lsp_start = now_ns();
    {
        if (language == CBM_LANG_GO) {
            cbm_run_go_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
            cbm_run_c_lsp(a, result, source, source_len, root, language != CBM_LANG_C);
        }
        if (language == CBM_LANG_PHP) {
            cbm_run_php_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_PERL) {
            cbm_run_perl_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_PYTHON) {
            cbm_run_py_lsp(a, result, source, source_len, root);
        }
        if (language == CBM_LANG_JAVASCRIPT || language == CBM_LANG_TYPESCRIPT ||
            language == CBM_LANG_TSX) {
            bool js_mode = (language == CBM_LANG_JAVASCRIPT);
            // jsx_mode: TSX always; .jsx in the JS bucket also enables it.
            bool jsx_mode = (language == CBM_LANG_TSX);
            if (language == CBM_LANG_JAVASCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 4 && strcmp(rel_path + rl - 4, ".jsx") == 0)
                    jsx_mode = true;
            }
            // dts_mode: ".d.ts" suffix (TypeScript only).
            bool dts_mode = false;
            if (language == CBM_LANG_TYPESCRIPT && rel_path) {
                size_t rl = strlen(rel_path);
                if (rl >= 5 && strcmp(rel_path + rl - 5, ".d.ts") == 0)
                    dts_mode = true;
            }
            cbm_run_ts_lsp(a, result, source, source_len, root, js_mode, jsx_mode, dts_mode);
        }
        if (language == CBM_LANG_CSHARP) {
            cbm_run_cs_lsp(a, result, source, source_len, root);
        }
    }
    if (language == CBM_LANG_JAVA) {
        cbm_run_java_lsp(a, result, source, source_len, root);
    }
    if (language == CBM_LANG_KOTLIN) {
        cbm_run_kotlin_lsp(a, result, source, source_len, root);
    }
    if (language == CBM_LANG_RUST) {
        cbm_run_rust_lsp(a, result, source, source_len, root);
    }
    atomic_fetch_add(&total_lsp_ns, now_ns() - lsp_start);

    // Calls extracted so far all carry ORIGINAL-source line numbers; the C/C++
    // preprocessor second pass below appends calls with EXPANDED-source lines,
    // which must not be used for the def line-range attribution of the bottleneck
    // metrics. Remember the boundary.
    int orig_calls_count = result->calls.count;
    CBMRecoveredCallableArray recovered_callables = {0};

    // Second pass: preprocess C/C++/CUDA and extract additional macro-hidden
    // callables/calls. Existing defs keep original-source line numbers; only
    // callables missing from the raw tree-sitter pass are filled from expanded
    // source so #ifdef/#else signature blocks cannot swallow later methods.
    if (language == CBM_LANG_C || language == CBM_LANG_CPP || language == CBM_LANG_CUDA) {
        uint64_t pp_start = now_ns();
        CBMPreprocessedSource *preprocessed = cbm_preprocess_with_map(
            source, source_len, rel_path, extra_defines, include_paths, language != CBM_LANG_C);
        if (preprocessed && preprocessed->source) {
            char *expanded = preprocessed->source;
            int expanded_len = (int)strlen(expanded);
            // Record calls count before second pass
            int calls_before = result->calls.count;

            // Parse expanded source with fresh tree
            TSParser *pp_parser = get_thread_parser(ts_lang, language);
            if (pp_parser) {
                ts_parser_reset(pp_parser);
                CBMStringInput pp_input = {expanded, (uint32_t)expanded_len};
                TSInput pp_ts_input = {
                    &pp_input,
                    cbm_string_read,
                    TSInputEncodingUTF8,
                    NULL,
                };
                TSParseOptions pp_opts = {0};
                TSTree *pp_tree =
                    ts_parser_parse_with_options(pp_parser, NULL, pp_ts_input, pp_opts);
                if (pp_tree) {
                    TSNode pp_root = ts_tree_root_node(pp_tree);

                    // Build context for expanded source — extract only calls via unified extractor
                    CBMExtractCtx pp_ctx = {
                        .arena = a,
                        .result = result,
                        .source = expanded,
                        .source_len = expanded_len,
                        .language = language,
                        .project = project,
                        .rel_path = rel_path,
                        .module_qn = result->module_qn,
                        .root = pp_root,
                    };
                    CBMFileResult pp_defs_result = {0};
                    pp_defs_result.module_qn = result->module_qn;
                    pp_defs_result.is_test_file = result->is_test_file;
                    CBMExtractCtx pp_defs_ctx = pp_ctx;
                    pp_defs_ctx.result = &pp_defs_result;
                    cbm_extract_definition_nodes(&pp_defs_ctx);
                    merge_missing_preprocessed_callables(result, &pp_defs_result, a, preprocessed,
                                                         source, source_len, &recovered_callables);

                    // Re-run unified extraction on expanded source.
                    // This adds macro-expanded calls; duplicates with original calls are
                    // harmless (pipeline deduplicates by caller+callee).
                    cbm_extract_unified(&pp_ctx);

                    // Also run LSP on expanded source for additional type-resolved
                    // calls (language is already C/C++/CUDA — checked in enclosing
                    // block). Runs in every mode.
                    cbm_run_c_lsp(a, result, expanded, expanded_len, pp_root,
                                  language != CBM_LANG_C);

                    ts_tree_delete(pp_tree);
                }
            }
            cbm_preprocessed_source_free(preprocessed);
            atomic_fetch_add(&total_files_preprocessed, 1);
            (void)calls_before; // used for future logging
        }
        atomic_fetch_add(&total_preprocess_ns, now_ns() - pp_start);
    }

    // Bottleneck call-context metrics. Each call is attributed to the INNERMOST
    // enclosing Function/Method def by source-line range (defs and calls in one
    // CBMFileResult share the same file). Range matching is used instead of
    // enclosing_func_qn string matching because some grammars (notably C, whose
    // function_definition has no "name" field) attribute the call's scope to the
    // module rather than the function — line ranges are unambiguous and
    // language-agnostic. Bounded per file (defs x calls), not a repo-scale scan.
    int def_count = result->defs.count;
    bool *has_self = def_count > 0 ? calloc((size_t)def_count, sizeof(bool)) : NULL;
    bool *has_guarded = def_count > 0 ? calloc((size_t)def_count, sizeof(bool)) : NULL;

    // param_count is a standalone structural smell (independent of calls). Prefer
    // the parsed param_names array; fall back to counting from the signature text
    // for languages (e.g. C) that populate only the signature.
    for (int di = 0; di < def_count; di++) {
        CBMDefinition *d = &result->defs.items[di];
        int pc = 0;
        if (d->param_names) {
            while (d->param_names[pc]) {
                pc++;
            }
        }
        if (pc == 0 && d->signature) {
            pc = count_params_from_signature(d->signature);
        }
        d->param_count = pc;
    }

    for (int ci = 0; ci < orig_calls_count; ci++) {
        const CBMCall *c = &result->calls.items[ci];
        if (!c->callee_name || c->start_line <= 0) {
            continue;
        }
        // Innermost enclosing Function/Method def by line range (smallest span).
        int best = -1;
        int best_span = -1;
        for (int di = 0; di < def_count; di++) {
            const CBMDefinition *d = &result->defs.items[di];
            if (!d->name || !d->label ||
                (strcmp(d->label, "Function") != 0 && strcmp(d->label, "Method") != 0)) {
                continue;
            }
            if ((int)d->start_line <= c->start_line && c->start_line <= (int)d->end_line) {
                int span = (int)d->end_line - (int)d->start_line;
                if (best < 0 || span < best_span) {
                    best_span = span;
                    best = di;
                }
            }
        }
        if (best < 0) {
            continue;
        }
        CBMDefinition *d = &result->defs.items[best];
        // callee_name may be bare ("recur") or qualified ("self.recur",
        // "super().save", "axios.get"). A short-name match alone is not
        // self-recursion: the callee must also target the same object
        // (is_self_receiver), or super().save() inside save and axios.get
        // inside get are false positives (#599).
        const char *dot = strrchr(c->callee_name, '.');
        const char *callee_short = dot ? dot + 1 : c->callee_name;
        bool in_loop = c->loop_depth > 0;

        if (strcmp(callee_short, d->name) == 0 && is_self_receiver(c->callee_name, d->receiver)) {
            // Direct self-recursion. The call graph omits self-edges (pass_calls
            // skips source==target), so detect it here; seeds "recursive".
            d->is_recursive = true;
            if (has_self) {
                has_self[best] = true;
            }
            if (in_loop) {
                d->recursion_in_loop = true; // recursion compounded by a loop
            }
            if (c->branch_depth > 0 && has_guarded) {
                has_guarded[best] = true; // a self-call guarded by some conditional
            }
        }
        if (in_loop && is_linear_scan_name(callee_short)) {
            d->linear_scan_in_loop++; // hidden O(n^2): linear scan inside a loop
        }
        if (in_loop && is_alloc_name(callee_short)) {
            d->alloc_in_loop++; // repeated allocation/append inside a loop
        }
    }

    // Recursive with no self-call guarded by any conditional → no obvious base
    // case on the recursive path: a stronger "potentially unbounded" signal.
    for (int di = 0; di < def_count; di++) {
        if (has_self && has_self[di] && !(has_guarded && has_guarded[di])) {
            result->defs.items[di].unguarded_recursion = true;
        }
    }
    free(has_self);
    free(has_guarded);

    uint64_t t2 = now_ns();

    /* Best-effort parse-coverage signal (#963): flag files whose tree contains
     * ERROR/MISSING regions. Computed AFTER extraction so definite recovery is
     * subtracted first — a region fully re-extracted as definitions is not a
     * miss, and a fully recovered file is not flagged at all. Detection aid
     * only: the absence of this flag is NOT a completeness guarantee. */
    if (ts_node_has_error(root)) {
        cbm_error_regions_t regs = {{0}, {0}, 0};
        if (strcmp(ts_node_type(root), "ERROR") == 0) {
            cbm_error_regions_push(&regs, root); /* whole file unparseable */
        } else {
            cbm_collect_error_regions(root, &regs);
        }
        cbm_subtract_recovered_regions(&regs, &result->defs, &recovered_callables, source,
                                       source_len, language);
        if (regs.count > 0) {
            result->parse_incomplete = true;
            result->error_region_count = regs.count;
            result->error_ranges = cbm_error_ranges_str(a, &regs);
        }
    }

    result->imports_count = result->imports.count;

    // Accumulate profiling counters
    atomic_fetch_add(&total_parse_ns, t1 - t0);
    atomic_fetch_add(&total_extract_ns, t2 - t1);
    atomic_fetch_add(&total_files, 1);

    // Retain tree for cross-file LSP reuse (caller frees via cbm_free_tree)
    result->cached_tree = tree;
    result->cached_lang = language;
    return result;
}

void cbm_free_result(CBMFileResult *result) {
    if (!result) {
        return;
    }
    if (result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
    cbm_arena_destroy(&result->arena);
    free(result);
}

void cbm_free_tree(CBMFileResult *result) {
    if (result && result->cached_tree) {
        ts_tree_delete(result->cached_tree);
        result->cached_tree = NULL;
    }
}

void cbm_free_tree_ptr(TSTree *tree) {
    if (tree) {
        ts_tree_delete(tree);
    }
}
