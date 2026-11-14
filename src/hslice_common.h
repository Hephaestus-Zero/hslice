#ifndef HSLICE_COMMON_H
#define HSLICE_COMMON_H

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HSLICE_VERSION "0.2.0"
#define HSLICE_MAX_ROOTS 16

static inline const char *ci_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!needle[0]) return haystack;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (tolower((unsigned char)*p) == tolower((unsigned char)needle[0])) {
            size_t i = 1;
            while (i < nlen && p[i] &&
                   tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
                i++;
            }
            if (i == nlen) return p;
        }
    }
    return NULL;
}

static inline bool hslice_resolve_dir(const char *in, char *out, size_t cap) {
    if (!in || !in[0] || cap < 2) return false;
    char tmp[PATH_MAX];
    const char *src = realpath(in, tmp) ? tmp : in;
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    int n = snprintf(out, cap, "%s", src);
    return (n > 0 && (size_t)n < cap);
}

static inline int hslice_add_unique_root(char roots[][PATH_MAX], int count, int max_roots, const char *dir) {
    if (count >= max_roots) return count;
    char resolved[PATH_MAX];
    if (!hslice_resolve_dir(dir, resolved, sizeof(resolved))) return count;
    for (int i = 0; i < count; i++) {
        if (strcmp(roots[i], resolved) == 0) return count;
    }
    int n = snprintf(roots[count], PATH_MAX, "%s", resolved);
    if (n > 0 && n < PATH_MAX) count++;
    return count;
}

static inline int hslice_parse_root_spec(const char *spec, char roots[][PATH_MAX], int max_roots) {
    int count = 0;
    const char *p = spec;
    while (p && *p && count < max_roots) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < PATH_MAX) {
            char part[PATH_MAX];
            memcpy(part, p, len);
            part[len] = '\0';
            count = hslice_add_unique_root(roots, count, max_roots, part);
        }
        if (!colon) break;
        p = colon + 1;
    }
    return count;
}

/*
 * Workspace root resolution priority:
 *   1. Explicit `--root <dir1[:dir2...]>` CLI flag
 *   2. `HSLICE_ROOTS` environment variable (`dir1[:dir2...]`)
 *   3. Nearest parent directory containing `.git`, or canonical `getcwd()`
 */
static inline int hslice_discover_roots(const char *cli_root, char roots[][PATH_MAX], int max_roots) {
    if (max_roots <= 0) return 0;

    if (cli_root && cli_root[0]) {
        int n = hslice_parse_root_spec(cli_root, roots, max_roots);
        if (n > 0) return n;
    }

    const char *env_roots = getenv("HSLICE_ROOTS");
    if (env_roots && env_roots[0]) {
        int n = hslice_parse_root_spec(env_roots, roots, max_roots);
        if (n > 0) return n;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return hslice_add_unique_root(roots, 0, max_roots, "/tmp");
    }

    char probe[PATH_MAX];
    if (snprintf(probe, sizeof(probe), "%s", cwd) < (int)sizeof(probe)) {
        char cur[PATH_MAX];
        int cn = snprintf(cur, sizeof(cur), "%s", probe);
        if (cn > 0 && (size_t)cn < sizeof(cur)) {
            for (;;) {
                char git_marker[PATH_MAX + 8];
                int gn = snprintf(git_marker, sizeof(git_marker), "%s/.git", cur);
                if (gn > 0 && (size_t)gn < sizeof(git_marker) && access(git_marker, F_OK) == 0) {
                    int n = hslice_add_unique_root(roots, 0, max_roots, cur);
                    if (n > 0) return n;
                    break;
                }
                if (strcmp(cur, "/") == 0) break;
                char *slash = strrchr(cur, '/');
                if (!slash) break;
                if (slash == cur) {
                    cur[1] = '\0';
                    break;
                }
                *slash = '\0';
            }
        }
    }

    return hslice_add_unique_root(roots, 0, max_roots, cwd);
}

static inline void hslice_ensure_dir(const char *path) {
    if (!path || !path[0]) return;
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

static inline bool hslice_index_path(char *out, size_t cap, const char *prefix,
                                     char roots[][PATH_MAX], int n_roots,
                                     char *joined_out, size_t joined_cap) {
    uint64_t hash = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit */
    size_t jpos = 0;
    if (joined_out && joined_cap > 0) joined_out[0] = '\0';

    for (int i = 0; i < n_roots; i++) {
        if (i > 0) {
            hash ^= (uint64_t)':';
            hash *= 0x100000001b3ULL;
            if (joined_out && jpos + 1 < joined_cap) {
                joined_out[jpos++] = ':';
                joined_out[jpos] = '\0';
            }
        }
        for (const char *s = roots[i]; *s; s++) {
            hash ^= (uint64_t)(unsigned char)*s;
            hash *= 0x100000001b3ULL;
            if (joined_out && jpos + 1 < joined_cap) {
                joined_out[jpos++] = *s;
                joined_out[jpos] = '\0';
            }
        }
    }

    char cache_dir[PATH_MAX];
    const char *custom_dir = getenv("HSLICE_CACHE_DIR");
    if (custom_dir && custom_dir[0]) {
        int n = snprintf(cache_dir, sizeof(cache_dir), "%s", custom_dir);
        if (n <= 0 || (size_t)n >= sizeof(cache_dir)) return false;
    } else {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && xdg[0]) {
            int n = snprintf(cache_dir, sizeof(cache_dir), "%s/hslice", xdg);
            if (n <= 0 || (size_t)n >= sizeof(cache_dir)) return false;
        } else {
            int n = snprintf(cache_dir, sizeof(cache_dir), "/tmp/hslice-%u", (unsigned)getuid());
            if (n <= 0 || (size_t)n >= sizeof(cache_dir)) return false;
        }
    }

    hslice_ensure_dir(cache_dir);
    int n = snprintf(out, cap, "%s/%s_%016" PRIx64 ".idx", cache_dir, prefix, hash);
    return (n > 0 && (size_t)n < cap);
}

#endif /* HSLICE_COMMON_H */
