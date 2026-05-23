#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifdef _WIN32
/* Convert a UTF-8 path to UTF-16 for the wide-API syscalls. fopen /
 * mkdir / stat on Windows interpret their char* argument as CP_ACP
 * (CP1252 on most installs), which mangles UTF-8 byte sequences and
 * breaks pakka's tests where the test-side file creation must match
 * the UTF-16 path pakka.exe (via wmain) actually opens. Caller frees. */
static wchar_t *path_to_wide(const char *utf8)
{
    if (!utf8) {
        return NULL;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static int wide_mkdir(const char *path)
{
    wchar_t *w = path_to_wide(path);
    if (!w) {
        errno = ENOMEM;
        return -1;
    }
    int rc = _wmkdir(w);
    free(w);
    return rc;
}
#define MKDIR(p) wide_mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

char *fs_join(const char *a, const char *b)
{
    size_t alen   = strlen(a);
    int    needs  = alen > 0 && a[alen - 1] != '/';
    size_t blen   = strlen(b);
    size_t total  = alen + (needs ? 1 : 0) + blen + 1;
    char  *result = (char *)malloc(total);
    if (!result) {
        return NULL;
    }
    memcpy(result, a, alen);
    size_t pos = alen;
    if (needs) {
        result[pos++] = '/';
    }
    memcpy(result + pos, b, blen);
    result[pos + blen] = '\0';
    return result;
}

int fs_mkdir_p(const char *path)
{
    char *copy = strdup(path);
    if (!copy) {
        return -1;
    }

    /* Walk the path forward, creating each prefix in turn. Handles both
     * '/' and '\\' separators (Windows). The walk starts past the drive-
     * letter prefix ("C:") and past the leading separator so we never
     * try to mkdir("C:") or mkdir("") as a parent component. */
    char *p = copy;
#ifdef _WIN32
    if (copy[0] && copy[1] == ':') {
        p = copy + 2;
    }
#endif
    if (*p == '/' || *p == '\\') {
        p++;
    }

    for (; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p         = '\0';
            if (MKDIR(copy) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = saved;
        }
    }
    if (MKDIR(copy) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

unsigned char *fs_read_file(const char *path, size_t *out_len)
{
#ifdef _WIN32
    wchar_t *wpath = path_to_wide(path);
    if (!wpath) return NULL;
    FILE *f = _wfopen(wpath, L"rb");
    free(wpath);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    size_t         len = (size_t)pos;
    unsigned char *buf = (unsigned char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

int fs_write_file(const char *path, const void *data, size_t len)
{
#ifdef _WIN32
    wchar_t *wpath = path_to_wide(path);
    if (!wpath) return -1;
    FILE *f = _wfopen(wpath, L"wb");
    free(wpath);
#else
    FILE *f = fopen(path, "wb");
#endif
    if (!f) {
        return -1;
    }
    size_t wrote = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int    err   = (wrote == len) ? 0 : -1;
    if (fclose(f) != 0) {
        err = -1;
    }
    return err;
}

int fs_is_file(const char *path)
{
#ifdef _WIN32
    wchar_t *wpath = path_to_wide(path);
    if (!wpath) return 0;
    struct _stat64 st;
    int            rc = _wstat64(wpath, &st);
    free(wpath);
    if (rc != 0) return 0;
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
#endif
}

int fs_is_dir(const char *path)
{
#ifdef _WIN32
    wchar_t *wpath = path_to_wide(path);
    if (!wpath) return 0;
    struct _stat64 st;
    int            rc = _wstat64(wpath, &st);
    free(wpath);
    if (rc != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
#endif
}

/* ---------- fs_diff_tree ---------- */

typedef struct {
    char **names;
    size_t count;
    size_t cap;
} entry_list_t;

static int entry_list_add(entry_list_t *el, const char *name)
{
    if (el->count + 1 > el->cap) {
        size_t new_cap = el->cap ? el->cap * 2 : 16;
        char **p       = (char **)realloc(el->names, new_cap * sizeof(char *));
        if (!p) {
            return -1;
        }
        el->names = p;
        el->cap   = new_cap;
    }
    el->names[el->count] = strdup(name);
    if (!el->names[el->count]) {
        return -1;
    }
    el->count++;
    return 0;
}

static void entry_list_free(entry_list_t *el)
{
    if (!el->names) {
        return;
    }
    for (size_t i = 0; i < el->count; i++) {
        free(el->names[i]);
    }
    free(el->names);
    el->names = NULL;
    el->count = 0;
    el->cap   = 0;
}

static int compare_str_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int list_dir(const char *path, entry_list_t *out)
{
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (entry_list_add(out, fd.cFileName) < 0) {
            FindClose(h);
            return -1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (entry_list_add(out, e->d_name) < 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
#endif
    qsort(out->names, out->count, sizeof(char *), compare_str_ptr);
    return 0;
}

static int compare_file_contents(const char *a, const char *b)
{
    size_t         alen = 0, blen = 0;
    unsigned char *ad   = fs_read_file(a, &alen);
    unsigned char *bd   = fs_read_file(b, &blen);
    int            rc   = 0;
    if (!ad || !bd) {
        rc = -1;
    } else if (alen != blen || memcmp(ad, bd, alen) != 0) {
        rc = 1;
    }
    free(ad);
    free(bd);
    return rc;
}

int fs_diff_tree(const char *a, const char *b)
{
    entry_list_t la = {0};
    entry_list_t lb = {0};

    if (list_dir(a, &la) != 0) {
        fprintf(stderr, "fs_diff_tree: can't list %s\n", a);
        entry_list_free(&la);
        return -1;
    }
    if (list_dir(b, &lb) != 0) {
        fprintf(stderr, "fs_diff_tree: can't list %s\n", b);
        entry_list_free(&la);
        entry_list_free(&lb);
        return -1;
    }

    int rc = 0;

    if (la.count != lb.count) {
        fprintf(stderr,
                "fs_diff_tree: entry-count mismatch: %s has %zu, %s has %zu\n",
                a, la.count, b, lb.count);
        rc = 1;
        goto out;
    }

    for (size_t i = 0; i < la.count; i++) {
        if (strcmp(la.names[i], lb.names[i]) != 0) {
            fprintf(stderr,
                    "fs_diff_tree: name mismatch at index %zu: %s vs %s\n",
                    i, la.names[i], lb.names[i]);
            rc = 1;
            goto out;
        }
        char *pa = fs_join(a, la.names[i]);
        char *pb = fs_join(b, lb.names[i]);
        if (!pa || !pb) {
            free(pa);
            free(pb);
            rc = -1;
            goto out;
        }

        int pa_is_dir = fs_is_dir(pa);
        int pb_is_dir = fs_is_dir(pb);
        if (pa_is_dir && pb_is_dir) {
            rc = fs_diff_tree(pa, pb);
        } else if (fs_is_file(pa) && fs_is_file(pb)) {
            rc = compare_file_contents(pa, pb);
            if (rc != 0) {
                fprintf(stderr, "fs_diff_tree: content differs: %s vs %s\n", pa, pb);
            }
        } else {
            fprintf(stderr,
                    "fs_diff_tree: file/dir type mismatch: %s vs %s\n", pa, pb);
            rc = 1;
        }
        free(pa);
        free(pb);
        if (rc != 0) {
            goto out;
        }
    }

out:
    entry_list_free(&la);
    entry_list_free(&lb);
    return rc;
}

int fs_rmtree(const char *path)
{
    if (fs_is_file(path)) {
        return remove(path);
    }
    if (!fs_is_dir(path)) {
        return 0; /* nothing there — treat as success */
    }

    entry_list_t entries = {0};
    if (list_dir(path, &entries) != 0) {
        return -1;
    }

    int rc = 0;
    for (size_t i = 0; i < entries.count; i++) {
        char *child = fs_join(path, entries.names[i]);
        if (!child) {
            rc = -1;
            break;
        }
        rc = fs_rmtree(child);
        free(child);
        if (rc != 0) {
            break;
        }
    }
    entry_list_free(&entries);
    if (rc != 0) {
        return rc;
    }

#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}
