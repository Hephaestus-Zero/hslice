/*
 * hslice.c - Sub-2ms AST/Structural Code Slicer & Single-Shot Recon Engine
 *
 * Collapses 4-8 sequential agent Search/Read turns into ONE sub-2ms call:
 *   1. `hslice <symbol> [symbol2...]`        -> Exact function/class/struct body via mmap index
 *   2. `hslice <file>:<line>`                -> Enclosing function/class around traceback line
 *   3. `hslice <file>:<start>-<end>`         -> Exact numbered line slice
 *   4. `hslice -s <file>` (`--skeleton`)     -> Structural file outline (~90% fewer tokens)
 *   5. `hslice -x <symbol> [dir]` (`--xref`) -> Definition body + bounded callers/refs in 1 shot
 *   6. `hslice -A, --audit [dir]`            -> Shareable repo token-savings receipt
 *
 * Zero-daemon lazy freshness: matched files with `stat().st_mtime > idx->built_epoch`
 * trigger an automatic one-shot index rebuild in single-digit ms.
 *
 * Build: cc -std=c11 -O3 -Wall -Wextra -Werror -o hslice src/hslice.c
 */
#include "hslice_common.h"
#include <dirent.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#define HSYM_MAGIC   0x4853594DU
#define IDX_VERSION  1U
#define ALPHA_BITS   6U
#define NUM_TRIGRAMS (1U << (ALPHA_BITS * 3U))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_recs;
    uint32_t str_bytes;
    uint32_t postings_len;
    uint32_t pad;
    uint64_t built_epoch;
} IndexHeader;

typedef struct {
    uint32_t offset;
    uint32_t len;
} TriBucket;

typedef struct {
    uint32_t path_off;
    uint32_t sym_off;
    uint32_t sig_off;
    uint32_t line;
    uint16_t base_off;
    uint8_t  sym_len;
    uint8_t  kind;
} SymRec;

static const char *KIND_NAMES[] = {
    "fn", "class", "struct", "type", "iface", "enum", "trait", "const", "macro"
};

typedef enum {
    LANG_GENERIC = 0,
    LANG_PY,
    LANG_BRACE
} SliceLang;

typedef struct {
    char **lines;
    int count;
    char *raw;
    size_t raw_len;
} FileLines;

static inline uint32_t sym6(unsigned char c) {
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    if (c >= 'a' && c <= 'z') return (uint32_t)(c - 'a' + 1);
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0' + 27);
    switch (c) {
        case '_': return 37;
        case '-': return 38;
        case '.': return 39;
        case '/': return 40;
        case '+': return 41;
        case '@': return 42;
        default:  return 63;
    }
}

static inline uint32_t pack_tri(unsigned char a, unsigned char b, unsigned char c) {
    return (sym6(a) << 12) | (sym6(b) << 6) | sym6(c);
}

static SliceLang detect_lang(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return LANG_GENERIC;
    if (strcmp(dot, ".py") == 0 || strcmp(dot, ".pyi") == 0) return LANG_PY;
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
        strcmp(dot, ".cpp") == 0 || strcmp(dot, ".hpp") == 0 ||
        strcmp(dot, ".cc") == 0 || strcmp(dot, ".cxx") == 0 ||
        strcmp(dot, ".hxx") == 0 || strcmp(dot, ".rs") == 0 ||
        strcmp(dot, ".go") == 0 || strcmp(dot, ".ts") == 0 ||
        strcmp(dot, ".tsx") == 0 || strcmp(dot, ".js") == 0 ||
        strcmp(dot, ".jsx") == 0 || strcmp(dot, ".mjs") == 0 ||
        strcmp(dot, ".cjs") == 0 || strcmp(dot, ".java") == 0) {
        return LANG_BRACE;
    }
    return LANG_GENERIC;
}

static bool load_file_lines(const char *path, FileLines *fl) {
    memset(fl, 0, sizeof(*fl));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) {
        close(fd);
        return false;
    }
    fl->raw_len = (size_t)st.st_size;
    fl->raw = malloc(fl->raw_len + 2);
    if (!fl->raw) {
        close(fd);
        return false;
    }
    ssize_t rd = read(fd, fl->raw, fl->raw_len);
    close(fd);
    if (rd <= 0) {
        free(fl->raw);
        fl->raw = NULL;
        return false;
    }
    fl->raw[rd] = '\0';

    int cap = 256;
    fl->lines = malloc((size_t)cap * sizeof(char *));
    char *p = fl->raw;
    char *end = fl->raw + rd;
    while (p < end) {
        if (fl->count >= cap) {
            cap *= 2;
            fl->lines = realloc(fl->lines, (size_t)cap * sizeof(char *));
        }
        fl->lines[fl->count++] = p;
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        if (nl > p && nl[-1] == '\r') nl[-1] = '\0';
        *nl = '\0';
        p = nl + 1;
    }
    return true;
}

static void free_file_lines(FileLines *fl) {
    free(fl->lines);
    free(fl->raw);
    memset(fl, 0, sizeof(*fl));
}

static int line_indent(const char *s, bool *is_blank_or_comment, char comment_ch) {
    int col = 0;
    while (*s == ' ' || *s == '\t') {
        col += (*s == '\t') ? 4 : 1;
        s++;
    }
    if (*s == '\0' || (comment_ch && *s == comment_ch)) {
        *is_blank_or_comment = true;
    } else {
        *is_blank_or_comment = false;
    }
    return col;
}

/* Find exact [start_line, end_line] (1-based) for symbol starting at def_line (1-based) */
static void compute_symbol_bounds(const FileLines *fl, SliceLang lang, int def_line,
                                  int *out_start, int *out_end) {
    if (def_line < 1) def_line = 1;
    if (def_line > fl->count) def_line = fl->count;

    int start = def_line;
    int end = def_line;

    if (lang == LANG_PY) {
        /* Walk upwards for @decorators or adjacent # comments */
        while (start > 1) {
            const char *prev = fl->lines[start - 2];
            while (*prev == ' ' || *prev == '\t') prev++;
            if (*prev == '@' || (*prev == '#' && (def_line - start) < 4)) {
                start--;
            } else {
                break;
            }
        }

        bool dummy = false;
        int base_indent = line_indent(fl->lines[def_line - 1], &dummy, '#');
        int paren_depth = 0;
        bool sig_done = false;
        int triple_q = 0; /* 0=none, 1=''', 2=""" */
        int last_nonblank = def_line;

        for (int ln = def_line; ln <= fl->count; ln++) {
            const char *s = fl->lines[ln - 1];
            bool blank = false;
            int ind = line_indent(s, &blank, (triple_q == 0) ? '#' : '\0');

            if (ln > def_line && sig_done && triple_q == 0 && paren_depth <= 0 && !blank) {
                /* Decorator at <= base_indent starts the NEXT sibling def/class */
                if (ind <= base_indent) {
                    break;
                }
            }

            /* Scan characters for parens & triple quotes */
            for (size_t i = 0; s[i]; i++) {
                if (triple_q != 0) {
                    char qch = (triple_q == 1) ? '\'' : '"';
                    if (s[i] == '\\' && s[i + 1]) { i++; continue; }
                    if (s[i] == qch && s[i + 1] == qch && s[i + 2] == qch) {
                        triple_q = 0;
                        i += 2;
                    }
                    continue;
                }
                if (s[i] == '#') break;
                if ((s[i] == '\'' || s[i] == '"') && s[i + 1] == s[i] && s[i + 2] == s[i]) {
                    triple_q = (s[i] == '\'') ? 1 : 2;
                    i += 2;
                    continue;
                }
                if (s[i] == '\'' || s[i] == '"') {
                    char q = s[i++];
                    while (s[i] && s[i] != q) {
                        if (s[i] == '\\' && s[i + 1]) i++;
                        i++;
                    }
                    if (!s[i]) break;
                    continue;
                }
                if (s[i] == '(' || s[i] == '[' || s[i] == '{') paren_depth++;
                else if (s[i] == ')' || s[i] == ']' || s[i] == '}') {
                    if (paren_depth > 0) paren_depth--;
                } else if (s[i] == ':' && paren_depth == 0) {
                    sig_done = true;
                }
            }
            if (!blank || triple_q != 0) last_nonblank = ln;
            end = last_nonblank;
        }
    } else if (lang == LANG_BRACE) {
        /* Capture up to 4 preceding doc/attribute/template lines */
        while (start > 1 && (def_line - start) < 5) {
            const char *prev = fl->lines[start - 2];
            while (*prev == ' ' || *prev == '\t') prev++;
            if (strncmp(prev, "//", 2) == 0 || strncmp(prev, "/*", 2) == 0 ||
                *prev == '*' || strncmp(prev, "#[", 2) == 0 ||
                strncmp(prev, "template", 8) == 0 || strncmp(prev, "export ", 7) == 0) {
                start--;
            } else {
                break;
            }
        }

        int brace_depth = 0;
        bool opened = false;
        bool in_block_comment = false;

        for (int ln = def_line; ln <= fl->count; ln++) {
            const char *s = fl->lines[ln - 1];
            end = ln;
            for (size_t i = 0; s[i]; i++) {
                if (in_block_comment) {
                    if (s[i] == '*' && s[i + 1] == '/') {
                        in_block_comment = false;
                        i++;
                    }
                    continue;
                }
                if (s[i] == '/' && s[i + 1] == '/') break;
                if (s[i] == '/' && s[i + 1] == '*') {
                    in_block_comment = true;
                    i++;
                    continue;
                }
                if (s[i] == '"' || s[i] == '\'' || s[i] == '`') {
                    char q = s[i++];
                    while (s[i] && s[i] != q) {
                        if (s[i] == '\\' && s[i + 1]) i++;
                        i++;
                    }
                    if (!s[i]) break;
                    continue;
                }
                if (s[i] == '{') {
                    brace_depth++;
                    opened = true;
                } else if (s[i] == '}') {
                    brace_depth--;
                    if (opened && brace_depth <= 0) {
                        *out_start = start;
                        *out_end = ln;
                        return;
                    }
                } else if (!opened && s[i] == ';' && (ln - def_line) <= 6) {
                    /* Forward declaration / typedef / single-line const */
                    *out_start = start;
                    *out_end = ln;
                    return;
                }
            }
            if (!opened && (ln - def_line) > 12) {
                break;
            }
        }
    } else {
        start = (def_line > 5) ? (def_line - 5) : 1;
        end = (def_line + 35 <= fl->count) ? (def_line + 35) : fl->count;
    }

    *out_start = start;
    *out_end = end;
}

static void print_slice(const FileLines *fl, const char *path, int start, int end,
                        const char *tag, int max_lines) {
    if (start < 1) start = 1;
    if (end > fl->count) end = fl->count;
    if (end < start) end = start;

    printf("=== %s:%d-%d", path, start, end);
    if (tag && tag[0]) printf(" [%s]", tag);
    printf(" ===\n");

    int total = end - start + 1;
    if (max_lines > 0 && total > max_lines) {
        int head_n = (max_lines * 3) / 4;
        if (head_n < 1) head_n = 1;
        int tail_n = max_lines - head_n;
        for (int ln = start; ln < start + head_n; ln++) {
            printf("%4d: %s\n", ln, fl->lines[ln - 1]);
        }
        printf("  ... [%d middle lines elided; pass -m %d for full body] ...\n",
               total - max_lines, total + 5);
        for (int ln = end - tail_n + 1; ln <= end; ln++) {
            printf("%4d: %s\n", ln, fl->lines[ln - 1]);
        }
    } else {
        for (int ln = start; ln <= end; ln++) {
            printf("%4d: %s\n", ln, fl->lines[ln - 1]);
        }
    }
}

typedef struct {
    void *map;
    size_t map_size;
    uint64_t built_epoch;
    const IndexHeader *hdr;
    const TriBucket *buckets;
    const SymRec *syms;
    const uint32_t *postings;
    const char *strs;
} MappedSymIdx;

static bool open_sym_index(MappedSymIdx *idx, const char *argv0,
                           char roots[][PATH_MAX], int n_roots,
                           bool force_rebuild, bool quiet_rebuild,
                           bool *did_build_out) {
    memset(idx, 0, sizeof(*idx));
    if (did_build_out) *did_build_out = false;

    char idx_path[PATH_MAX + 128];
    char joined_roots[PATH_MAX * 2];
    if (!hslice_index_path(idx_path, sizeof(idx_path), "hsym",
                           roots, n_roots, joined_roots, sizeof(joined_roots))) {
        return false;
    }

    struct stat st;
    bool need_build = force_rebuild || (stat(idx_path, &st) != 0) ||
                      ((time(NULL) - st.st_mtime) > 3600);
    if (need_build) {
        if (did_build_out) *did_build_out = true;
        pid_t pid = fork();
        if (pid == 0) {
            if (quiet_rebuild) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDOUT_FILENO);
                    close(devnull);
                }
            }
            if (argv0 && strchr(argv0, '/') != NULL) {
                char sibling[PATH_MAX];
                const char *slash = strrchr(argv0, '/');
                size_t dlen = (size_t)(slash - argv0);
                if (dlen > 0 && dlen + 16 < sizeof(sibling)) {
                    memcpy(sibling, argv0, dlen);
                    snprintf(sibling + dlen, sizeof(sibling) - dlen, "/hsearch");
                    if (access(sibling, X_OK) == 0) {
                        execl(sibling, "hsearch", "--rebuild", "--root", joined_roots, (char *)NULL);
                    }
                }
            }
            execlp("hsearch", "hsearch", "--rebuild", "--root", joined_roots, (char *)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            (void)waitpid(pid, &status, 0);
        }
        if (stat(idx_path, &st) != 0 || st.st_size < (off_t)sizeof(IndexHeader)) return false;
    }

    int fd = open(idx_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return false;

    const IndexHeader *hdr = (const IndexHeader *)map;
    if (hdr->magic != HSYM_MAGIC || hdr->version != IDX_VERSION) {
        munmap(map, (size_t)st.st_size);
        return false;
    }

    idx->map = map;
    idx->map_size = (size_t)st.st_size;
    idx->built_epoch = hdr->built_epoch;
    idx->hdr = hdr;
    idx->buckets = (const TriBucket *)((const char *)map + sizeof(IndexHeader));
    idx->syms = (const SymRec *)((const char *)idx->buckets + NUM_TRIGRAMS * sizeof(TriBucket));
    idx->postings = (const uint32_t *)((const char *)idx->syms + hdr->num_recs * sizeof(SymRec));
    idx->strs = (const char *)idx->postings + hdr->postings_len * sizeof(uint32_t);
    return true;
}

static void close_sym_index(MappedSymIdx *idx) {
    if (idx->map && idx->map != MAP_FAILED) {
        munmap(idx->map, idx->map_size);
    }
    memset(idx, 0, sizeof(*idx));
}

/* Zero-daemon lazy freshness: check `stat().st_mtime > idx->built_epoch` on disk */
static bool file_is_stale(const MappedSymIdx *idx, const char *path) {
    if (!idx || !idx->hdr || !path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return true;
    return ((uint64_t)st.st_mtime > idx->built_epoch);
}

static bool index_has_any_stale_file(const MappedSymIdx *idx) {
    if (!idx || !idx->hdr) return false;
    uint32_t prev_path_off = UINT32_MAX;
    for (uint32_t i = 0; i < idx->hdr->num_recs; i++) {
        uint32_t poff = idx->syms[i].path_off;
        if (poff == prev_path_off) continue;
        prev_path_off = poff;
        if (file_is_stale(idx, idx->strs + poff)) return true;
    }
    return false;
}

static bool roots_or_index_stale(const MappedSymIdx *idx, char roots[][PATH_MAX], int n_roots) {
    for (int r = 0; r < n_roots; r++) {
        if (file_is_stale(idx, roots[r])) return true;
    }
    return index_has_any_stale_file(idx);
}

static void refresh_sym_index(MappedSymIdx *idx, bool *idx_ok, bool *rebuilt_once,
                              const char *argv0, char roots[][PATH_MAX], int n_roots) {
    close_sym_index(idx);
    *rebuilt_once = true;
    *idx_ok = (n_roots > 0) && open_sym_index(idx, argv0, roots, n_roots, true, true, NULL);
}

static size_t gallop_seek(const uint32_t *arr, size_t lo, size_t n, uint32_t target) {
    if (lo >= n || arr[lo] >= target) return lo;
    size_t step = 1;
    size_t hi = lo + step;
    while (hi < n && arr[hi] < target) {
        lo = hi;
        step <<= 1;
        hi = lo + step;
    }
    if (hi > n) hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

typedef struct {
    uint32_t id;
    int score;
} SymMatch;

static int cmp_match_desc(const void *a, const void *b) {
    const SymMatch *ma = (const SymMatch *)a, *mb = (const SymMatch *)b;
    if (mb->score != ma->score) return (mb->score > ma->score) - (mb->score < ma->score);
    return (ma->id > mb->id) - (ma->id < mb->id);
}

static int find_symbols(const MappedSymIdx *idx, const char *sym_query, const char *file_filter,
                        SymMatch *out, int max_out) {
    size_t qlen = strlen(sym_query);
    if (qlen == 0) return 0;

    uint32_t qtris[64];
    uint32_t qlens[64];
    uint32_t qoffs[64];
    int n_qt = 0;

    if (qlen >= 3) {
        const unsigned char *u = (const unsigned char *)sym_query;
        for (size_t i = 0; i + 2 < qlen && n_qt < 64; i++) {
            uint32_t tri = pack_tri(u[i], u[i + 1], u[i + 2]);
            bool dup = false;
            for (int k = 0; k < n_qt; k++) {
                if (qtris[k] == tri) { dup = true; break; }
            }
            if (!dup) {
                qtris[n_qt] = tri;
                qlens[n_qt] = idx->buckets[tri].len;
                qoffs[n_qt] = idx->buckets[tri].offset;
                n_qt++;
            }
        }
        /* Put rarest bucket at index 0 */
        for (int i = 1; i < n_qt; i++) {
            if (qlens[i] < qlens[0]) {
                uint32_t t0 = qtris[0], l0 = qlens[0], o0 = qoffs[0];
                qtris[0] = qtris[i]; qlens[0] = qlens[i]; qoffs[0] = qoffs[i];
                qtris[i] = t0; qlens[i] = l0; qoffs[i] = o0;
            }
        }
    }

    SymMatch *cands = malloc(4096 * sizeof(SymMatch));
    int n_cands = 0;

    if (n_qt > 0) {
        if (qlens[0] == 0) {
            free(cands);
            return 0;
        }
        const uint32_t *base = idx->postings + qoffs[0];
        uint32_t blen = qlens[0];
        size_t cursors[64] = {0};

        for (uint32_t i = 0; i < blen; i++) {
            uint32_t id = base[i];
            bool ok = true;
            for (int k = 1; k < n_qt; k++) {
                const uint32_t *lst = idx->postings + qoffs[k];
                size_t pos = gallop_seek(lst, cursors[k], qlens[k], id);
                cursors[k] = pos;
                if (pos >= qlens[k] || lst[pos] != id) { ok = false; break; }
            }
            if (!ok) continue;

            const SymRec *sr = &idx->syms[id];
            const char *sym = idx->strs + sr->sym_off;
            const char *path = idx->strs + sr->path_off;
            if (file_filter && file_filter[0] && !ci_strstr(path, file_filter)) continue;

            const char *m = ci_strstr(sym, sym_query);
            if (!m) continue;

            int sc = 1000 - (int)sr->sym_len * 3;
            if (strcmp(sym, sym_query) == 0) sc += 3000;
            else if (strcasecmp(sym, sym_query) == 0) sc += 2200;
            else if (m == sym) sc += 900;

            /* Prefer implementation files over tests/headers when tied */
            if (strstr(path, "/tests/") || strstr(path, "test_")) sc -= 180;
            const char *dot = strrchr(path, '.');
            if (dot && strcmp(dot, ".h") == 0) sc -= 60;

            if (n_cands < 4096) {
                cands[n_cands].id = id;
                cands[n_cands].score = sc;
                n_cands++;
            }
        }
    } else {
        for (uint32_t id = 0; id < idx->hdr->num_recs && n_cands < 4096; id++) {
            const SymRec *sr = &idx->syms[id];
            const char *sym = idx->strs + sr->sym_off;
            const char *path = idx->strs + sr->path_off;
            if (file_filter && file_filter[0] && !ci_strstr(path, file_filter)) continue;
            if (strcmp(sym, sym_query) == 0 || strcasecmp(sym, sym_query) == 0) {
                cands[n_cands].id = id;
                cands[n_cands].score = (strcmp(sym, sym_query) == 0) ? 4000 : 3000;
                n_cands++;
            }
        }
    }

    qsort(cands, (size_t)n_cands, sizeof(SymMatch), cmp_match_desc);
    int ret = (n_cands < max_out) ? n_cands : max_out;
    for (int i = 0; i < ret; i++) out[i] = cands[i];
    free(cands);
    return ret;
}

static bool matches_have_stale_file(const MappedSymIdx *idx, const SymMatch *matches, int nm) {
    for (int i = 0; i < nm; i++) {
        const SymRec *sr = &idx->syms[matches[i].id];
        if (file_is_stale(idx, idx->strs + sr->path_off)) return true;
    }
    return false;
}

static int resolve_symbols_lazy(MappedSymIdx *idx, bool *idx_ok, bool *rebuilt_once,
                                const char *argv0, char roots[][PATH_MAX], int n_roots,
                                const char *sym_query, const char *file_filter,
                                SymMatch *out, int max_out) {
    int nm = (*idx_ok) ? find_symbols(idx, sym_query, file_filter, out, max_out) : 0;
    if (!*rebuilt_once && *idx_ok) {
        bool stale = (nm > 0) ? matches_have_stale_file(idx, out, nm)
                              : roots_or_index_stale(idx, roots, n_roots);
        if (stale) {
            refresh_sym_index(idx, idx_ok, rebuilt_once, argv0, roots, n_roots);
            nm = (*idx_ok) ? find_symbols(idx, sym_query, file_filter, out, max_out) : 0;
        }
    }
    return nm;
}

typedef struct {
    uint32_t line;
    uint8_t kind;
    const char *sym;
    const char *sig;
} FileSymEntry;

static int cmp_filesym_line(const void *a, const void *b) {
    const FileSymEntry *fa = (const FileSymEntry *)a, *fb = (const FileSymEntry *)b;
    return (fa->line > fb->line) - (fa->line < fb->line);
}

static bool resolve_existing_file(const char *spec, char *resolved, size_t cap) {
    if (realpath(spec, resolved)) {
        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    struct stat st;
    if (stat(spec, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(resolved, cap, "%s", spec);
        return true;
    }
    return false;
}

/* Mode 4: File Structural Skeleton (-s / --skeleton) */
static int run_skeleton_mode(const MappedSymIdx *idx, const char *file_arg) {
    char full_path[PATH_MAX];
    if (!resolve_existing_file(file_arg, full_path, sizeof(full_path))) {
        fprintf(stderr, "hslice: file not found: %s\nFIX: run `hfind %s` to get exact path.\n",
                file_arg, file_arg);
        return 1;
    }

    FileLines fl;
    if (!load_file_lines(full_path, &fl)) {
        fprintf(stderr, "hslice: cannot read %s\n", full_path);
        return 1;
    }
    SliceLang lang = detect_lang(full_path);

    printf("=== SKELETON %s (%d lines) ===\n", full_path, fl.count);

    /* Print compact imports/includes (max 15 lines) */
    int imp_printed = 0;
    for (int i = 0; i < fl.count && i < 80 && imp_printed < 15; i++) {
        const char *s = fl.lines[i];
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "#include ", 9) == 0 || strncmp(s, "import ", 7) == 0 ||
            strncmp(s, "from ", 5) == 0 || strncmp(s, "use ", 4) == 0) {
            printf("  %4d: %s\n", i + 1, s);
            imp_printed++;
        }
    }

    /* Gather symbols in this file from hsym.idx */
    FileSymEntry *entries = malloc(2048 * sizeof(FileSymEntry));
    int n_ent = 0;
    if (idx && idx->hdr) {
        for (uint32_t i = 0; i < idx->hdr->num_recs && n_ent < 2048; i++) {
            const SymRec *sr = &idx->syms[i];
            if (strcmp(idx->strs + sr->path_off, full_path) == 0) {
                entries[n_ent].line = sr->line;
                entries[n_ent].kind = sr->kind;
                entries[n_ent].sym = idx->strs + sr->sym_off;
                entries[n_ent].sig = idx->strs + sr->sig_off;
                n_ent++;
            }
        }
    }

    if (n_ent > 0) {
        qsort(entries, (size_t)n_ent, sizeof(FileSymEntry), cmp_filesym_line);
        printf("SYMBOLS (%d):\n", n_ent);
        for (int i = 0; i < n_ent; i++) {
            int s_ln = 0, e_ln = 0;
            compute_symbol_bounds(&fl, lang, (int)entries[i].line, &s_ln, &e_ln);
            const char *kn = (entries[i].kind < 9) ? KIND_NAMES[entries[i].kind] : "sym";
            printf("  %4d-%-4d [%s] %s\n", s_ln, e_ln, kn, entries[i].sig);
        }
    } else {
        /* Fallback line-scan if file wasn't in indexed roots */
        printf("OUTLINE:\n");
        for (int i = 0; i < fl.count; i++) {
            const char *s = fl.lines[i];
            const char *t = s;
            while (*t == ' ' || *t == '\t') t++;
            if (strncmp(t, "def ", 4) == 0 || strncmp(t, "async def ", 10) == 0 ||
                strncmp(t, "class ", 6) == 0 || strncmp(t, "fn ", 3) == 0 ||
                strncmp(t, "pub fn ", 7) == 0 || strncmp(t, "func ", 5) == 0 ||
                strncmp(t, "export function ", 16) == 0 || strncmp(t, "function ", 9) == 0) {
                int s_ln = 0, e_ln = 0;
                compute_symbol_bounds(&fl, lang, i + 1, &s_ln, &e_ln);
                printf("  %4d-%-4d %s\n", s_ln, e_ln, t);
            }
        }
    }

    free(entries);
    free_file_lines(&fl);
    return 0;
}

/* Parse `file:line` or `file:start-end` */
static bool try_parse_file_spec(const char *arg, char *out_path, size_t cap,
                                int *out_line1, int *out_line2, bool *is_range) {
    const char *colon = strrchr(arg, ':');
    if (!colon || colon == arg || !isdigit((unsigned char)colon[1])) return false;

    size_t plen = (size_t)(colon - arg);
    if (plen >= cap) return false;
    char raw_path[PATH_MAX];
    memcpy(raw_path, arg, plen);
    raw_path[plen] = '\0';

    if (!resolve_existing_file(raw_path, out_path, cap)) return false;

    const char *num_part = colon + 1;
    char *endp = NULL;
    long v1 = strtol(num_part, &endp, 10);
    if (v1 <= 0) return false;
    *out_line1 = (int)v1;
    if (endp && (*endp == '-' || *endp == ':') && isdigit((unsigned char)endp[1])) {
        long v2 = strtol(endp + 1, NULL, 10);
        *out_line2 = (v2 >= v1) ? (int)v2 : (int)v1;
        *is_range = true;
    } else {
        *out_line2 = (int)v1;
        *is_range = false;
    }
    return true;
}

static void slice_enclosing_at_line(const MappedSymIdx *idx, const char *full_path,
                                    int target_line, int max_lines) {
    FileLines fl;
    if (!load_file_lines(full_path, &fl)) {
        fprintf(stderr, "hslice: cannot read %s\n", full_path);
        return;
    }
    SliceLang lang = detect_lang(full_path);

    /* Find closest symbol in full_path with line <= target_line */
    uint32_t best_line = 0;
    const SymRec *best_sr = NULL;
    if (idx && idx->hdr) {
        for (uint32_t i = 0; i < idx->hdr->num_recs; i++) {
            const SymRec *sr = &idx->syms[i];
            if (sr->line <= (uint32_t)target_line && sr->line > best_line &&
                strcmp(idx->strs + sr->path_off, full_path) == 0) {
                best_line = sr->line;
                best_sr = sr;
            }
        }
    }

    if (best_sr) {
        int s_ln = 0, e_ln = 0;
        compute_symbol_bounds(&fl, lang, (int)best_sr->line, &s_ln, &e_ln);
        if (target_line >= s_ln && target_line <= e_ln + 2) {
            char tag[192];
            const char *kn = (best_sr->kind < 9) ? KIND_NAMES[best_sr->kind] : "sym";
            snprintf(tag, sizeof(tag), "%s %s @L%d", kn, idx->strs + best_sr->sym_off, target_line);
            print_slice(&fl, full_path, s_ln, e_ln, tag, max_lines);
            free_file_lines(&fl);
            return;
        }
    }

    /* Fallback window around target_line */
    int s_ln = (target_line > 18) ? (target_line - 18) : 1;
    int e_ln = (target_line + 22 <= fl.count) ? (target_line + 22) : fl.count;
    char tag[64];
    snprintf(tag, sizeof(tag), "context @L%d", target_line);
    print_slice(&fl, full_path, s_ln, e_ln, tag, max_lines);
    free_file_lines(&fl);
}

static void run_xref_refs(const char *sym, const char *scope_dir, const char *default_root) {
    char dir[PATH_MAX];
    if (scope_dir && scope_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", scope_dir);
    } else if (default_root && default_root[0]) {
        snprintf(dir, sizeof(dir), "%s", default_root);
    } else if (!getcwd(dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), ".");
    }

    printf("=== REFS (%s in %s) ===\n", sym, dir);
    fflush(stdout);

    int pipefd[2];
    if (pipe(pipefd) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pipefd[1]);
        char *rg_argv[] = {
            "rg", "-n", "--no-heading", "--color=never", "-w",
            "--max-columns=180", "--max-columns-preview",
            "-g", "!.git", "-g", "!node_modules", "-g", "!.venv",
            "-g", "!target", "-g", "!build", "-g", "!dist",
            (char *)sym, dir, NULL
        };
        execvp("rg", rg_argv);

        char *grep_argv[] = {
            "grep", "-rnEw",
            "--exclude-dir=.git", "--exclude-dir=node_modules", "--exclude-dir=.venv",
            "--exclude-dir=target", "--exclude-dir=build", "--exclude-dir=dist",
            (char *)sym, dir, NULL
        };
        execvp("grep", grep_argv);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *fp = fdopen(pipefd[0], "r");
    if (fp) {
        char line[512];
        int printed = 0, total = 0;
        while (fgets(line, sizeof(line), fp)) {
            total++;
            if (printed < 18) {
                fputs(line, stdout);
                printed++;
            }
        }
        if (total > printed) {
            printf("  ... [+%d more refs; run `hsearch -g \"\\b%s\\b\"` for more]\n",
                   total - printed, sym);
        } else if (total == 0) {
            printf("  (0 external references found)\n");
        }
        fclose(fp);
    }
    int st = 0;
    (void)waitpid(pid, &st, 0);
}

/* ==================== REPOSITORY TOKEN AUDIT (`--audit` / `-A`) ==================== */

static bool is_excluded_audit_dir(const char *name) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        return true;
    if (strncmp(name, ".venv", 5) == 0 || strncmp(name, "venv", 4) == 0 ||
        strncmp(name, "python3.", 8) == 0 ||
        strstr(name, ".dist-info") != NULL || strstr(name, ".egg-info") != NULL)
        return true;
    static const char *const skip[] = {
        ".git", ".hg", ".svn", "node_modules", "__pycache__", ".cache", "site-packages",
        ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", "target",
        "vendor", "third_party", "dist", "build", ".next", "out",
        "coverage", ".pnpm", ".turbo", NULL
    };
    for (int i = 0; skip[i]; i++) {
        if (strcmp(name, skip[i]) == 0) return true;
    }
    return false;
}

static bool is_virtualenv_audit_dir(const char *dir_path) {
    char cfg[PATH_MAX + 16];
    int n = snprintf(cfg, sizeof(cfg), "%s/pyvenv.cfg", dir_path);
    return (n > 0 && (size_t)n < sizeof(cfg) && access(cfg, F_OK) == 0);
}

static bool dir_has_dist_info_audit(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return false;
    struct dirent *de;
    int dist_cnt = 0;
    while ((de = readdir(d)) != NULL) {
        if (strstr(de->d_name, ".dist-info") != NULL && ++dist_cnt >= 3) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

static bool is_audit_source_file(const char *name) {
    if (strstr(name, ".min.") || strstr(name, ".bundle.")) return false;
    return detect_lang(name) != LANG_GENERIC;
}

static uint32_t count_source_files_rec(const char *dir, int depth) {
    if (depth > 18) return 0;
    bool flat_target = dir_has_dist_info_audit(dir);
    DIR *d = opendir(dir);
    if (!d) return 0;

    uint32_t total = 0;
    uint32_t src_in_dir = 0;
    struct dirent *de;
    char child[PATH_MAX];
    while ((de = readdir(d)) != NULL) {
        if (is_excluded_audit_dir(de->d_name)) continue;
        int n = snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;

        unsigned char dt = DT_UNKNOWN;
#ifdef DT_DIR
        dt = de->d_type;
#endif
        if (dt == DT_UNKNOWN) {
            struct stat st;
            if (lstat(child, &st) == 0) {
                if (S_ISDIR(st.st_mode)) dt = DT_DIR;
                else if (S_ISREG(st.st_mode)) dt = DT_REG;
            }
        }
        if (dt == DT_DIR) {
            if (flat_target || is_virtualenv_audit_dir(child)) continue;
            total += count_source_files_rec(child, depth + 1);
        } else if (dt == DT_REG) {
            if (is_audit_source_file(de->d_name) && ++src_in_dir <= 8192) {
                total++;
            }
        }
    }
    closedir(d);
    return total;
}

typedef struct {
    uint32_t sym_id;
    uint32_t line;
    uint8_t  kind;
    uint8_t  is_test;
    size_t   file_bytes;
    size_t   slice_bytes;
    uint64_t file_tokens;
    uint64_t slice_tokens;
} AuditSym;

static int audit_sym_tier(const AuditSym *s) {
    if (s->kind == 0 && !s->is_test) return 2;
    if (s->kind == 0) return 1;
    return 0;
}

static int cmp_audit_largest(const void *a, const void *b) {
    const AuditSym *sa = (const AuditSym *)a;
    const AuditSym *sb = (const AuditSym *)b;
    int ta = audit_sym_tier(sa);
    int tb = audit_sym_tier(sb);
    if (ta != tb) return tb - ta;
    if (sa->slice_bytes != sb->slice_bytes) {
        return (sb->slice_bytes > sa->slice_bytes) - (sb->slice_bytes < sa->slice_bytes);
    }
    if (sa->file_bytes != sb->file_bytes) {
        return (sb->file_bytes > sa->file_bytes) - (sb->file_bytes < sa->file_bytes);
    }
    return (sa->sym_id > sb->sym_id) - (sa->sym_id < sb->sym_id);
}

static void format_u64_commas(uint64_t v, char *out, size_t cap) {
    char raw[32];
    int n = snprintf(raw, sizeof(raw), "%" PRIu64, v);
    if (n <= 0 || cap == 0) {
        if (cap > 0) out[0] = '\0';
        return;
    }
    int commas = (n - 1) / 3;
    size_t total = (size_t)(n + commas);
    if (total + 1 > cap) {
        snprintf(out, cap, "%s", raw);
        return;
    }
    out[total] = '\0';
    int src = n - 1;
    int dst = (int)total - 1;
    int grp = 0;
    while (src >= 0) {
        out[dst--] = raw[src--];
        if (++grp == 3 && src >= 0) {
            out[dst--] = ',';
            grp = 0;
        }
    }
}

static void truncate_right(const char *src, char *out, size_t width) {
    size_t len = strlen(src);
    if (len <= width) {
        memcpy(out, src, len + 1);
        return;
    }
    if (width <= 2) {
        memcpy(out, src, width);
        out[width] = '\0';
        return;
    }
    memcpy(out, src, width - 2);
    out[width - 2] = '.';
    out[width - 1] = '.';
    out[width] = '\0';
}

static void truncate_left(const char *src, char *out, size_t width) {
    size_t len = strlen(src);
    if (len <= width) {
        memcpy(out, src, len + 1);
        return;
    }
    if (width <= 2) {
        memcpy(out, src + (len - width), width);
        out[width] = '\0';
        return;
    }
    out[0] = '.';
    out[1] = '.';
    memcpy(out + 2, src + (len - (width - 2)), width - 2);
    out[width] = '\0';
}

static void print_hairline(void) {
    for (int i = 0; i < 78; i++) {
        fputs("\xe2\x94\x80", stdout);
    }
    fputc('\n', stdout);
}

static int run_audit_mode(const char *argv0, const char *audit_arg, const char *cli_root,
                          bool force_rebuild) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char audit_roots[HSLICE_MAX_ROOTS][PATH_MAX];
    int n_roots = 0;
    if (audit_arg && audit_arg[0]) {
        if (!hslice_resolve_dir(audit_arg, audit_roots[0], sizeof(audit_roots[0]))) {
            fprintf(stderr, "hslice: directory not found: %s\n", audit_arg);
            return 1;
        }
        n_roots = 1;
    } else {
        n_roots = hslice_discover_roots(cli_root, audit_roots, HSLICE_MAX_ROOTS);
        if (n_roots <= 0) {
            fprintf(stderr, "hslice: failed to resolve workspace directory\n");
            return 1;
        }
    }

    MappedSymIdx idx;
    bool did_build = false;
    bool idx_ok = open_sym_index(&idx, argv0, audit_roots, n_roots, force_rebuild, true, &did_build);
    bool rebuilt = force_rebuild || did_build;
    if (idx_ok && !rebuilt && roots_or_index_stale(&idx, audit_roots, n_roots)) {
        refresh_sym_index(&idx, &idx_ok, &rebuilt, argv0, audit_roots, n_roots);
    }
    if (!idx_ok) {
        fprintf(stderr, "hslice: failed to index symbols in %s\n", audit_roots[0]);
        return 1;
    }

    uint32_t walked_files = 0;
    for (int r = 0; r < n_roots; r++) {
        walked_files += count_source_files_rec(audit_roots[r], 0);
    }

    uint32_t n_recs = idx.hdr->num_recs;
    AuditSym *items = malloc((size_t)(n_recs ? n_recs : 1) * sizeof(AuditSym));
    if (!items) {
        close_sym_index(&idx);
        return 1;
    }

    uint32_t n_items = 0;
    uint32_t unique_idx_files = 0;
    uint64_t all_file_tok = 0;
    uint64_t all_slice_tok = 0;

    FileLines cur_fl = {0};
    uint32_t cur_path_off = UINT32_MAX;
    bool cur_fl_ok = false;
    bool cur_is_test = false;
    SliceLang cur_lang = LANG_GENERIC;

    for (uint32_t i = 0; i < n_recs; i++) {
        const SymRec *sr = &idx.syms[i];
        if (sr->path_off != cur_path_off) {
            free_file_lines(&cur_fl);
            cur_path_off = sr->path_off;
            const char *p = idx.strs + cur_path_off;
            cur_fl_ok = load_file_lines(p, &cur_fl);
            cur_lang = cur_fl_ok ? detect_lang(p) : LANG_GENERIC;
            const char *base = strrchr(p, '/');
            base = base ? (base + 1) : p;
            cur_is_test = (strstr(p, "/tests/") != NULL || strncmp(base, "test_", 5) == 0);
            if (cur_fl_ok) unique_idx_files++;
        }
        if (!cur_fl_ok || cur_fl.raw_len == 0) continue;

        int s_ln = 0, e_ln = 0;
        compute_symbol_bounds(&cur_fl, cur_lang, (int)sr->line, &s_ln, &e_ln);
        if (s_ln < 1) s_ln = 1;
        if (e_ln > cur_fl.count) e_ln = cur_fl.count;
        if (e_ln < s_ln) e_ln = s_ln;

        size_t s_bytes = 0;
        for (int ln = s_ln; ln <= e_ln; ln++) {
            s_bytes += strlen(cur_fl.lines[ln - 1]) + 1;
        }
        if (s_bytes > cur_fl.raw_len) s_bytes = cur_fl.raw_len;
        if (s_bytes == 0) s_bytes = 1;

        uint64_t f_tok = (uint64_t)((cur_fl.raw_len + 3) / 4);
        uint64_t s_tok = (uint64_t)((s_bytes + 3) / 4);
        if (s_tok > f_tok) s_tok = f_tok;

        items[n_items].sym_id = i;
        items[n_items].line = sr->line;
        items[n_items].kind = sr->kind;
        items[n_items].is_test = cur_is_test ? 1 : 0;
        items[n_items].file_bytes = cur_fl.raw_len;
        items[n_items].slice_bytes = s_bytes;
        items[n_items].file_tokens = f_tok;
        items[n_items].slice_tokens = s_tok;
        n_items++;

        all_file_tok += f_tok;
        all_slice_tok += s_tok;
    }
    free_file_lines(&cur_fl);

    if (n_items > 1) {
        qsort(items, (size_t)n_items, sizeof(AuditSym), cmp_audit_largest);
    }

    uint32_t total_files = (walked_files >= unique_idx_files) ? walked_files : unique_idx_files;
    uint32_t top_n = (n_items < 8U) ? n_items : 8U;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    const char *repo_base = strrchr(audit_roots[0], '/');
    repo_base = (repo_base && repo_base[1]) ? (repo_base + 1) : audit_roots[0];
    size_t root0_len = strlen(audit_roots[0]);

    char files_buf[32], syms_buf[32];
    format_u64_commas(total_files, files_buf, sizeof(files_buf));
    format_u64_commas(n_items, syms_buf, sizeof(syms_buf));

    printf("hslice audit  %s (%s %s, %s %s)\n",
           repo_base,
           files_buf, (total_files == 1) ? "file" : "files",
           syms_buf, (n_items == 1) ? "symbol" : "symbols");
    print_hairline();

    if (n_items == 0) {
        printf("0 indexed symbols found in %s\n", audit_roots[0]);
        print_hairline();
        printf("context saved: 0.0%%  \xc2\xb7  density: 1.0x  \xc2\xb7  elapsed: %.1f ms\n"
               "github.com/Hephaestus-Zero/hslice\n",
               elapsed_ms);
        free(items);
        close_sym_index(&idx);
        return 0;
    }

    printf("%-24s  %-25s %9s %9s %6s\n",
           "symbol", "location", "file tok", "slice tok", "saved");
    print_hairline();

    uint64_t sample_file_tok = 0;
    uint64_t sample_slice_tok = 0;
    bool all_top_fn = true;

    for (uint32_t k = 0; k < top_n; k++) {
        const AuditSym *it = &items[k];
        const SymRec *sr = &idx.syms[it->sym_id];
        const char *sym_name = idx.strs + sr->sym_off;
        const char *full_path = idx.strs + sr->path_off;
        const char *rel_path = full_path;
        if (strncmp(full_path, audit_roots[0], root0_len) == 0 && full_path[root0_len] == '/') {
            rel_path = full_path + root0_len + 1;
        }

        if (it->kind != 0) all_top_fn = false;
        sample_file_tok += it->file_tokens;
        sample_slice_tok += it->slice_tokens;

        char sym_col[32];
        char loc_raw[PATH_MAX + 32];
        char loc_col[32];
        char ftok_col[32];
        char stok_col[32];
        char pct_col[16];

        truncate_right(sym_name, sym_col, 24);
        snprintf(loc_raw, sizeof(loc_raw), "%.4096s:%u", rel_path, it->line);
        truncate_left(loc_raw, loc_col, 25);
        format_u64_commas(it->file_tokens, ftok_col, sizeof(ftok_col));
        format_u64_commas(it->slice_tokens, stok_col, sizeof(stok_col));

        double row_saved = (it->file_tokens > 0 && it->file_tokens >= it->slice_tokens)
            ? (100.0 * (double)(it->file_tokens - it->slice_tokens) / (double)it->file_tokens)
            : 0.0;
        snprintf(pct_col, sizeof(pct_col), "%.1f%%", row_saved);

        printf("%-24s  %-25s %9s %9s %6s\n",
               sym_col, loc_col, ftok_col, stok_col, pct_col);
    }

    print_hairline();

    if (n_items > top_n) {
        char sample_lbl[64];
        char s_ftok[32], s_stok[32], s_pct[16];
        snprintf(sample_lbl, sizeof(sample_lbl), "sample (top %u largest %s)",
                 top_n, all_top_fn ? "functions" : "symbols");
        format_u64_commas(sample_file_tok, s_ftok, sizeof(s_ftok));
        format_u64_commas(sample_slice_tok, s_stok, sizeof(s_stok));
        double sample_saved = (sample_file_tok > 0 && sample_file_tok >= sample_slice_tok)
            ? (100.0 * (double)(sample_file_tok - sample_slice_tok) / (double)sample_file_tok)
            : 0.0;
        snprintf(s_pct, sizeof(s_pct), "%.1f%%", sample_saved);
        printf("%-51s %9s %9s %6s\n", sample_lbl, s_ftok, s_stok, s_pct);
    }

    char all_lbl[64];
    char a_ftok[32], a_stok[32], a_pct[16];
    snprintf(all_lbl, sizeof(all_lbl), "all %s indexed %s",
             syms_buf, (n_items == 1) ? "symbol" : "symbols");
    format_u64_commas(all_file_tok, a_ftok, sizeof(a_ftok));
    format_u64_commas(all_slice_tok, a_stok, sizeof(a_stok));
    double all_saved = (all_file_tok > 0 && all_file_tok >= all_slice_tok)
        ? (100.0 * (double)(all_file_tok - all_slice_tok) / (double)all_file_tok)
        : 0.0;
    double density_x = (all_slice_tok > 0)
        ? ((double)all_file_tok / (double)all_slice_tok)
        : 1.0;
    snprintf(a_pct, sizeof(a_pct), "%.1f%%", all_saved);
    printf("%-51s %9s %9s %6s\n", all_lbl, a_ftok, a_stok, a_pct);

    print_hairline();
    printf("context saved: %.1f%%  \xc2\xb7  density: %.1fx  \xc2\xb7  elapsed: %.1f ms\n"
           "github.com/Hephaestus-Zero/hslice\n",
           all_saved, density_x, elapsed_ms);

    free(items);
    close_sym_index(&idx);
    return 0;
}

static void print_usage(void) {
    printf("hslice v%s: Sub-2ms C11 Structural Code Slicer & Single-Shot Recon\n"
           "Usage:\n"
           "  hslice <symbol> [symbol2 ...] [--root DIR] [-f FILE_FILTER] [-n TOP_DEFS] [-m MAX_LINES]\n"
           "  hslice <file>:<line> [<file2>:<start>-<end> ...]   (batch traceback / range slice)\n"
           "  hslice -s <file>                                   (structural file skeleton)\n"
           "  hslice -x <symbol> [dir] [--root DIR]              (1-shot definition + callers)\n"
           "  hslice -A, --audit [dir]                           (repo token-savings receipt)\n",
           HSLICE_VERSION);
}

int main(int argc, char **argv) {
    bool skeleton_mode = false;
    bool xref_mode = false;
    bool audit_mode = false;
    bool force_rebuild = false;
    const char *file_filter = NULL;
    const char *cli_root = NULL;
    int top_defs = 2;
    int max_lines = 110;

    const char *targets[32];
    int n_targets = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--skeleton") == 0) {
            skeleton_mode = true;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--xref") == 0) {
            xref_mode = true;
            top_defs = 1;
        } else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--audit") == 0) {
            audit_mode = true;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rebuild") == 0) {
            force_rebuild = true;
        } else if ((strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--root") == 0) && i + 1 < argc) {
            cli_root = argv[++i];
        } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc) {
            file_filter = argv[++i];
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--defs") == 0) && i + 1 < argc) {
            top_defs = atoi(argv[++i]);
            if (top_defs < 1) top_defs = 1;
            if (top_defs > 10) top_defs = 10;
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--max-lines") == 0) && i + 1 < argc) {
            max_lines = atoi(argv[++i]);
            if (max_lines < 0) max_lines = 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("hslice v%s\n", HSLICE_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (n_targets < 32) {
            targets[n_targets++] = argv[i];
        }
    }

    if (audit_mode) {
        const char *audit_arg = (n_targets >= 1) ? targets[0] : NULL;
        return run_audit_mode(argv[0], audit_arg, cli_root, force_rebuild);
    }

    char roots[HSLICE_MAX_ROOTS][PATH_MAX];
    int n_roots = hslice_discover_roots(cli_root, roots, HSLICE_MAX_ROOTS);

    if (n_targets == 0) {
        if (force_rebuild && n_roots > 0) {
            MappedSymIdx tmp_idx;
            bool ok = open_sym_index(&tmp_idx, argv[0], roots, n_roots, true, false, NULL);
            close_sym_index(&tmp_idx);
            return ok ? 0 : 1;
        }
        print_usage();
        return 1;
    }

    MappedSymIdx idx;
    bool did_build = false;
    bool idx_ok = (n_roots > 0) && open_sym_index(&idx, argv[0], roots, n_roots, force_rebuild, true, &did_build);
    bool rebuilt_once = force_rebuild || did_build;

    if (skeleton_mode) {
        int rc = 0;
        for (int i = 0; i < n_targets; i++) {
            char skel_path[PATH_MAX];
            if (idx_ok && !rebuilt_once &&
                resolve_existing_file(targets[i], skel_path, sizeof(skel_path)) &&
                file_is_stale(&idx, skel_path)) {
                refresh_sym_index(&idx, &idx_ok, &rebuilt_once, argv[0], roots, n_roots);
            }
            rc |= run_skeleton_mode(idx_ok ? &idx : NULL, targets[i]);
        }
        close_sym_index(&idx);
        return rc;
    }

    if (xref_mode) {
        const char *sym = targets[0];
        const char *scope_dir = (n_targets >= 2) ? targets[1] : NULL;
        const char *default_root = (n_roots > 0) ? roots[0] : ".";
        SymMatch matches[2];
        int nm = resolve_symbols_lazy(&idx, &idx_ok, &rebuilt_once, argv[0],
                                      roots, n_roots, sym, file_filter, matches, 2);
        if (nm == 0) {
            fprintf(stderr, "hslice: 0 symbol definitions for \"%s\".\n"
                            "NEXT: running body reference search (`hsearch -g`) instead:\n", sym);
            run_xref_refs(sym, scope_dir, default_root);
            close_sym_index(&idx);
            return 0;
        }
        for (int m = 0; m < nm && m < top_defs; m++) {
            const SymRec *sr = &idx.syms[matches[m].id];
            const char *path = idx.strs + sr->path_off;
            FileLines fl;
            if (load_file_lines(path, &fl)) {
                int s_ln = 0, e_ln = 0;
                compute_symbol_bounds(&fl, detect_lang(path), (int)sr->line, &s_ln, &e_ln);
                char tag[192];
                const char *kn = (sr->kind < 9) ? KIND_NAMES[sr->kind] : "sym";
                snprintf(tag, sizeof(tag), "%s %s", kn, idx.strs + sr->sym_off);
                print_slice(&fl, path, s_ln, e_ln, tag, max_lines);
                free_file_lines(&fl);
            }
        }
        run_xref_refs(sym, scope_dir, default_root);
        close_sym_index(&idx);
        return 0;
    }

    int exit_rc = 0;
    for (int t = 0; t < n_targets; t++) {
        char fpath[PATH_MAX];
        int l1 = 0, l2 = 0;
        bool is_range = false;
        if (try_parse_file_spec(targets[t], fpath, sizeof(fpath), &l1, &l2, &is_range)) {
            if (is_range) {
                FileLines fl;
                if (load_file_lines(fpath, &fl)) {
                    print_slice(&fl, fpath, l1, l2, "range", max_lines);
                    free_file_lines(&fl);
                } else {
                    fprintf(stderr, "hslice: cannot read %s\n", fpath);
                    exit_rc = 1;
                }
            } else {
                if (idx_ok && !rebuilt_once && file_is_stale(&idx, fpath)) {
                    refresh_sym_index(&idx, &idx_ok, &rebuilt_once, argv[0], roots, n_roots);
                }
                slice_enclosing_at_line(idx_ok ? &idx : NULL, fpath, l1, max_lines);
            }
            continue;
        }

        /* Check if user passed bare file path without :line -> show skeleton */
        if (resolve_existing_file(targets[t], fpath, sizeof(fpath))) {
            if (idx_ok && !rebuilt_once && file_is_stale(&idx, fpath)) {
                refresh_sym_index(&idx, &idx_ok, &rebuilt_once, argv[0], roots, n_roots);
            }
            run_skeleton_mode(idx_ok ? &idx : NULL, fpath);
            continue;
        }

        /* Otherwise treat target as symbol query */
        if (!idx_ok) {
            fprintf(stderr, "hslice: symbol index unavailable. Run `hsearch --rebuild`.\n");
            exit_rc = 1;
            continue;
        }

        SymMatch matches[10];
        int nm = resolve_symbols_lazy(&idx, &idx_ok, &rebuilt_once, argv[0],
                                      roots, n_roots, targets[t], file_filter, matches, top_defs);
        if (nm == 0) {
            fprintf(stderr, "hslice: 0 symbol definitions matched \"%s\"%s%s.\n"
                            "NEXT: try `hsearch -g \"%s\"` for raw text occurrences or `hfind %s` for filenames.\n",
                    targets[t],
                    file_filter ? " in " : "", file_filter ? file_filter : "",
                    targets[t], targets[t]);
            exit_rc = 1;
            continue;
        }

        for (int m = 0; m < nm; m++) {
            const SymRec *sr = &idx.syms[matches[m].id];
            const char *path = idx.strs + sr->path_off;
            FileLines fl;
            if (load_file_lines(path, &fl)) {
                int s_ln = 0, e_ln = 0;
                compute_symbol_bounds(&fl, detect_lang(path), (int)sr->line, &s_ln, &e_ln);
                char tag[192];
                const char *kn = (sr->kind < 9) ? KIND_NAMES[sr->kind] : "sym";
                snprintf(tag, sizeof(tag), "%s %s", kn, idx.strs + sr->sym_off);
                print_slice(&fl, path, s_ln, e_ln, tag, max_lines);
                free_file_lines(&fl);
            }
        }
    }

    close_sym_index(&idx);
    return exit_rc;
}
