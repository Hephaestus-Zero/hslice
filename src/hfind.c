/*
 * hfind.c — Sub-Millisecond Zero-Copy mmap Inverted 3-Gram Path & Symbol Engine
 *           (Serves both `hfind` path lookup <0.8ms and `hsearch` symbol lookup <1.5ms)
 *
 * Design:
 *   - Single O(N) Two-Pass Counting-Sort 262,144-Bucket 3-Gram Engine shared by:
 *       1. `hfind`   — workspace file/directory paths
 *       2. `hsearch` — structural code symbols (`file:line`)
 *   - Indices are scoped per workspace root hash under $HSLICE_CACHE_DIR,
 *     $XDG_RUNTIME_DIR/hslice, or /tmp/hslice-<uid>.
 *   - `hsearch -g <regex> [dir]` delegates via `execvp` to bounded `rg`
 *     (or `grep -rnE` fallback when ripgrep is not installed).
 *
 * Build: cc -std=c11 -O3 -Wall -Wextra -Werror -o hfind src/hfind.c
 *        ln -sf hfind hsearch
 */
#include "hslice_common.h"
#include <dirent.h>
#include <sys/mman.h>
#include <time.h>

#define HFIND_MAGIC 0x48464E44U /* "HFND" */
#define HSYM_MAGIC  0x4853594DU /* "HSYM" */
#define IDX_VERSION 1U
#define ALPHA_BITS  6U
#define NUM_TRIGRAMS (1U << (ALPHA_BITS * 3U)) /* 262,144 direct-mapped buckets */
#define MAX_QUERY_TRIS 128
#define MAX_TOKENS 16

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
    uint32_t str_off;
    uint16_t path_len;
    uint16_t base_off;
} FileRec;

typedef enum {
    SK_FN = 0,
    SK_CLASS,
    SK_STRUCT,
    SK_TYPE,
    SK_IFACE,
    SK_ENUM,
    SK_TRAIT,
    SK_CONST,
    SK_MACRO
} SymKind;

static const char *KIND_NAMES[] = {
    "fn", "class", "struct", "type", "iface", "enum", "trait", "const", "macro"
};

typedef struct {
    uint32_t path_off;
    uint32_t sym_off;
    uint32_t sig_off;
    uint32_t line;
    uint16_t base_off;
    uint8_t  sym_len;
    uint8_t  kind;
} SymRec;

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

static bool is_excluded_dir(const char *name) {
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

static bool is_virtualenv_dir(const char *dir_path) {
    char cfg[4096];
    int n = snprintf(cfg, sizeof(cfg), "%s/pyvenv.cfg", dir_path);
    return (n > 0 && (size_t)n < sizeof(cfg) && access(cfg, F_OK) == 0);
}

/* Detect flat `pip install --target` directories: index own top-level files, skip wheel subdirs */
static bool dir_has_dist_info(const char *dir_path) {
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

static bool is_bulk_shard_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 ||
           strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".webp") == 0 ||
           strcmp(dot, ".npy") == 0 || strcmp(dot, ".npz") == 0 ||
           strcmp(dot, ".pt") == 0 || strcmp(dot, ".safetensors") == 0 ||
           strcmp(dot, ".idx") == 0 || strcmp(dot, ".pack") == 0;
}

/* ==================== PATH INDEXER (`hfind`) ==================== */

typedef struct {
    FileRec *files;
    uint32_t files_len, files_cap;
    char *strs;
    uint32_t strs_len, strs_cap;
    uint32_t *tri_counts;
    uint32_t *seen_stamp;
} PathBuilder;

static uint32_t append_str(char **buf, uint32_t *len, uint32_t *cap, const char *s, size_t slen) {
    if (*len + slen + 1 > *cap) {
        while (*len + slen + 1 > *cap)
            *cap = *cap ? (*cap * 2U) : (4U * 1024U * 1024U);
        *buf = realloc(*buf, *cap);
    }
    uint32_t off = *len;
    memcpy(*buf + off, s, slen);
    (*buf)[off + slen] = '\0';
    *len += (uint32_t)(slen + 1);
    return off;
}

static void count_tris_for_str(uint32_t *tri_counts, uint32_t *seen_stamp,
                               uint32_t stamp, const char *s, size_t slen) {
    if (slen < 3) return;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i + 2 < slen; i++) {
        uint32_t tri = pack_tri(u[i], u[i + 1], u[i + 2]);
        if (seen_stamp[tri] != stamp) {
            seen_stamp[tri] = stamp;
            tri_counts[tri]++;
        }
    }
}

static void scatter_tris_for_str(uint32_t *postings, uint32_t *write_cur, uint32_t *seen_stamp,
                                 uint32_t stamp, uint32_t rec_id, const char *s, size_t slen) {
    if (slen < 3) return;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i + 2 < slen; i++) {
        uint32_t tri = pack_tri(u[i], u[i + 1], u[i + 2]);
        if (seen_stamp[tri] != stamp) {
            seen_stamp[tri] = stamp;
            postings[write_cur[tri]++] = rec_id;
        }
    }
}

static void path_builder_add(PathBuilder *b, const char *path) {
    size_t plen = strlen(path);
    if (plen == 0 || plen > 65000) return;

    if (b->files_len == b->files_cap) {
        b->files_cap = b->files_cap ? b->files_cap * 2 : 65536;
        b->files = realloc(b->files, (size_t)b->files_cap * sizeof(FileRec));
    }
    uint32_t fid = b->files_len++;
    const char *slash = strrchr(path, '/');
    uint16_t base_off = slash ? (uint16_t)((slash - path) + 1) : 0;

    b->files[fid].str_off = append_str(&b->strs, &b->strs_len, &b->strs_cap, path, plen);
    b->files[fid].path_len = (uint16_t)plen;
    b->files[fid].base_off = base_off;

    count_tris_for_str(b->tri_counts, b->seen_stamp, fid + 1, path, plen);
}

static void walk_paths(PathBuilder *b, const char *dir, int depth) {
    if (depth > 18) return;
    bool flat_target = dir_has_dist_info(dir);
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    char child[PATH_MAX];
    uint32_t shard_files = 0, reg_files = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        if (is_excluded_dir(de->d_name)) continue;

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
            if (flat_target || is_virtualenv_dir(child)) continue;
            path_builder_add(b, child);
            walk_paths(b, child, depth + 1);
        } else if (dt == DT_REG) {
            if (is_bulk_shard_ext(de->d_name) && ++shard_files > 64) continue;
            if (++reg_files > 16384) continue;
            path_builder_add(b, child);
        }
    }
    closedir(d);
}

static int build_path_index(const char *idx_path, char roots[][PATH_MAX], int n_roots, bool verbose) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    PathBuilder b = {0};
    b.tri_counts = calloc(NUM_TRIGRAMS, sizeof(uint32_t));
    b.seen_stamp = calloc(NUM_TRIGRAMS, sizeof(uint32_t));
    if (!b.tri_counts || !b.seen_stamp) return -1;

    for (int i = 0; i < n_roots; i++) {
        struct stat st;
        if (stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode)) walk_paths(&b, roots[i], 0);
    }

    TriBucket *buckets = calloc(NUM_TRIGRAMS, sizeof(TriBucket));
    uint32_t *write_cur = malloc(NUM_TRIGRAMS * sizeof(uint32_t));
    uint64_t total_postings = 0;
    for (uint32_t i = 0; i < NUM_TRIGRAMS; i++) {
        buckets[i].offset = (uint32_t)total_postings;
        buckets[i].len = b.tri_counts[i];
        write_cur[i] = (uint32_t)total_postings;
        total_postings += b.tri_counts[i];
    }

    uint32_t *postings = malloc((total_postings ? total_postings : 1) * sizeof(uint32_t));
    memset(b.seen_stamp, 0, NUM_TRIGRAMS * sizeof(uint32_t));

    for (uint32_t fid = 0; fid < b.files_len; fid++) {
        scatter_tris_for_str(postings, write_cur, b.seen_stamp, fid + 1, fid,
                             b.strs + b.files[fid].str_off, b.files[fid].path_len);
    }

    char tmp_path[PATH_MAX + 64];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", idx_path, (int)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) return -1;
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;

    IndexHeader hdr = {
        .magic = HFIND_MAGIC,
        .version = IDX_VERSION,
        .num_recs = b.files_len,
        .str_bytes = b.strs_len,
        .postings_len = (uint32_t)total_postings,
        .pad = 0,
        .built_epoch = (uint64_t)time(NULL)
    };

    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); unlink(tmp_path); return -1; }
    bool werr = (fwrite(&hdr, sizeof(hdr), 1, out) != 1) ||
                (fwrite(buckets, sizeof(TriBucket), NUM_TRIGRAMS, out) != NUM_TRIGRAMS) ||
                (b.files_len > 0 && fwrite(b.files, sizeof(FileRec), b.files_len, out) != b.files_len) ||
                (total_postings > 0 && fwrite(postings, sizeof(uint32_t), total_postings, out) != total_postings) ||
                (b.strs_len > 0 && fwrite(b.strs, 1, b.strs_len, out) != b.strs_len) ||
                (fflush(out) != 0);
    if (fclose(out) != 0) werr = true;
    if (werr || rename(tmp_path, idx_path) != 0) {
        unlink(tmp_path);
        free(postings);
        free(write_cur);
        free(buckets);
        free(b.seen_stamp);
        free(b.tri_counts);
        free(b.files);
        free(b.strs);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (verbose) {
        size_t sz = sizeof(hdr) + NUM_TRIGRAMS * sizeof(TriBucket) +
                    b.files_len * sizeof(FileRec) + total_postings * 4 + b.strs_len;
        printf("hfind: indexed %u paths (%" PRIu64 " trigram postings, %.2f MiB mmap) in %.1f ms -> %s\n",
               b.files_len, total_postings, (double)sz / 1048576.0, ms, idx_path);
    }

    free(postings);
    free(write_cur);
    free(buckets);
    free(b.seen_stamp);
    free(b.tri_counts);
    free(b.files);
    free(b.strs);
    return 0;
}

/* ==================== STRUCTURAL SYMBOL INDEXER (`hsearch`) ==================== */

typedef enum {
    LANG_NONE = 0,
    LANG_PY,
    LANG_C,
    LANG_RS,
    LANG_TS,
    LANG_GO
} SrcLang;

static SrcLang detect_lang(const char *name) {
    if (strstr(name, ".min.") || strstr(name, ".bundle.")) return LANG_NONE;
    const char *dot = strrchr(name, '.');
    if (!dot) return LANG_NONE;
    if (strcmp(dot, ".py") == 0 || strcmp(dot, ".pyi") == 0) return LANG_PY;
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
        strcmp(dot, ".cc") == 0 || strcmp(dot, ".cpp") == 0 ||
        strcmp(dot, ".hpp") == 0 || strcmp(dot, ".cxx") == 0 ||
        strcmp(dot, ".hxx") == 0)
        return LANG_C;
    if (strcmp(dot, ".rs") == 0) return LANG_RS;
    if (strcmp(dot, ".ts") == 0 || strcmp(dot, ".tsx") == 0 ||
        strcmp(dot, ".js") == 0 || strcmp(dot, ".jsx") == 0 ||
        strcmp(dot, ".mjs") == 0 || strcmp(dot, ".cjs") == 0)
        return LANG_TS;
    if (strcmp(dot, ".go") == 0) return LANG_GO;
    return LANG_NONE;
}

static inline bool is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool is_ident_char(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static size_t read_ident(const char *p, char *out, size_t cap) {
    if (!is_ident_start((unsigned char)p[0])) return 0;
    size_t i = 0;
    while (is_ident_char((unsigned char)p[i])) {
        if (i + 1 < cap) out[i] = p[i];
        i++;
    }
    size_t w = (i < cap) ? i : (cap - 1);
    out[w] = '\0';
    return i;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool match_kw(const char **pp, const char *kw) {
    size_t kl = strlen(kw);
    if (strncmp(*pp, kw, kl) == 0 && !is_ident_char((unsigned char)(*pp)[kl])) {
        *pp = skip_ws(*pp + kl);
        return true;
    }
    return false;
}

static bool is_c_control_kw(const char *id) {
    static const char *const kws[] = {
        "if", "for", "while", "switch", "return", "sizeof", "typeof",
        "alignof", "__attribute__", "__attribute", "defined", "else", "do", NULL
    };
    for (int i = 0; kws[i]; i++) {
        if (strcmp(id, kws[i]) == 0) return true;
    }
    return false;
}

static bool extract_symbol_on_line(SrcLang lang, const char *raw_line,
                                   char *sym_out, size_t sym_cap, SymKind *kind_out) {
    int indent = 0;
    const char *p = raw_line;
    while (*p == ' ' || *p == '\t') {
        indent += (*p == '\t') ? 4 : 1;
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r') return false;

    if (lang == LANG_PY) {
        if (indent > 8 || *p == '#') return false;
        if (match_kw(&p, "async")) {
            if (match_kw(&p, "def")) {
                *kind_out = SK_FN;
                return read_ident(p, sym_out, sym_cap) >= 2;
            }
            return false;
        }
        if (match_kw(&p, "def")) {
            *kind_out = SK_FN;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        if (match_kw(&p, "class")) {
            *kind_out = SK_CLASS;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        return false;
    }

    if (lang == LANG_RS) {
        if (indent > 8 || strncmp(p, "//", 2) == 0) return false;
        if (strncmp(p, "pub", 3) == 0 && (p[3] == ' ' || p[3] == '(')) {
            p += 3;
            if (*p == '(') {
                const char *rp = strchr(p, ')');
                if (rp) p = rp + 1;
            }
            p = skip_ws(p);
        }
        (void)match_kw(&p, "async");
        (void)match_kw(&p, "unsafe");
        (void)match_kw(&p, "const");
        if (match_kw(&p, "fn")) { *kind_out = SK_FN; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "struct")) { *kind_out = SK_STRUCT; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "enum")) { *kind_out = SK_ENUM; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "trait")) { *kind_out = SK_TRAIT; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "type")) { *kind_out = SK_TYPE; return read_ident(p, sym_out, sym_cap) >= 2; }
        return false;
    }

    if (lang == LANG_TS) {
        if (indent > 4 || strncmp(p, "//", 2) == 0 || *p == '*') return false;
        bool exported = match_kw(&p, "export");
        (void)match_kw(&p, "default");
        (void)match_kw(&p, "declare");
        (void)match_kw(&p, "async");
        if (match_kw(&p, "function")) {
            if (*p == '*') p = skip_ws(p + 1);
            *kind_out = SK_FN;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        if (match_kw(&p, "class") || (match_kw(&p, "abstract") && match_kw(&p, "class"))) {
            *kind_out = SK_CLASS;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        if (match_kw(&p, "interface")) { *kind_out = SK_IFACE; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "type")) { *kind_out = SK_TYPE; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (match_kw(&p, "enum")) { *kind_out = SK_ENUM; return read_ident(p, sym_out, sym_cap) >= 2; }
        if (exported && (match_kw(&p, "const") || match_kw(&p, "let"))) {
            *kind_out = SK_CONST;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        return false;
    }

    if (lang == LANG_GO) {
        if (indent > 0 || strncmp(p, "//", 2) == 0) return false;
        if (match_kw(&p, "func")) {
            if (*p == '(') {
                const char *rp = strchr(p, ')');
                if (!rp) return false;
                p = skip_ws(rp + 1);
            }
            *kind_out = SK_FN;
            return read_ident(p, sym_out, sym_cap) >= 2;
        }
        if (match_kw(&p, "type")) {
            size_t il = read_ident(p, sym_out, sym_cap);
            if (il < 2) return false;
            p = skip_ws(p + il);
            if (strncmp(p, "interface", 9) == 0) *kind_out = SK_IFACE;
            else if (strncmp(p, "struct", 6) == 0) *kind_out = SK_STRUCT;
            else *kind_out = SK_TYPE;
            return true;
        }
        return false;
    }

    if (lang == LANG_C) {
        if (*p == '#' && indent == 0) {
            p = skip_ws(p + 1);
            if (match_kw(&p, "define")) {
                size_t il = read_ident(p, sym_out, sym_cap);
                if (il >= 3 && p[il] == '(') {
                    *kind_out = SK_MACRO;
                    return true;
                }
            }
            return false;
        }
        if (indent > 0 || strncmp(p, "//", 2) == 0 || strncmp(p, "/*", 2) == 0 || *p == '*')
            return false;

        if (match_kw(&p, "struct")) {
            size_t il = read_ident(p, sym_out, sym_cap);
            if (il >= 2 && strchr(p + il, '{') && !strchr(p + il, ';')) {
                *kind_out = SK_STRUCT;
                return true;
            }
            return false;
        }
        if (match_kw(&p, "typedef")) {
            const char *semi = strchr(p, ';');
            if (semi && !strchr(p, '(')) {
                const char *q = semi;
                while (q > p && (q[-1] == ' ' || q[-1] == '\t')) q--;
                const char *id_end = q;
                while (q > p && is_ident_char((unsigned char)q[-1])) q--;
                if (id_end > q && is_ident_start((unsigned char)*q)) {
                    size_t il = (size_t)(id_end - q);
                    if (il >= 2 && il < sym_cap) {
                        memcpy(sym_out, q, il);
                        sym_out[il] = '\0';
                        *kind_out = SK_TYPE;
                        return true;
                    }
                }
            }
            return false;
        }

        /* Top-level C/C++ function definition: `<ret_type> <name>(...` without `;` */
        const char *lp = strchr(p, '(');
        if (!lp || lp == p || strchr(p, ';') || strchr(p, '=')) return false;
        const char *q = lp;
        while (q > p && (q[-1] == ' ' || q[-1] == '\t')) q--;
        const char *id_end = q;
        while (q > p && is_ident_char((unsigned char)q[-1])) q--;
        if (q == p || id_end <= q || !is_ident_start((unsigned char)*q)) return false;
        size_t il = (size_t)(id_end - q);
        if (il < 2 || il >= sym_cap) return false;
        memcpy(sym_out, q, il);
        sym_out[il] = '\0';
        if (is_c_control_kw(sym_out)) return false;
        *kind_out = SK_FN;
        return true;
    }

    return false;
}

typedef struct {
    SymRec *recs;
    uint32_t recs_len, recs_cap;
    char *strs;
    uint32_t strs_len, strs_cap;
    uint32_t *tri_counts;
    uint32_t *seen_stamp;
    char *fbuf;
    size_t fbuf_cap;
} SymBuilder;

static void scan_source_file(SymBuilder *sb, const char *path, SrcLang lang) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1048576) {
        close(fd);
        return;
    }
    size_t sz = (size_t)st.st_size;
    if (sz + 2 > sb->fbuf_cap) {
        sb->fbuf_cap = (sz + 4096) * 2;
        sb->fbuf = realloc(sb->fbuf, sb->fbuf_cap);
    }
    ssize_t rd = read(fd, sb->fbuf, sz);
    close(fd);
    if (rd <= 0) return;
    sb->fbuf[rd] = '\0';

    bool path_stored = false;
    uint32_t path_off = 0;
    size_t plen = strlen(path);
    const char *slash = strrchr(path, '/');
    uint16_t base_off = slash ? (uint16_t)((slash - path) + 1) : 0;
    const char *base_str = path + base_off;
    size_t base_len = plen - base_off;

    uint32_t lineno = 0;
    char *cur = sb->fbuf;
    char sym[128], sig[160];
    while (cur && *cur) {
        lineno++;
        char *nl = strchr(cur, '\n');
        if (nl) *nl = '\0';

        SymKind kind = SK_FN;
        if (extract_symbol_on_line(lang, cur, sym, sizeof(sym), &kind)) {
            if (!path_stored) {
                path_off = append_str(&sb->strs, &sb->strs_len, &sb->strs_cap, path, plen);
                path_stored = true;
            }
            const char *trimmed = skip_ws(cur);
            size_t sl = strlen(trimmed);
            while (sl > 0 && (trimmed[sl - 1] == '\r' || trimmed[sl - 1] == ' ' || trimmed[sl - 1] == '\t'))
                sl--;
            if (sl >= sizeof(sig)) sl = sizeof(sig) - 1;
            memcpy(sig, trimmed, sl);
            sig[sl] = '\0';

            if (sb->recs_len == sb->recs_cap) {
                sb->recs_cap = sb->recs_cap ? sb->recs_cap * 2 : 32768;
                sb->recs = realloc(sb->recs, (size_t)sb->recs_cap * sizeof(SymRec));
            }
            uint32_t rid = sb->recs_len++;
            size_t symlen = strlen(sym);
            if (symlen > 250) symlen = 250;

            sb->recs[rid].path_off = path_off;
            sb->recs[rid].sym_off = append_str(&sb->strs, &sb->strs_len, &sb->strs_cap, sym, symlen);
            sb->recs[rid].sig_off = append_str(&sb->strs, &sb->strs_len, &sb->strs_cap, sig, sl);
            sb->recs[rid].line = lineno;
            sb->recs[rid].base_off = base_off;
            sb->recs[rid].sym_len = (uint8_t)symlen;
            sb->recs[rid].kind = (uint8_t)kind;

            uint32_t stamp = rid + 1;
            count_tris_for_str(sb->tri_counts, sb->seen_stamp, stamp, sym, symlen);
            count_tris_for_str(sb->tri_counts, sb->seen_stamp, stamp, base_str, base_len);
        }

        if (!nl) break;
        cur = nl + 1;
    }
}

static void walk_symbols(SymBuilder *sb, const char *dir, int depth) {
    if (depth > 18) return;
    bool flat_target = dir_has_dist_info(dir);
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    char child[PATH_MAX];
    uint32_t src_in_dir = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        if (is_excluded_dir(de->d_name)) continue;

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
            if (flat_target || is_virtualenv_dir(child)) continue;
            walk_symbols(sb, child, depth + 1);
        } else if (dt == DT_REG) {
            SrcLang lang = detect_lang(de->d_name);
            if (lang != LANG_NONE && ++src_in_dir <= 8192) {
                scan_source_file(sb, child, lang);
            }
        }
    }
    closedir(d);
}

static int build_sym_index(const char *idx_path, char roots[][PATH_MAX], int n_roots, bool verbose) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    SymBuilder sb = {0};
    sb.tri_counts = calloc(NUM_TRIGRAMS, sizeof(uint32_t));
    sb.seen_stamp = calloc(NUM_TRIGRAMS, sizeof(uint32_t));
    if (!sb.tri_counts || !sb.seen_stamp) return -1;

    for (int i = 0; i < n_roots; i++) {
        struct stat st;
        if (stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode)) walk_symbols(&sb, roots[i], 0);
    }

    TriBucket *buckets = calloc(NUM_TRIGRAMS, sizeof(TriBucket));
    uint32_t *write_cur = malloc(NUM_TRIGRAMS * sizeof(uint32_t));
    uint64_t total_postings = 0;
    for (uint32_t i = 0; i < NUM_TRIGRAMS; i++) {
        buckets[i].offset = (uint32_t)total_postings;
        buckets[i].len = sb.tri_counts[i];
        write_cur[i] = (uint32_t)total_postings;
        total_postings += sb.tri_counts[i];
    }

    uint32_t *postings = malloc((total_postings ? total_postings : 1) * sizeof(uint32_t));
    memset(sb.seen_stamp, 0, NUM_TRIGRAMS * sizeof(uint32_t));

    for (uint32_t rid = 0; rid < sb.recs_len; rid++) {
        const SymRec *r = &sb.recs[rid];
        const char *sym = sb.strs + r->sym_off;
        const char *base = (sb.strs + r->path_off) + r->base_off;
        uint32_t stamp = rid + 1;
        scatter_tris_for_str(postings, write_cur, sb.seen_stamp, stamp, rid, sym, r->sym_len);
        scatter_tris_for_str(postings, write_cur, sb.seen_stamp, stamp, rid, base, strlen(base));
    }

    char tmp_path[PATH_MAX + 64];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", idx_path, (int)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) return -1;
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;

    IndexHeader hdr = {
        .magic = HSYM_MAGIC,
        .version = IDX_VERSION,
        .num_recs = sb.recs_len,
        .str_bytes = sb.strs_len,
        .postings_len = (uint32_t)total_postings,
        .pad = 0,
        .built_epoch = (uint64_t)time(NULL)
    };

    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); unlink(tmp_path); return -1; }
    bool werr = (fwrite(&hdr, sizeof(hdr), 1, out) != 1) ||
                (fwrite(buckets, sizeof(TriBucket), NUM_TRIGRAMS, out) != NUM_TRIGRAMS) ||
                (sb.recs_len > 0 && fwrite(sb.recs, sizeof(SymRec), sb.recs_len, out) != sb.recs_len) ||
                (total_postings > 0 && fwrite(postings, sizeof(uint32_t), total_postings, out) != total_postings) ||
                (sb.strs_len > 0 && fwrite(sb.strs, 1, sb.strs_len, out) != sb.strs_len) ||
                (fflush(out) != 0);
    if (fclose(out) != 0) werr = true;
    if (werr || rename(tmp_path, idx_path) != 0) {
        unlink(tmp_path);
        free(postings);
        free(write_cur);
        free(buckets);
        free(sb.seen_stamp);
        free(sb.tri_counts);
        free(sb.recs);
        free(sb.strs);
        free(sb.fbuf);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (verbose) {
        size_t sz = sizeof(hdr) + NUM_TRIGRAMS * sizeof(TriBucket) +
                    sb.recs_len * sizeof(SymRec) + total_postings * 4 + sb.strs_len;
        printf("hsearch: indexed %u symbols (%" PRIu64 " trigram postings, %.2f MiB mmap) in %.1f ms -> %s\n",
               sb.recs_len, total_postings, (double)sz / 1048576.0, ms, idx_path);
    }

    free(postings);
    free(write_cur);
    free(buckets);
    free(sb.seen_stamp);
    free(sb.tri_counts);
    free(sb.recs);
    free(sb.strs);
    free(sb.fbuf);
    return 0;
}

/* ==================== SHARED GALLOPING QUERY ENGINE ==================== */

static inline size_t gallop_seek(const uint32_t *arr, size_t lo, size_t hi, uint32_t target) {
    if (lo >= hi || arr[lo] >= target) return lo;
    size_t step = 1;
    while (lo + step < hi && arr[lo + step] < target) {
        lo += step;
        step <<= 1;
    }
    if (lo + step < hi) hi = lo + step + 1;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

typedef struct {
    uint32_t tri;
    uint32_t len;
    uint32_t off;
} QueryTri;

static int cmp_qtri(const void *a, const void *b) {
    uint32_t la = ((const QueryTri *)a)->len;
    uint32_t lb = ((const QueryTri *)b)->len;
    return (la > lb) - (la < lb);
}

typedef struct {
    uint32_t id;
    int score;
} Hit;

static int cmp_hit_desc(const void *a, const void *b) {
    const Hit *ha = (const Hit *)a, *hb = (const Hit *)b;
    if (hb->score != ha->score) return (hb->score > ha->score) - (hb->score < ha->score);
    return (ha->id > hb->id) - (ha->id < hb->id);
}

static int score_path(const char *path, uint16_t plen, uint16_t base_off,
                      char *const *toks, const size_t *tlens, int n_toks) {
    const char *base = path + base_off;
    int total = 1000 - ((int)plen / 4);
    if (strstr(path, "/tests/") || strstr(path, "test_")) total -= 60;
    for (int i = 0; i < n_toks; i++) {
        const char *m_base = ci_strstr(base, toks[i]);
        if (m_base) {
            total += 450;
            if (m_base == base) {
                total += 350;
                if (strlen(base) == tlens[i]) total += 500;
            } else {
                char prev = m_base[-1];
                if (prev == '_' || prev == '-' || prev == '.') total += 160;
            }
        } else {
            const char *m_path = ci_strstr(path, toks[i]);
            if (!m_path) return -1;
            total += 120;
            if (m_path > path && (m_path[-1] == '/' || m_path[-1] == '_' || m_path[-1] == '-'))
                total += 80;
        }
    }
    return total;
}

static int score_symbol(const char *sym, uint8_t sym_len, const char *path, uint16_t base_off,
                        char *const *toks, const size_t *tlens, int n_toks) {
    const char *base = path + base_off;
    int total = 1200 - (int)sym_len * 3;
    if (strstr(path, "/tests/") || strstr(path, "test_")) total -= 180;
    const char *dot = strrchr(path, '.');
    if (dot && strcmp(dot, ".h") == 0) total -= 60;

    for (int i = 0; i < n_toks; i++) {
        const char *m_sym = ci_strstr(sym, toks[i]);
        if (m_sym) {
            total += 600;
            if (m_sym == sym) {
                total += 500;
                if ((size_t)sym_len == tlens[i]) {
                    total += (strcmp(sym, toks[i]) == 0) ? 1200 : 900;
                }
            } else if (m_sym[-1] == '_') {
                total += 220;
            }
        } else {
            const char *m_base = ci_strstr(base, toks[i]);
            if (m_base) {
                total += 250;
            } else if (ci_strstr(path, toks[i])) {
                total += 90;
            } else {
                return -1;
            }
        }
    }
    return total;
}

static int exec_ripgrep_fallback(char *const *tokens, int n_toks, int limit, const char *default_dir) {
    if (n_toks <= 0) return 1;
    char lim_str[16];
    snprintf(lim_str, sizeof(lim_str), "%d", limit);
    char def_dir[PATH_MAX];
    if (n_toks >= 2) {
        snprintf(def_dir, sizeof(def_dir), "%s", tokens[1]);
    } else if (default_dir && default_dir[0]) {
        snprintf(def_dir, sizeof(def_dir), "%s", default_dir);
    } else if (!getcwd(def_dir, sizeof(def_dir))) {
        snprintf(def_dir, sizeof(def_dir), ".");
    }
    char *rg_argv[] = {
        "rg", "-n", "--no-heading", "--color=never", "-S",
        "-m", lim_str, "--max-columns=200", "--max-columns-preview",
        "-g", "!.git", "-g", "!node_modules", "-g", "!.venv",
        "-g", "!target", "-g", "!build", "-g", "!dist",
        tokens[0], def_dir, NULL
    };
    execvp("rg", rg_argv);

    char *grep_argv[] = {
        "grep", "-rnE",
        "--exclude-dir=.git", "--exclude-dir=node_modules", "--exclude-dir=.venv",
        "--exclude-dir=target", "--exclude-dir=build", "--exclude-dir=dist",
        tokens[0], def_dir, NULL
    };
    execvp("grep", grep_argv);
    perror("grep");
    return 1;
}

int main(int argc, char **argv) {
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    bool sym_mode = (strcmp(prog, "hsearch") == 0);
    bool grep_mode = false;
    bool force_rebuild = false, show_stats = false;
    const char *cli_root = NULL;
    int limit = 20;

    char *tokens[MAX_TOKENS];
    char clean_bufs[MAX_TOKENS][256];
    size_t tlens[MAX_TOKENS];
    int n_toks = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sym") == 0 || strcmp(argv[i], "-s") == 0) {
            sym_mode = true;
        } else if (strcmp(argv[i], "--path") == 0 || strcmp(argv[i], "-p") == 0) {
            sym_mode = false;
        } else if (strcmp(argv[i], "--grep") == 0 || strcmp(argv[i], "-g") == 0) {
            grep_mode = true;
        } else if (strcmp(argv[i], "--active") == 0 || strcmp(argv[i], "-a") == 0) {
            /* Compatibility no-op flag */
        } else if (strcmp(argv[i], "--rebuild") == 0 || strcmp(argv[i], "-r") == 0) {
            force_rebuild = true;
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = true;
        } else if ((strcmp(argv[i], "--root") == 0 || strcmp(argv[i], "-R") == 0) && i + 1 < argc) {
            cli_root = argv[++i];
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--limit") == 0) && i + 1 < argc) {
            limit = atoi(argv[++i]);
            if (limit < 1) limit = 1;
            if (limit > 500) limit = 500;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s v%s\n", prog, HSLICE_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("hfind/hsearch v%s — Sub-millisecond mmap 3-gram path & symbol search\n"
                   "Usage:\n"
                   "  hfind   [--root DIR] [--rebuild] [--stats] [-n LIMIT] <path tokens...>\n"
                   "  hsearch [--root DIR] [--rebuild] [--stats] [-n LIMIT] <symbol tokens...>\n"
                   "  hsearch -g <regex_or_literal> [dir]   (fast bounded ripgrep/grep content search)\n",
                   HSLICE_VERSION);
            return 0;
        } else if (n_toks < MAX_TOKENS) {
            if (grep_mode) {
                tokens[n_toks] = argv[i];
                tlens[n_toks] = strlen(argv[i]);
                n_toks++;
            } else {
                const char *src = argv[i];
                if (src[0] == '.' && src[1] == '/') src += 2;
                size_t j = 0;
                for (size_t k = 0; src[k] && j + 1 < sizeof(clean_bufs[0]); k++) {
                    if (src[k] == '*' || src[k] == '?') continue;
                    if (src[k] == ':' && isdigit((unsigned char)src[k + 1])) break;
                    clean_bufs[n_toks][j++] = src[k];
                }
                clean_bufs[n_toks][j] = '\0';
                if (j > 0) {
                    tokens[n_toks] = clean_bufs[n_toks];
                    tlens[n_toks] = j;
                    n_toks++;
                }
            }
        }
    }

    if (!grep_mode && !force_rebuild && !show_stats && n_toks == 0) {
        fprintf(stderr,
                "Usage:\n"
                "  hfind   [--root DIR] [--rebuild] [--stats] [-n LIMIT] <path tokens...>\n"
                "  hsearch [--root DIR] [--rebuild] [--stats] [-n LIMIT] <symbol tokens...>\n"
                "  hsearch -g <regex_or_literal> [dir]\n");
        return 1;
    }

    char roots[HSLICE_MAX_ROOTS][PATH_MAX];
    int n_roots = hslice_discover_roots(cli_root, roots, HSLICE_MAX_ROOTS);
    if (n_roots <= 0) {
        fprintf(stderr, "%s: failed to resolve workspace root\n", prog);
        return 1;
    }

    if (grep_mode) {
        return exec_ripgrep_fallback(tokens, n_toks, limit, roots[0]);
    }

    char idx_path[PATH_MAX + 128];
    if (!hslice_index_path(idx_path, sizeof(idx_path), sym_mode ? "hsym" : "hfind",
                           roots, n_roots, NULL, 0)) {
        fprintf(stderr, "%s: index cache path overflow\n", prog);
        return 1;
    }

    struct stat st;
    bool need_build = force_rebuild || (stat(idx_path, &st) != 0) ||
                      ((time(NULL) - st.st_mtime) > 3600);
    if (need_build) {
        int rc = sym_mode ? build_sym_index(idx_path, roots, n_roots, force_rebuild || show_stats)
                          : build_path_index(idx_path, roots, n_roots, force_rebuild || show_stats);
        if (rc != 0) {
            fprintf(stderr, "%s: failed to build index at %s\n", prog, idx_path);
            return 1;
        }
        if (force_rebuild && n_toks == 0 && !show_stats) return 0;
        if (stat(idx_path, &st) != 0) return 1;
    }

    int fd = open(idx_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 1;

    const IndexHeader *hdr = (const IndexHeader *)map;
    uint32_t expected_magic = sym_mode ? HSYM_MAGIC : HFIND_MAGIC;
    if (hdr->magic != expected_magic || hdr->version != IDX_VERSION) {
        munmap(map, (size_t)st.st_size);
        return 1;
    }

    const TriBucket *buckets = (const TriBucket *)((const char *)map + sizeof(IndexHeader));
    if (show_stats) {
        printf("%s index: %u %s, %u trigram postings, %.2f MiB mmap (%s)\n",
               sym_mode ? "hsearch" : "hfind",
               hdr->num_recs, sym_mode ? "symbols" : "paths",
               hdr->postings_len, (double)st.st_size / 1048576.0, idx_path);
        if (n_toks == 0) {
            munmap(map, (size_t)st.st_size);
            return 0;
        }
    }
    if (n_toks == 0) {
        munmap(map, (size_t)st.st_size);
        return 0;
    }

    const FileRec *files = NULL;
    const SymRec *syms = NULL;
    const uint32_t *postings = NULL;
    const char *strs = NULL;

    if (sym_mode) {
        syms = (const SymRec *)((const char *)buckets + NUM_TRIGRAMS * sizeof(TriBucket));
        postings = (const uint32_t *)((const char *)syms + hdr->num_recs * sizeof(SymRec));
        strs = (const char *)postings + hdr->postings_len * sizeof(uint32_t);
    } else {
        files = (const FileRec *)((const char *)buckets + NUM_TRIGRAMS * sizeof(TriBucket));
        postings = (const uint32_t *)((const char *)files + hdr->num_recs * sizeof(FileRec));
        strs = (const char *)postings + hdr->postings_len * sizeof(uint32_t);
    }

    QueryTri qtris[MAX_QUERY_TRIS];
    int n_qtris = 0;
    int tri_tok_limit = (sym_mode && n_toks > 1) ? 1 : n_toks;
    for (int t = 0; t < tri_tok_limit; t++) {
        const unsigned char *u = (const unsigned char *)tokens[t];
        if (tlens[t] < 3) continue;
        for (size_t j = 0; j + 2 < tlens[t] && n_qtris < MAX_QUERY_TRIS; j++) {
            uint32_t tri = pack_tri(u[j], u[j + 1], u[j + 2]);
            bool dup = false;
            for (int k = 0; k < n_qtris; k++) {
                if (qtris[k].tri == tri) { dup = true; break; }
            }
            if (!dup) {
                qtris[n_qtris].tri = tri;
                qtris[n_qtris].len = buckets[tri].len;
                qtris[n_qtris].off = buckets[tri].offset;
                n_qtris++;
            }
        }
    }

    Hit *hits = malloc((size_t)(hdr->num_recs ? hdr->num_recs : 1) * sizeof(Hit));
    int n_hits = 0;

    if (n_qtris > 0) {
        qsort(qtris, (size_t)n_qtris, sizeof(QueryTri), cmp_qtri);
        if (qtris[0].len == 0) {
            if (sym_mode)
                fprintf(stderr, "hsearch: 0 symbol defs for \"%s\". NEXT: run `hsearch -g \"%s\"` (body grep) or `hfind %s`.\n",
                        tokens[0], tokens[0], tokens[0]);
            else
                fprintf(stderr, "hfind: 0 paths matched \"%s\". NEXT: try shorter substring tokens or `hfind --rebuild`.\n",
                        tokens[0]);
            free(hits);
            munmap(map, (size_t)st.st_size);
            return 0;
        }
        const uint32_t *base_list = postings + qtris[0].off;
        uint32_t base_len = qtris[0].len;
        size_t cursors[MAX_QUERY_TRIS] = {0};

        for (uint32_t i = 0; i < base_len; i++) {
            uint32_t cand = base_list[i];
            bool all_match = true;
            for (int k = 1; k < n_qtris; k++) {
                const uint32_t *lst = postings + qtris[k].off;
                size_t pos = gallop_seek(lst, cursors[k], qtris[k].len, cand);
                cursors[k] = pos;
                if (pos >= qtris[k].len || lst[pos] != cand) {
                    all_match = false;
                    break;
                }
            }
            if (all_match) {
                int sc = sym_mode
                    ? score_symbol(strs + syms[cand].sym_off, syms[cand].sym_len,
                                   strs + syms[cand].path_off, syms[cand].base_off,
                                   tokens, tlens, n_toks)
                    : score_path(strs + files[cand].str_off, files[cand].path_len,
                                 files[cand].base_off, tokens, tlens, n_toks);
                if (sc >= 0) {
                    hits[n_hits].id = cand;
                    hits[n_hits].score = sc;
                    n_hits++;
                }
            }
        }
    } else {
        for (uint32_t cand = 0; cand < hdr->num_recs; cand++) {
            int sc = sym_mode
                ? score_symbol(strs + syms[cand].sym_off, syms[cand].sym_len,
                               strs + syms[cand].path_off, syms[cand].base_off,
                               tokens, tlens, n_toks)
                : score_path(strs + files[cand].str_off, files[cand].path_len,
                             files[cand].base_off, tokens, tlens, n_toks);
            if (sc >= 0) {
                hits[n_hits].id = cand;
                hits[n_hits].score = sc;
                n_hits++;
            }
        }
    }

    qsort(hits, (size_t)n_hits, sizeof(Hit), cmp_hit_desc);
    if (n_hits == 0 && n_toks > 0) {
        if (sym_mode)
            fprintf(stderr, "hsearch: 0 symbol defs for \"%s\". NEXT: run `hsearch -g \"%s\"` (body grep) or `hslice -x %s`.\n",
                    tokens[0], tokens[0], tokens[0]);
        else
            fprintf(stderr, "hfind: 0 paths matched \"%s\". NEXT: try shorter substring tokens or `hfind --rebuild`.\n",
                    tokens[0]);
    }
    int out_n = (n_hits < limit) ? n_hits : limit;
    for (int i = 0; i < out_n; i++) {
        uint32_t id = hits[i].id;
        if (sym_mode) {
            const SymRec *sr = &syms[id];
            const char *kn = (sr->kind < 9) ? KIND_NAMES[sr->kind] : "sym";
            printf("%s:%u\t[%s]\t%s\n",
                   strs + sr->path_off, sr->line, kn, strs + sr->sig_off);
        } else {
            printf("%s\n", strs + files[id].str_off);
        }
    }

    free(hits);
    munmap(map, (size_t)st.st_size);
    return 0;
}
