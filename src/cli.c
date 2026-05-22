#include <stdarg.h>

#include "common.h"
#include "pakka.h"

#ifdef _WIN32
/* Windows entry point uses wmain(). windows.h is needed for
 * WideCharToMultiByte/CP_UTF8/WC_ERR_INVALID_CHARS at the boundary. */
#include <windows.h>
#endif

static void emit_and_exit(int saved_errno, const char *format, va_list args) {
    char msg[1000];
    vsnprintf(msg, sizeof(msg), format, args);
    if (saved_errno != 0) {
        fprintf(stderr, "%s: %s\n", msg, strerror(saved_errno));
    } else {
        fprintf(stderr, "%s\n", msg);
    }
    exit(1);
}

static void pakka_die(const char *format, ...) {
    int saved_errno = errno;
    va_list args;
    va_start(args, format);
    emit_and_exit(saved_errno, format, args);
    va_end(args);
}

static void pakka_die_e(int saved_errno, const char *format, ...) {
    va_list args;
    va_start(args, format);
    emit_and_exit(saved_errno, format, args);
    va_end(args);
}

static void pakka_fprint_sanitized(FILE *out, const char *s) {
    /* Two stages:
     *   1. Substitute invalid UTF-8 byte runs with '?'. Some legacy
     *      PAK names carry CP1251 / Shift-JIS bytes that the console
     *      can't render; printing them raw produces mojibake or
     *      corrupts the TTY state. Sanitize before printing so the
     *      list output is always printable on a UTF-8 console.
     *   2. Substitute control bytes (< 0x20 or 0x7F) with '?'. This is
     *      the original behavior — kept because UTF-8-valid input can
     *      still contain low-ASCII control bytes. */
    char buf[PAKKA_ENTRY_NAME_SIZE * 4 + 1];
    const char *p;
    unsigned char c;
    (void)pakka_utf8_substitute_invalid(s, buf, sizeof(buf), '?');
    p = buf;
    while ((c = (unsigned char)*p++) != '\0') {
        fputc((c < 0x20 || c == 0x7F) ? '?' : c, out);
    }
}

static char *pakka_sanitize_name(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    unsigned char c;
    if (dstsz == 0) return dst;
    while ((c = (unsigned char)*src++) != '\0' && i + 1 < dstsz) {
        dst[i++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
    dst[i] = '\0';
    return dst;
}

#define PAK_EXTRACT 1
#define PAK_CREATE  2
#define PAK_ADD     4
#define PAK_REMOVE  8
#define PAK_LIST    16
#define PAK_VERIFY  32

#define ADD_FOLDER_MAX_DEPTH 100
#define PAK_TREE_MAX_DEPTH   100

/* UTF-8 box-drawing for --tree output. Octal escapes so the source
 * file's encoding doesn't affect the emitted bytes. */
#define PAK_TREE_ROOT         "."
#define PAK_TREE_BRANCH_MID   "\342\224\234\342\224\200\342\224\200 "
#define PAK_TREE_BRANCH_LAST  "\342\224\224\342\224\200\342\224\200 "
#define PAK_TREE_PREFIX_MID   "\342\224\202   "
#define PAK_TREE_PREFIX_LAST  "    "

typedef struct opts_s {
    short mode;
    char *pakfile;
    char *destination;
    char **paths;
    int path_count;
    int tree;
    int deep;
    /* --compress: PK3/PK4 only — enable DEFLATE on newly-added
     * entries, with -c or -a. Rejected for PAK/SiN/Daikatana
     * (DK compression is automatic by entry extension). Per-entry
     * auto-fallback to STORED when DEFLATE wouldn't win. */
    int compress;
    /* --as <entry_name> <source_path> pairs. Parallel arrays so a
     * caller can pass any number of --as occurrences alongside plain
     * paths. Only valid with PAK_ADD / PAK_CREATE. */
    char **aliased_entries;
    char **aliased_sources;
    int aliased_count;
    /* --format <name>. PAKKA_FORMAT_AUTO means "infer from extension or
     * on-disk magic" (today's behaviour). Other values override both at
     * open time (pakka_open_ex) and at create time (overrides the
     * extension sniffer). */
    pakka_format_t format;
} opts_t;

typedef struct treenode_s {
    char *name;
    int is_dir;
    struct treenode_s *children;
    struct treenode_s *next;
} treenode_t;

static char *g_argv0;

static void usage(void);
static void usage_banner(void);
static void help(void);
static void version(void);
static int parseopts(int argc, char **argv, opts_t *opts);
static int strip_long_options(int argc, char **argv, opts_t *opts);
static void setmodetype(opts_t *opts, short mode);

static void fail_from_err(const pakka_error_t *err);
static int build_filename(char *dest, size_t dest_size,
                          const char *basedir, const char *filename);

static void op_list(pakka_archive_t *pak);
static void op_list_tree(pakka_archive_t *pak);
static void op_extract(pakka_archive_t *pak, char *destination,
                       char **paths, int path_count);
static void op_add(pakka_archive_t *pak, char **paths, int path_count,
                   char **aliased_entries, char **aliased_sources,
                   int aliased_count);
static void op_remove(pakka_archive_t *pak, char **paths, int path_count);
static void op_verify(pakka_archive_t *pak, int deep);
static void cli_add_path(pakka_archive_t *pak, char *path);
static void cli_add_folder_r(pakka_archive_t *pak, char *path, int depth);

static treenode_t *create_tree_node(const char *name, int is_dir);
static void insert_tree_path(treenode_t *root, const char *path,
                             uint32_t *dir_count, uint32_t *file_count);
static treenode_t *find_tree_dir(treenode_t *parent, const char *name);
static void insert_tree_child(treenode_t *parent, treenode_t *node);
static void print_tree_children(treenode_t *node, const char *prefix);
static void print_tree_summary(uint32_t dir_count, uint32_t file_count);
static void free_tree(treenode_t *node);

/* cli_run is the platform-neutral CLI entry. main()/wmain() below
 * own argv conversion at the OS boundary; everything inside cli_run
 * sees UTF-8 narrow strings (UTF-8 on Windows by convention; the
 * native narrow encoding on POSIX, which is UTF-8 in practice). */
static int cli_run(int argc, char **argv) {
    opts_t opts = {0};
    pakka_archive_t *pak;
    pakka_error_t err;
    pakka_status_t s;

    g_argv0 = argv[0];
    parseopts(argc, argv, &opts);

    /* --compress is only meaningful on create and add. For -l / -x / -d
     * / --verify there's no add path to apply compression to, so reject
     * before any file is opened to keep the error message specific. */
    if (opts.compress
        && opts.mode != PAK_CREATE && opts.mode != PAK_ADD) {
        pakka_die("--compress is only valid with -c (create) or -a (add)");
    }
    /* --compress requires a ZIP-class target. The library setter also
     * enforces this at pakka_set_compression time, but failing here
     * gives a friendlier message (CLI flag context) and skips opening
     * the archive. The --format-pinned PAK / SiN cases are caught
     * directly; AUTO is caught by extension-sniff inference on create
     * and by post-open format probe on add. */
    if (opts.compress) {
        if (opts.format == PAKKA_FORMAT_PAK
            || opts.format == PAKKA_FORMAT_SIN
            || opts.format == PAKKA_FORMAT_DAIKATANA
            || opts.format == PAKKA_FORMAT_IWAD
            || opts.format == PAKKA_FORMAT_PWAD) {
            pakka_die("--compress (DEFLATE) is only valid for PK3 / PK4 "
                      "archives, not the requested PAK-class format");
        }
    }

    if (opts.mode == PAK_CREATE) {
        /* Format selection from --format if explicit, else from
         * extension (case-insensitive): .pk3 → PK3 (Quake 3), .pk4 →
         * PK4 (Doom 3), .sin → SiN (Ritual), .wad → PWAD (Doom 1/2
         * patch — IWAD authoring is rare; users explicitly pass
         * --format iwad when they want it), anything else → PAK.
         * PK3/PK4 default to STORED on write; pass --compress to enable
         * DEFLATE (pakka_set_compression is invoked after create below). */
        pakka_format_t fmt = opts.format;
        if (fmt == PAKKA_FORMAT_AUTO) {
            size_t plen = strlen(opts.pakfile);
            fmt = PAKKA_FORMAT_PAK;
            if (plen >= 4) {
                const char *ext = opts.pakfile + plen - 4;
                if ((ext[0] == '.')
                    && (ext[1] == 'p' || ext[1] == 'P')
                    && (ext[2] == 'k' || ext[2] == 'K')) {
                    if (ext[3] == '3') {
                        fmt = PAKKA_FORMAT_PK3;
                    } else if (ext[3] == '4') {
                        fmt = PAKKA_FORMAT_PK4;
                    }
                }
                if (plen >= 4
                    && (opts.pakfile[plen - 4] == '.')
                    && (opts.pakfile[plen - 3] == 's' || opts.pakfile[plen - 3] == 'S')
                    && (opts.pakfile[plen - 2] == 'i' || opts.pakfile[plen - 2] == 'I')
                    && (opts.pakfile[plen - 1] == 'n' || opts.pakfile[plen - 1] == 'N')) {
                    fmt = PAKKA_FORMAT_SIN;
                }
                if (plen >= 4
                    && (opts.pakfile[plen - 4] == '.')
                    && (opts.pakfile[plen - 3] == 'w' || opts.pakfile[plen - 3] == 'W')
                    && (opts.pakfile[plen - 2] == 'a' || opts.pakfile[plen - 2] == 'A')
                    && (opts.pakfile[plen - 1] == 'd' || opts.pakfile[plen - 1] == 'D')) {
                    fmt = PAKKA_FORMAT_PWAD;
                }
            }
        }
        s = pakka_create(opts.pakfile, fmt,
                         PAKKA_CREATE_DEFAULT, &pak, &err);
    } else {
        pakka_open_mode_t mode =
            (opts.mode == PAK_ADD || opts.mode == PAK_REMOVE)
              ? PAKKA_OPEN_READ_WRITE
              : PAKKA_OPEN_READ;
        s = pakka_open_ex(opts.pakfile, mode, opts.format, &pak, &err);
    }
    if (s != PAKKA_OK) {
        fail_from_err(&err);
    }

    /* Apply --compress now that we have an open handle. The format
     * guard above caught PAK-class targets when --format was explicit;
     * this call catches the AUTO-extension case (e.g. a `.pak` file
     * passed without --format). pakka_set_compression rejects DEFLATE
     * on non-ZIP with PAKKA_ERR_INVALID_ARGUMENT — fail_from_err
     * formats the library's message. */
    if (opts.compress) {
        s = pakka_set_compression(pak, PAKKA_COMPRESSION_DEFLATE, &err);
        if (s != PAKKA_OK) {
            fail_from_err(&err);
        }
    }

    switch (opts.mode) {
        case PAK_LIST:
            if (opts.tree) {
                op_list_tree(pak);
            } else {
                op_list(pak);
            }
            break;
        case PAK_EXTRACT:
            op_extract(pak, opts.destination, opts.paths, opts.path_count);
            break;
        case PAK_ADD:
        case PAK_CREATE:
            op_add(pak, opts.paths, opts.path_count,
                   opts.aliased_entries, opts.aliased_sources,
                   opts.aliased_count);
            break;
        case PAK_REMOVE:
            op_remove(pak, opts.paths, opts.path_count);
            break;
        case PAK_VERIFY:
            op_verify(pak, opts.deep);
            break;
        default:
            fprintf(stderr, "Unknown operation mode selected\n");
            exit(1);
    }

    s = pakka_close(pak, &err);
    if (s != PAKKA_OK) {
        fail_from_err(&err);
    }
    free(opts.paths);
    free(opts.aliased_entries);
    free(opts.aliased_sources);

    return 0;
}

#ifdef _WIN32

/* Windows: the C runtime invokes wmain when present, handing us argv
 * as UTF-16 from CommandLineToArgvW. Convert once at the boundary so
 * the rest of pakka sees a single canonical narrow encoding (UTF-8),
 * which is what every pakka_platform_* helper expects on Windows. */
static void wmain_free_argv(char **argv_utf8, int filled) {
    int i;
    if (!argv_utf8) return;
    for (i = 0; i < filled; i++) free(argv_utf8[i]);
    free(argv_utf8);
}

int wmain(int argc, wchar_t **wargv) {
    char **argv_utf8;
    int i;
    int ret;

    /* argc + 1 for the trailing NULL sentinel. argv_utf8 is mutable
     * because strip_long_options rewrites it in place during option
     * parsing (see parseopts). */
    argv_utf8 = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv_utf8) {
        fprintf(stderr, "pakka: out of memory in argv conversion\n");
        return 2;
    }

    for (i = 0; i < argc; i++) {
        int u8_len;
        /* WC_ERR_INVALID_CHARS so malformed surrogates fail loudly
         * instead of being silently replaced with U+FFFD. */
        u8_len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                     wargv[i], -1,
                                     NULL, 0, NULL, NULL);
        if (u8_len <= 0) {
            fprintf(stderr,
                    "pakka: argv[%d] is not a valid UTF-16 string "
                    "(GetLastError=%lu)\n",
                    i, (unsigned long)GetLastError());
            wmain_free_argv(argv_utf8, i);
            return 2;
        }
        argv_utf8[i] = (char *)malloc((size_t)u8_len);
        if (!argv_utf8[i]) {
            fprintf(stderr, "pakka: out of memory in argv conversion\n");
            wmain_free_argv(argv_utf8, i);
            return 2;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                wargv[i], -1,
                                argv_utf8[i], u8_len, NULL, NULL) <= 0) {
            fprintf(stderr,
                    "pakka: argv[%d] UTF-8 conversion failed "
                    "(GetLastError=%lu)\n",
                    i, (unsigned long)GetLastError());
            free(argv_utf8[i]);
            argv_utf8[i] = NULL;
            wmain_free_argv(argv_utf8, i);
            return 2;
        }
    }
    argv_utf8[argc] = NULL;

    ret = cli_run(argc, argv_utf8);
    wmain_free_argv(argv_utf8, argc);
    return ret;
}

#else

int main(int argc, char *argv[]) {
    return cli_run(argc, argv);
}

#endif

static void fail_from_err(const pakka_error_t *err) {
    if (err->domain == PAKKA_ERR_DOMAIN_ERRNO) {
        pakka_die_e((int)err->system_code, "%s", err->message);
    } else if (err->domain == PAKKA_ERR_DOMAIN_WIN32) {
        /* GetLastError DWORDs don't decode through strerror — print the
         * raw code so Windows users have something to feed into a search
         * or HRESULT lookup. */
        pakka_die_e(0, "%s (Win32 error %u)",
                    err->message, (unsigned)err->system_code);
    } else {
        pakka_die_e(0, "%s", err->message);
    }
}

static void op_list(pakka_archive_t *pak) {
    size_t count = pakka_entry_count(pak);
    size_t i;
    const pakka_entry_t *entry;
    pakka_status_t s;

    if (count == 0) {
        printf("Pak is empty\n");
        return;
    }

    for (i = 0; i < count; i++) {
        s = pakka_entry_at(pak, i, &entry);
        if (s != PAKKA_OK) {
            pakka_die_e(0, "Cannot read entry %zu", i);
        }
        pakka_fprint_sanitized(stdout, pakka_entry_name(entry));
        printf(" (%" PRIu64 " bytes)\n", pakka_entry_size(entry));
    }
}

static void op_list_tree(pakka_archive_t *pak) {
    size_t count = pakka_entry_count(pak);
    size_t i;
    const pakka_entry_t *entry;
    pakka_status_t s;
    treenode_t *root = create_tree_node(PAK_TREE_ROOT, 1);
    uint32_t dir_count = 0;
    uint32_t file_count = 0;

    printf("%s\n", PAK_TREE_ROOT);

    for (i = 0; i < count; i++) {
        s = pakka_entry_at(pak, i, &entry);
        if (s != PAKKA_OK) {
            pakka_die_e(0, "Cannot read entry %zu", i);
        }
        insert_tree_path(root, pakka_entry_name(entry),
                         &dir_count, &file_count);
    }
    if (count > 0) {
        print_tree_children(root, "");
    }

    print_tree_summary(dir_count, file_count);
    free_tree(root);
}

static treenode_t *create_tree_node(const char *name, int is_dir) {
    treenode_t *node = calloc(1, sizeof(treenode_t));
    size_t name_len;

    if (node == NULL) {
        pakka_die("Cannot allocate tree node");
    }
    name_len = strlen(name);
    node->name = malloc(name_len + 1);
    if (node->name == NULL) {
        pakka_die("Cannot allocate tree node name");
    }
    memcpy(node->name, name, name_len + 1);
    node->is_dir = is_dir;

    return node;
}

static void insert_tree_path(treenode_t *root, const char *path,
                             uint32_t *dir_count, uint32_t *file_count) {
    char *path_copy;
    char *component;
    char *next;
    treenode_t *parent = root;
    treenode_t *dir;
    int depth = 0;

    if (path[0] == '\0') {
        return;
    }

    path_copy = pakka_platform_strdup(path);
    if (path_copy == NULL) {
        pakka_die("Cannot allocate tree-path copy");
    }
    component = strtok(path_copy, "/");

    while (component != NULL) {
        next = strtok(NULL, "/");

        if (depth >= PAK_TREE_MAX_DEPTH) {
            pakka_die("Pak directory nesting exceeds %d levels at '%s'",
                       PAK_TREE_MAX_DEPTH, path);
        }

        if (next == NULL) {
            insert_tree_child(parent, create_tree_node(component, 0));
            (*file_count)++;
        } else {
            dir = find_tree_dir(parent, component);
            if (dir == NULL) {
                dir = create_tree_node(component, 1);
                insert_tree_child(parent, dir);
                (*dir_count)++;
            }
            parent = dir;
            depth++;
        }

        component = next;
    }

    free(path_copy);
}

static treenode_t *find_tree_dir(treenode_t *parent, const char *name) {
    treenode_t *current = parent->children;
    while (current != NULL) {
        if (current->is_dir && strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static void insert_tree_child(treenode_t *parent, treenode_t *node) {
    treenode_t *current = parent->children;
    treenode_t *previous = NULL;
    while (current != NULL && strcmp(current->name, node->name) <= 0) {
        previous = current;
        current = current->next;
    }
    if (previous == NULL) {
        node->next = parent->children;
        parent->children = node;
    } else {
        node->next = previous->next;
        previous->next = node;
    }
}

static void print_tree_children(treenode_t *node, const char *prefix) {
    treenode_t *current = node->children;
    char *next_prefix;
    const char *branch;
    const char *prefix_suffix;
    size_t prefix_len;

    while (current != NULL) {
        branch        = current->next == NULL ? PAK_TREE_BRANCH_LAST : PAK_TREE_BRANCH_MID;
        prefix_suffix = current->next == NULL ? PAK_TREE_PREFIX_LAST : PAK_TREE_PREFIX_MID;

        printf("%s%s", prefix, branch);
        pakka_fprint_sanitized(stdout, current->name);
        printf("\n");

        if (current->is_dir) {
            prefix_len = strlen(prefix) + strlen(prefix_suffix);
            next_prefix = malloc(prefix_len + 1);
            if (next_prefix == NULL) {
                pakka_die("Cannot allocate tree prefix");
            }
            memcpy(next_prefix, prefix, strlen(prefix));
            memcpy(next_prefix + strlen(prefix), prefix_suffix,
                   strlen(prefix_suffix) + 1);
            print_tree_children(current, next_prefix);
            free(next_prefix);
        }

        current = current->next;
    }
}

static void print_tree_summary(uint32_t dir_count, uint32_t file_count) {
    printf("\n%" PRIu32 " director%s, %" PRIu32 " file%s\n",
           dir_count, dir_count == 1 ? "y" : "ies",
           file_count, file_count == 1 ? "" : "s");
}

static void free_tree(treenode_t *node) {
    treenode_t *current;
    treenode_t *next;

    if (node == NULL) {
        return;
    }
    current = node->children;
    while (current != NULL) {
        next = current->next;
        free_tree(current);
        current = next;
    }
    free(node->name);
    free(node);
}

static int name_ptr_cmp_main(const void *a, const void *b) {
    const char *const *aa = (const char *const *)a;
    const char *const *bb = (const char *const *)b;
    return strcmp(*aa, *bb);
}

static void op_extract(pakka_archive_t *pak, char *destination,
                       char **paths, int path_count) {
    char *realdest;
    struct stat sb;
    size_t count = pakka_entry_count(pak);
    int *should_extract = NULL;
    int *path_matched = NULL;
    char **norms = NULL;
    int i;
    size_t idx;
    const pakka_entry_t *entry;
    pakka_error_t err;
    pakka_status_t s;
    uint32_t selected = 0;
    uint32_t k;

    realdest = malloc(OS_PATH_MAX);
    if (realdest == NULL) {
        pakka_die("Cannot allocate destination buffer");
    }
    fprintf(stderr, "[trace] op_extract: destination=%s\n",
            destination ? destination : "(null)");
    fflush(stderr);
    if (destination != NULL) {
        fprintf(stderr, "[trace] op_extract: calling pakka_platform_realpath\n");
        fflush(stderr);
        if (pakka_platform_realpath(destination, realdest) == NULL) {
            pakka_die("Cannot open destination path '%s'", destination);
        }
        fprintf(stderr, "[trace] op_extract: realpath -> %s\n", realdest);
        fflush(stderr);
    } else {
        fprintf(stderr, "[trace] op_extract: calling pakka_platform_getcwd\n");
        fflush(stderr);
        if (pakka_platform_getcwd(realdest, OS_PATH_MAX) == NULL) {
            pakka_die("Cannot get current working directory");
        }
        fprintf(stderr, "[trace] op_extract: getcwd -> %s\n", realdest);
        fflush(stderr);
    }
    fprintf(stderr, "[trace] op_extract: calling pakka_platform_stat\n");
    fflush(stderr);
    if (pakka_platform_stat(realdest, &sb) != 0) {
        pakka_die("Cannot stat destination '%s'", realdest);
    }
    fprintf(stderr, "[trace] op_extract: stat ok\n");
    fflush(stderr);
    if (!S_ISDIR(sb.st_mode)) {
        pakka_die("Destination '%s' is not a directory", realdest);
    }

    if (count == 0) {
        if (path_count > 0) {
            pakka_die("Pak is empty; cannot extract %s", paths[0]);
        }
        printf("Pak is empty\n");
        free(realdest);
        return;
    }

    should_extract = calloc(count, sizeof(int));
    if (should_extract == NULL) {
        pakka_die("Cannot allocate extract preflight buffer");
    }
    if (path_count > 0) {
        path_matched = calloc(path_count, sizeof(int));
        if (path_matched == NULL) {
            pakka_die("Cannot allocate path-match buffer");
        }
    }

    /* Pass 1a + 1b: path-match + unsafe-name reject. */
    for (idx = 0; idx < count; idx++) {
        int matched;
        const char *name;
        s = pakka_entry_at(pak, idx, &entry);
        if (s != PAKKA_OK) {
            pakka_die_e(0, "Cannot read entry %zu", idx);
        }
        name = pakka_entry_name(entry);

        if (path_count == 0) {
            matched = 1;
        } else {
            int wad = pakka_format_is_wad(pakka_format(pak));
            matched = 0;
            for (i = 0; i < path_count; i++) {
                if (strcmp(name, paths[i]) == 0) {
                    /* Doom IWADs/PWADs deliberately ship duplicate lump
                     * names (every map repeats THINGS, LINEDEFS, SECTORS,
                     * etc.). Extract-by-name selects only the first
                     * match per path argument — extracting all matches
                     * would collide on disk. C API consumers iterate
                     * with pakka_entry_at + pakka_open_entry_handle for
                     * index-based access. */
                    if (wad && path_matched[i]) {
                        continue;
                    }
                    matched = 1;
                    path_matched[i] = 1;
                }
            }
        }

        if (matched) {
            if (pakka_unsafe_entry_name(name)) {
                char safe[PAKFILE_PATH_BUF];
                pakka_sanitize_name(safe, sizeof(safe), name);
                pakka_die_e(0,
                    "Refusing to extract '%s': entry name would escape destination",
                    safe);
            }
            should_extract[idx] = 1;
            selected++;
        }
    }
    if (path_count > 0) {
        for (i = 0; i < path_count; i++) {
            if (!path_matched[i]) {
                pakka_die("Cannot find %s in pak file", paths[i]);
            }
        }
    }

    /* Pass 1c: normalized-collision reject.
     *
     * Apply invalid-UTF-8 substitution BEFORE normalization so two
     * different legacy CP1251 names that both sanitize to "____.txt"
     * are caught here rather than silently clobbering each other at
     * extract time. Without this, the write pass's '_'-substitution
     * would produce two different on-disk operations targeting the
     * same path. */
    if (selected > 1) {
        norms = calloc(selected, sizeof(char *));
        if (norms == NULL) {
            pakka_die("Cannot allocate collision-check buffer");
        }
        k = 0;
        for (idx = 0; idx < count; idx++) {
            if (should_extract[idx]) {
                const char *raw_name;
                char sanitized[PAKKA_ENTRY_NAME_SIZE * 4 + 1];
                const char *cmp_name;
                size_t cmp_len;
                s = pakka_entry_at(pak, idx, &entry);
                if (s != PAKKA_OK) {
                    pakka_die_e(0, "Cannot read entry %zu", idx);
                }
                raw_name = pakka_entry_name(entry);
                if (pakka_utf8_substitute_invalid(raw_name, sanitized,
                                                  sizeof(sanitized), '_')) {
                    cmp_name = sanitized;
                } else {
                    cmp_name = raw_name;
                }
                cmp_len = strlen(cmp_name);
                norms[k] = malloc(cmp_len + 1);
                if (norms[k] == NULL) {
                    pakka_die("Cannot allocate normalized name buffer");
                }
                pakka_normalize_entry_name(cmp_name, norms[k], cmp_len + 1);
                k++;
            }
        }
        qsort(norms, selected, sizeof(char *), name_ptr_cmp_main);
        for (k = 1; k < selected; k++) {
            if (strcmp(norms[k - 1], norms[k]) == 0) {
                char safe[PAKFILE_PATH_BUF];
                pakka_sanitize_name(safe, sizeof(safe), norms[k]);
                pakka_die_e(0,
                    "Refusing to extract: entries collide after "
                    "normalization on '%s'", safe);
            }
        }
        for (k = 0; k < selected; k++) {
            free(norms[k]);
        }
        free(norms);
    }

    /* Pass 2: write. */
    for (idx = 0; idx < count; idx++) {
        pakka_reader_t *reader;
        FILE *tfd;
        unsigned char buf[PAKFILE_COPY_CHUNK];
        size_t nread;
        const char *name;
        char sanitized[PAKKA_ENTRY_NAME_SIZE * 4 + 1];
        uint64_t size;

        if (!should_extract[idx]) continue;

        s = pakka_entry_at(pak, idx, &entry);
        if (s != PAKKA_OK) {
            pakka_die_e(0, "Cannot read entry %zu", idx);
        }
        name = pakka_entry_name(entry);
        size = pakka_entry_size(entry);

        /* Legacy PAK/SiN/WAD archives can contain entry names whose
         * bytes are not valid UTF-8 (e.g. CP1251 names from a Russian
         * PC, Shift-JIS from a Japanese PC). On Windows those bytes
         * can't be handed to CreateFileW. Substitute invalid byte
         * runs with '_' and create the file under the sanitized name
         * — preserves data over silent extraction failure. The PK3
         * read path already converts CP437 to UTF-8 (see
         * pk3_decode_entry_name) so PK3/PK4 entries should never hit
         * this branch. */
        if (pakka_utf8_substitute_invalid(name, sanitized,
                                          sizeof(sanitized), '_')) {
            fprintf(stderr,
                    "[warn] entry %zu: name not valid UTF-8, "
                    "substituting invalid bytes (writing as '%s')\n",
                    idx, sanitized);
            name = sanitized;
        }

        fprintf(stderr, "[trace] extract idx=%zu name=%s\n", idx, name);
        fflush(stderr);
        printf("Writing %" PRIu64 " bytes to %s/%s\n", size, realdest, name);

        fprintf(stderr, "[trace] extract idx=%zu calling open_extract_target\n", idx);
        fflush(stderr);
        tfd = pakka_platform_open_extract_target(realdest, name);
        fprintf(stderr, "[trace] extract idx=%zu open_extract_target returned %p\n",
                idx, (void *)tfd);
        fflush(stderr);
        if (tfd == NULL) {
            pakka_die("Cannot open %s/%s for writing "
                       "(symlink in path or filesystem error)",
                       realdest, name);
        }

        if (size > 0) {
            /* Reopen via handle, not by name: WADs may have duplicate
             * lump names (THINGS, LINEDEFS, ...) and a by-name lookup
             * would re-read the first match for every iteration of this
             * loop. The handle path is also a small perf win on every
             * format — no O(n) name scan per entry. */
            s = pakka_open_entry_handle(pak, entry, &reader, &err);
            if (s != PAKKA_OK) {
                fclose(tfd);
                fail_from_err(&err);
            }
            for (;;) {
                s = pakka_reader_read(reader, buf, sizeof(buf), &nread, &err);
                if (s != PAKKA_OK) {
                    pakka_reader_close(reader);
                    fclose(tfd);
                    fail_from_err(&err);
                }
                if (nread == 0) break;
                if (fwrite(buf, 1, nread, tfd) != nread) {
                    int werr = errno;
                    pakka_reader_close(reader);
                    fclose(tfd);
                    pakka_die_e(werr, "Cannot write entry '%s'", name);
                }
            }
            pakka_reader_close(reader);
        }

        if (fclose(tfd) != 0) {
            /* Buffered disk-full / NFS / quota failures first surface
             * here, not on the final fwrite — leaving them unchecked
             * lets a truncated extraction look successful. */
            int close_errno = errno;
            pakka_die_e(close_errno, "Cannot finalize %s/%s", realdest, name);
        }
    }

    free(should_extract);
    free(path_matched);
    free(realdest);
}

static void op_add(pakka_archive_t *pak, char **paths, int path_count,
                   char **aliased_entries, char **aliased_sources,
                   int aliased_count) {
    pakka_error_t err;
    pakka_status_t s;
    int i;

    for (i = 0; i < path_count; i++) {
        cli_add_path(pak, paths[i]);
    }

    /* --as never recurses or auto-detects directories — pakka_add_file
     * still enforces is_reparse_or_symlink and S_ISREG on the source. */
    for (i = 0; i < aliased_count; i++) {
        printf("Adding %s to pak as %s\n",
               aliased_sources[i], aliased_entries[i]);
        s = pakka_add_file(pak, aliased_sources[i],
                           aliased_entries[i], &err);
        if (s != PAKKA_OK) {
            fail_from_err(&err);
        }
    }

    s = pakka_commit(pak, &err);
    if (s != PAKKA_OK) {
        fail_from_err(&err);
    }
}

static void cli_add_path(pakka_archive_t *pak, char *path) {
    struct stat sb;

    if (pakka_platform_is_reparse_or_symlink(path)) {
        pakka_die_e(0, "Refusing to add symlink/reparse '%s'", path);
    }
    if (pakka_platform_lstat(path, &sb) != 0) {
        pakka_die_e(errno, "Cannot stat %s", path);
    }
    if (S_ISDIR(sb.st_mode)) {
        cli_add_folder_r(pak, path, 0);
    } else if (S_ISREG(sb.st_mode)) {
        pakka_error_t err;
        pakka_status_t s;
        printf("Adding %s to pak\n", path);
        s = pakka_add_file(pak, path, path, &err);
        if (s != PAKKA_OK) {
            fail_from_err(&err);
        }
    } else {
        pakka_die_e(0, "Refusing to add non-regular file '%s'", path);
    }
}

static void cli_add_folder_r(pakka_archive_t *pak, char *path, int depth) {
    pakka_dir_t *d;
    char name[OS_PATH_MAX];
    struct stat sb;
    char tmp[OS_PATH_MAX];
    int r;

    if (depth >= ADD_FOLDER_MAX_DEPTH) {
        pakka_die("Directory nesting too deep at '%s' (max %d)",
                   path, ADD_FOLDER_MAX_DEPTH);
    }
    if (!(d = pakka_platform_opendir(path))) {
        pakka_die("Cannot open directory %s", path);
    }
    while ((r = pakka_platform_readdir(d, name, sizeof(name))) > 0) {
        if (strcmp(name, "..") == 0 || strcmp(name, ".") == 0) {
            continue;
        }
        if (build_filename(tmp, sizeof(tmp), path, name) != 0) {
            pakka_platform_closedir(d);
            pakka_die_e(0, "Path too long: %s/%s", path, name);
        }
        if (pakka_platform_is_reparse_or_symlink(tmp)) {
            fprintf(stderr, "Skipping symlink/reparse point %s\n", tmp);
            continue;
        }
        if (pakka_platform_lstat(tmp, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                cli_add_folder_r(pak, tmp, depth + 1);
            } else if (S_ISREG(sb.st_mode)) {
                pakka_error_t err;
                pakka_status_t s;
                printf("Adding %s to pak\n", tmp);
                s = pakka_add_file(pak, tmp, tmp, &err);
                if (s != PAKKA_OK) {
                    fail_from_err(&err);
                }
            }
        } else {
            int stat_errno = errno;
            pakka_platform_closedir(d);
            pakka_die_e(stat_errno, "Couldn't stat %s", tmp);
        }
    }
    if (r < 0) {
        int read_errno = errno;
        pakka_platform_closedir(d);
        pakka_die_e(read_errno, "readdir failed in %s", path);
    }
    pakka_platform_closedir(d);
}

static void op_remove(pakka_archive_t *pak, char **paths, int path_count) {
    pakka_error_t err;
    pakka_status_t s;
    int i;

    for (i = 0; i < path_count; i++) {
        s = pakka_delete(pak, paths[i], &err);
        if (s != PAKKA_OK) {
            fail_from_err(&err);
        }
    }

    s = pakka_commit(pak, &err);
    if (s != PAKKA_OK) {
        fail_from_err(&err);
    }
}

static void verify_report_cb(void *userdata,
                             pakka_report_severity_t severity,
                             pakka_status_t status,
                             const char *entry_name,
                             const char *message) {
    (void)userdata;
    (void)status;
    {
        FILE *out = (severity == PAKKA_REPORT_INFO) ? stdout : stderr;
        const char *tag = (severity == PAKKA_REPORT_ERROR) ? "ERROR"
                       : (severity == PAKKA_REPORT_WARNING) ? "WARNING"
                       : "INFO";
        if (entry_name != NULL && entry_name[0] != '\0') {
            char safe[PAKFILE_PATH_BUF];
            pakka_sanitize_name(safe, sizeof(safe), entry_name);
            fprintf(out, "%s [%s]: %s\n", tag, safe, message);
        } else {
            fprintf(out, "%s: %s\n", tag, message);
        }
    }
}

static void op_verify(pakka_archive_t *pak, int deep) {
    pakka_error_t err;
    unsigned flags = deep ? PAKKA_VERIFY_DEEP : 0u;
    pakka_status_t s = pakka_verify(pak, flags, verify_report_cb, NULL, &err);
    if (s != PAKKA_OK) {
        /* Report callback already printed per-finding diagnostics;
         * surface a final aggregate line on stderr and exit non-zero. */
        fprintf(stderr, "Verify FAILED (status %d)\n", (int)s);
        pakka_close(pak, NULL);
        exit(1);
    }
}

static int build_filename(char *dest, size_t dest_size,
                          const char *basedir, const char *filename) {
    size_t blen = strlen(basedir);
    int needs_sep = (blen > 0 && basedir[blen - 1] != '/'
                              && basedir[blen - 1] != '\\');
    int written = snprintf(dest, dest_size, "%s%s%s",
                           basedir, needs_sep ? "/" : "", filename);
    if (written < 0 || (size_t)written >= dest_size) {
        return -1;
    }
    return 0;
}

static int strip_long_options(int argc, char **argv, opts_t *opts) {
    int src;
    int dst = 1;
    int option_end = 0;
    int as_capacity = 0;
    for (src = 1; src < argc; src++) {
        if (!option_end && strcmp(argv[src], "--") == 0) {
            option_end = 1;
        } else if (!option_end && strcmp(argv[src], "--help") == 0) {
            help();         /* exits */
        } else if (!option_end && strcmp(argv[src], "--version") == 0) {
            version();      /* exits */
        } else if (!option_end && strcmp(argv[src], "--tree") == 0) {
            opts->tree = 1;
            continue;
        } else if (!option_end && strcmp(argv[src], "--verify") == 0) {
            setmodetype(opts, PAK_VERIFY);
            continue;
        } else if (!option_end && strcmp(argv[src], "--deep") == 0) {
            opts->deep = 1;
            continue;
        } else if (!option_end && strcmp(argv[src], "--compress") == 0) {
            opts->compress = 1;
            continue;
        } else if (!option_end && strcmp(argv[src], "--format") == 0) {
            if (src + 1 >= argc) {
                fprintf(stderr,
                        "--format requires one argument: "
                        "pak | goldsrc | hl | sin | daikatana | iwad | pwad | pk3 | pk4\n");
                usage();
            }
            {
                const char *name = argv[src + 1];
                /* GoldSrc PAKs (Half-Life 1, CS 1.6, TFC, ...) are
                 * bit-identical to Quake/Q2 PAK; the aliases give modders
                 * a discoverable name on --help. IWAD and PWAD are not
                 * bit-identical (different 4-byte magic), so they get
                 * their own tokens — no shared "wad" alias. */
                if (strcmp(name, "pak") == 0
                    || strcmp(name, "goldsrc") == 0
                    || strcmp(name, "hl") == 0) {
                    opts->format = PAKKA_FORMAT_PAK;
                } else if (strcmp(name, "sin") == 0) {
                    opts->format = PAKKA_FORMAT_SIN;
                } else if (strcmp(name, "daikatana") == 0
                           || strcmp(name, "dk") == 0) {
                    opts->format = PAKKA_FORMAT_DAIKATANA;
                } else if (strcmp(name, "iwad") == 0) {
                    opts->format = PAKKA_FORMAT_IWAD;
                } else if (strcmp(name, "pwad") == 0) {
                    opts->format = PAKKA_FORMAT_PWAD;
                } else if (strcmp(name, "pk3") == 0) {
                    opts->format = PAKKA_FORMAT_PK3;
                } else if (strcmp(name, "pk4") == 0) {
                    opts->format = PAKKA_FORMAT_PK4;
                } else if (strcmp(name, "auto") == 0) {
                    opts->format = PAKKA_FORMAT_AUTO;
                } else {
                    fprintf(stderr,
                            "Unknown --format value: %s "
                            "(use pak, goldsrc, hl, sin, daikatana, iwad, pwad, pk3, or pk4)\n",
                            name);
                    usage();
                }
            }
            src += 1;
            continue;
        } else if (!option_end && strcmp(argv[src], "--as") == 0) {
            if (src + 2 >= argc) {
                fprintf(stderr,
                        "--as requires two arguments: <entry_name> <source_path>\n");
                usage();
            }
            if (opts->aliased_count >= as_capacity) {
                char **new_entries;
                char **new_sources;
                as_capacity = as_capacity ? as_capacity * 2 : 4;
                new_entries = realloc(opts->aliased_entries,
                                      sizeof(char *) * (size_t)as_capacity);
                if (new_entries == NULL) {
                    pakka_die("Cannot allocate --as entry buffer");
                }
                opts->aliased_entries = new_entries;
                new_sources = realloc(opts->aliased_sources,
                                      sizeof(char *) * (size_t)as_capacity);
                if (new_sources == NULL) {
                    pakka_die("Cannot allocate --as source buffer");
                }
                opts->aliased_sources = new_sources;
            }
            opts->aliased_entries[opts->aliased_count] = argv[src + 1];
            opts->aliased_sources[opts->aliased_count] = argv[src + 2];
            opts->aliased_count++;
            src += 2;
            continue;
        }
        argv[dst++] = argv[src];
    }
    argv[dst] = NULL;
    return dst;
}

static int parseopts(int argc, char *argv[], opts_t *opts) {
    int c;

    argc = strip_long_options(argc, argv, opts);
    if (argc < 2) {
        usage();
    }

    while ((c = getopt(argc, argv, "lxcadhVC:")) != -1) {
        switch (c) {
            case 'l': setmodetype(opts, PAK_LIST); break;
            case 'x': setmodetype(opts, PAK_EXTRACT); break;
            case 'c': setmodetype(opts, PAK_CREATE); break;
            case 'a': setmodetype(opts, PAK_ADD); break;
            case 'd': setmodetype(opts, PAK_REMOVE); break;
            case 'C': opts->destination = optarg; break;
            case 'h': help(); break;
            case 'V': version(); break;
            default: usage();
        }
    }

    /* First non-option argument is the archive; everything after is
     * paths (entry names for -x/-d, source paths for -a/-c). BSD/macOS
     * getopt is POSIX-strict and stops at the first non-option, so
     * callers must put all option flags (including -C <dir>) before
     * the archive name. Long options are handled by the strip pre-pass
     * so their position is unconstrained.
     *
     * getopt consumes "--" when it appears between option flags, but
     * if it appears between positionals (e.g. "pakka -x pak.pak --
     * --weird-entry"), it stays in argv. Skip both possible positions
     * so "--" never lands in pakfile or paths. */
    if (optind < argc && strcmp(argv[optind], "--") == 0) {
        optind++;
    }
    if (optind < argc) {
        opts->pakfile = argv[optind++];
    }
    if (optind < argc && strcmp(argv[optind], "--") == 0) {
        optind++;
    }
    if (optind < argc) {
        opts->paths = malloc(sizeof(char *) * (argc - optind));
        if (opts->paths == NULL) {
            pakka_die("Cannot allocate path argument list");
        }
        while (optind < argc) {
            opts->paths[opts->path_count++] = argv[optind++];
        }
    }

    if (!opts->mode) {
        fprintf(stderr, "You must specify one -lxcad option\n");
        usage();
    }
    if (!opts->pakfile || opts->pakfile[0] == '\0') {
        fprintf(stderr,
                "You must name the pakfile as the first positional argument\n");
        usage();
    }
    if (opts->tree && opts->mode != PAK_LIST) {
        fprintf(stderr, "--tree may only be used with -l\n");
        usage();
    }
    if (opts->aliased_count > 0
        && opts->mode != PAK_ADD
        && opts->mode != PAK_CREATE) {
        fprintf(stderr, "--as may only be used with -a or -c\n");
        usage();
    }
    if (opts->mode == PAK_ADD
        && opts->path_count == 0 && opts->aliased_count == 0) {
        fprintf(stderr,
                "-a requires at least one path argument or --as pair\n");
        usage();
    }
    if (opts->mode == PAK_REMOVE && opts->path_count == 0) {
        fprintf(stderr, "-d requires at least one path argument\n");
        usage();
    }

    return 0;
}

static void setmodetype(opts_t *opts, short mode) {
    if (!opts->mode) {
        opts->mode = mode;
    } else {
        fprintf(stderr, "You may not specify more than one -lxcad option\n");
        usage();
    }
}

static void usage_banner(void) {
    fprintf(stderr, "%s %s (%s).\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h | -V | [-lxcad] [--tree] [--verify] [--deep] [--compress] [--format <name>] [--as <entry> <source>] [-C <dest>] <pak> [path(s)]\n",
            g_argv0);
}

/* `--version` / `-V` output. Writes to stdout (the user asked for this
 * data and may pipe it elsewhere) and exits 0, unlike usage()/help()
 * which target stderr and exit 1 because they're shown on misuse. */
static void version(void) {
    printf("%s %s (%s)\n", APP_NAME, VERSION, BUILD_DATE);
    printf("libpakka %s (linked)\n", pakka_version());
    printf("\n");
    printf("Supported formats:\n");
    printf("  pak         Quake 1 / 2 and GoldSrc (Half-Life 1, CS 1.6, TFC, ...)  read + write\n");
    printf("              (--format goldsrc and --format hl are aliases for pak)\n");
    printf("  sin         Ritual SiN (1998)    read + write\n");
    printf("  daikatana   Ion Storm (2000)     read + write (.tga/.bmp/.wal/.pcx/.bsp auto-compressed)\n");
    printf("  iwad        id Doom 1 / 2 base   read + write (8-byte lump names; duplicates allowed)\n");
    printf("  pwad        id Doom 1 / 2 patch  read + write (same shape as IWAD; .wad extension defaults here)\n");
    printf("  pk3         Quake 3 / ZIP        read + write (STORED + DEFLATE; pass --compress to encode DEFLATE)\n");
    printf("  pk4         Doom 3 / ZIP         read + write (STORED + DEFLATE; pass --compress to encode DEFLATE)\n");
    printf("\n");
    printf("Use --format <name> to pin the format on open or override the\n");
    printf("create-time extension sniffer. PACK magic is shared between\n");
    printf("Quake and Daikatana; pakka probes both layouts and asks for\n");
    printf("an explicit --format on an ambiguous match.\n");
    printf("\n");
    printf("https://github.com/ajbonner/pakka\n");
    exit(0);
}

static void usage(void) {
    usage_banner();
    exit(1);
}

static void help(void) {
    usage_banner();
    fprintf(stderr, "\nOperation Modes:\n");
    fprintf(stderr, "You must specify exactly one mode\n");
    fprintf(stderr, " -l         list files contained in pak file\n");
    fprintf(stderr, " -x         extract files from pak file\n");
    fprintf(stderr, " -c         create a new pak file\n");
    fprintf(stderr, " -a         add files to pak file\n");
    fprintf(stderr, " -d         remove files from pak file\n");
    fprintf(stderr, " --verify   walk every entry, check name safety and payload readability,\n");
    fprintf(stderr, "            and flag entries that would collide after extraction normalization\n");
    fprintf(stderr, " -h, --help     this help\n");
    fprintf(stderr, " -V, --version  print app + libpakka version and supported formats\n");
    fprintf(stderr, "\nModifiers:\n");
    fprintf(stderr, " --tree                          list pak contents as a directory tree (only with -l)\n");
    fprintf(stderr, " --as <entry_name> <source_path> add source file as the given entry name (only with -a or -c)\n");
    fprintf(stderr, "                                 (repeat --as for multiple aliased pairs; may mix with plain paths)\n");
    fprintf(stderr, " --format <name>                 pin archive format: pak, sin, daikatana, iwad, pwad, pk3, pk4\n");
    fprintf(stderr, "                                 (goldsrc and hl are aliases for pak — Half-Life 1 / CS 1.6 / TFC use Quake PAK)\n");
    fprintf(stderr, "                                 (on open: skip auto-detect; on create: override extension sniffer)\n");
    fprintf(stderr, " --deep                          deeper integrity check (with --verify; ZIP CRC32, DK decode)\n");
    fprintf(stderr, " --compress                      DEFLATE-encode added entries (PK3/PK4 only, with -c or -a)\n");
    fprintf(stderr, "                                 (per-entry auto-fallback to STORED on incompressible payloads)\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -l pak1.pak                  # List contents of pak1.pak\n", g_argv0);
    fprintf(stderr, "  %s -l pak1.pak --tree           # List contents as a directory tree\n", g_argv0);
    fprintf(stderr, "  %s --verify pak1.pak            # Walk every entry, report integrity findings\n", g_argv0);
    fprintf(stderr, "  %s -a pak1.pak --as maps/foo.bsp /tmp/foo.bsp\n", g_argv0);
    fprintf(stderr, "                                  # Add /tmp/foo.bsp as entry 'maps/foo.bsp' (aliasing)\n");
    fprintf(stderr, "  %s -x pak1.pak                  # Extract pak1.pak to current dir\n", g_argv0);
    fprintf(stderr, "  %s -x -C /some/path pak1.pak    # Extract pak1.pak to /some/path\n", g_argv0);
    fprintf(stderr, "  # Extract one specific entry to current dir:\n");
    fprintf(stderr, "  %s -x pak1.pak models/weapons/g_blast/base.pcx\n", g_argv0);
    exit(1);
}
