/**
 * fileio.c — Cross-platform file I/O layer with automatic directory creation,
 * path resolution against platform_base_path, and case-insensitive fallback.
 */

#include "fileio.h"
#include "platform.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sr_mkdir(p) _mkdir(p)
#define sr_strcasecmp _stricmp
#else
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#define sr_mkdir(p) mkdir(p, 0755)
#define sr_strcasecmp strcasecmp
#endif

/**
 * Recursively creates parent directories for a path (like `mkdir -p`).
 */
static int sr_mkdir_p(const char *dirpath)
{
    if (!dirpath || !*dirpath) {
        return 0;
    }

    char tmp[1024];
    size_t len = strlen(dirpath);
    if (len >= sizeof(tmp)) {
        return -1;
    }
    strncpy(tmp, dirpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Remove trailing slash */
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            sr_mkdir(tmp);
            *p = c;
        }
    }
    return sr_mkdir(tmp);
}

/**
 * Searches for a file on a case-sensitive filesystem by matching each path segment
 * case-insensitively using opendir/readdir.
 */
static int sr_find_file_case_insensitive(const char *target_path, char *out_path, size_t out_len)
{
#ifdef _WIN32
    /* Windows NTFS is case-insensitive by default */
    strncpy(out_path, target_path, out_len - 1);
    out_path[out_len - 1] = '\0';
    return 1;
#else
    if (!target_path || !out_path || out_len == 0) {
        return 0;
    }

    /* Copy path and normalize backslashes */
    char path_copy[1024];
    size_t pl = strlen(target_path);
    if (pl >= sizeof(path_copy)) {
        return 0;
    }
    for (size_t i = 0; i <= pl; i++) {
        path_copy[i] = (target_path[i] == '\\') ? '/' : target_path[i];
    }

    char resolved[1024] = "";
    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/", &saveptr);

    if (target_path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "/");
    } else {
        snprintf(resolved, sizeof(resolved), ".");
    }

    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        if (strcmp(token, "..") == 0) {
            strncat(resolved, "/..", sizeof(resolved) - strlen(resolved) - 1);
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        DIR *d = opendir(resolved);
        if (!d) {
            return 0;
        }

        struct dirent *entry;
        int found = 0;
        char matched_name[256] = "";

        while ((entry = readdir(d)) != NULL) {
            if (sr_strcasecmp(entry->d_name, token) == 0) {
                strncpy(matched_name, entry->d_name, sizeof(matched_name) - 1);
                matched_name[sizeof(matched_name) - 1] = '\0';
                found = 1;
                break;
            }
        }
        closedir(d);

        if (!found) {
            return 0;
        }

        if (strcmp(resolved, "/") == 0) {
            snprintf(resolved, sizeof(resolved), "/%s", matched_name);
        } else if (strcmp(resolved, ".") == 0) {
            snprintf(resolved, sizeof(resolved), "%s", matched_name);
        } else {
            size_t cur_l = strlen(resolved);
            snprintf(resolved + cur_l, sizeof(resolved) - cur_l, "/%s", matched_name);
        }

        token = strtok_r(NULL, "/", &saveptr);
    }

    strncpy(out_path, resolved, out_len - 1);
    out_path[out_len - 1] = '\0';
    return 1;
#endif
}

/**
 * sr_fOpen — Robust cross-platform fopen wrapper.
 * Resolves relative paths against platform_base_path(), ensures parent directories exist
 * on write, and attempts case-insensitive lookup on read.
 */
FILE *sr_fOpen(const char *path, const char *mode)
{
    if (!path || !*path || !mode) {
        return NULL;
    }

    /* Normalize backslashes to forward slashes */
    char norm_path[1024];
    size_t path_len = strlen(path);
    if (path_len >= sizeof(norm_path)) {
        return NULL;
    }
    for (size_t i = 0; i <= path_len; i++) {
        norm_path[i] = (path[i] == '\\') ? '/' : path[i];
    }

    /* Strip leading "./" */
    const char *rel = norm_path;
    while (rel[0] == '.' && rel[1] == '/') {
        rel += 2;
    }

    /* Check if already an absolute path */
    int is_abs = (rel[0] == '/') || (rel[0] != '\0' && rel[1] == ':');

    char full_path[1024];
    const char *base = platform_base_path();

    if (!is_abs && base && *base) {
        snprintf(full_path, sizeof(full_path), "%s/%s", base, rel);
    } else {
        strncpy(full_path, rel, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }

    int is_write = (strchr(mode, 'w') != NULL) ||
                   (strchr(mode, 'a') != NULL) ||
                   (strchr(mode, '+') != NULL);

    if (is_write) {
        /* Ensure parent directory exists */
        char parent[1024];
        strncpy(parent, full_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash) {
            *last_slash = '\0';
            sr_mkdir_p(parent);
        }

        FILE *fp = fopen(full_path, mode);
        if (fp) {
            SDL_Log("sr_fOpen: write opened '%s' (mode '%s')", full_path, mode);
        } else {
            SDL_Log("sr_fOpen: write FAILED '%s' (mode '%s', errno=%d: %s)",
                    full_path, mode, errno, strerror(errno));
        }
        return fp;
    }

    /* Read mode: attempt exact path first */
    FILE *fp = fopen(full_path, mode);
    if (fp) {
        return fp;
    }

    /* Case-insensitive fallback if file not found */
    if (errno == ENOENT) {
        char case_path[1024];
        if (sr_find_file_case_insensitive(full_path, case_path, sizeof(case_path))) {
            fp = fopen(case_path, mode);
            if (fp) {
                SDL_Log("sr_fOpen: case-fallback resolved '%s' -> '%s'", full_path, case_path);
                return fp;
            }
        }

        /* Fallback to raw relative path in case current working directory has it */
        if (strcmp(full_path, rel) != 0) {
            fp = fopen(rel, mode);
            if (fp) {
                return fp;
            }
            if (sr_find_file_case_insensitive(rel, case_path, sizeof(case_path))) {
                fp = fopen(case_path, mode);
                if (fp) {
                    SDL_Log("sr_fOpen: relative case-fallback resolved '%s' -> '%s'", rel, case_path);
                    return fp;
                }
            }
        }
    }

    return NULL;
}
