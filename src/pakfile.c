#include "common.h"

/* UTF-8 box-drawing sequences for --tree output. Encoded as octal escapes
 * so the source file's text encoding doesn't change the emitted bytes. */
#define PAK_TREE_ROOT         "."
#define PAK_TREE_BRANCH_MID   "\342\224\234\342\224\200\342\224\200 "  /* "├── " */
#define PAK_TREE_BRANCH_LAST  "\342\224\224\342\224\200\342\224\200 "  /* "└── " */
#define PAK_TREE_PREFIX_MID   "\342\224\202   "                        /* "│   " */
#define PAK_TREE_PREFIX_LAST  "    "

typedef struct Paktreenode_s {
    char *name;
    int is_dir;
    struct Paktreenode_s *children;
    struct Paktreenode_s *next;
} Paktreenode_t;

static int build_filename(char *dest, size_t dest_size,
                          const char *basedir, const char *filename);
#define ADD_FOLDER_MAX_DEPTH 100
static int add_folder_r(Pak_t *pak, char *path, int depth);
static Paktreenode_t *create_tree_node(const char *name, int is_dir);
static void insert_tree_path(Paktreenode_t *root, const char *path,
                             uint32_t *dir_count, uint32_t *file_count);
static Paktreenode_t *find_tree_dir(Paktreenode_t *parent, const char *name);
static void insert_tree_child(Paktreenode_t *parent, Paktreenode_t *node);
static void print_tree_children(Paktreenode_t *node, const char *prefix);
static void print_tree_summary(uint32_t dir_count, uint32_t file_count);
static void free_tree(Paktreenode_t *node);
static void load_pakfile(Pak_t *pak);
static void load_directory(Pak_t *pak);
static uint64_t compute_payload_end(Pak_t *pak);
static Pakfileentry_t *find_tail(Pak_t *pak);
static void init_pak_header(Pak_t *pak);
static void write_pak_directory(Pak_t *pak);
static Pakfileentry_t *find_entry(Pak_t *pak, char *path);
static int delete_entries(Pak_t *pak, Pakfileentry_t **entries, int num_entries);
static int copy_between_paks(Pakfileentry_t *entry, FILE *ffd, FILE *tfd);
static FILE *create_tmp_pakfile(void);
static int in_array(Pakfileentry_t *entry, Pakfileentry_t **entries, int num_entries);
static int is_unsafe_extract_path(const char *path);

static int is_new = 0;
static char cur_pakpath[OS_PATH_MAX];
static char new_pakpath[OS_PATH_MAX];
/* L_tmpnam on MSVC is 20 bytes — not enough for "C:\Users\...\Temp\pakkaXXXXXX". */
static char tmp_pakpath[OS_PATH_MAX];
static FILE *fp;

Pak_t *open_pakfile(const char *pakpath) {
    strcpy(cur_pakpath, pakpath);
    Pak_t *pak = calloc(sizeof(Pak_t), 1);

    if (! (fp = fopen(pakpath, "r+b"))) {
        error_exit("Cannot open %s", pakpath);
    }

    load_pakfile(pak);
    load_directory(pak);

    return pak;
}

Pak_t *create_pakfile(const char *pakpath) {
    Pak_t *pak = calloc(sizeof(Pak_t), 1);
    struct stat sb;

    is_new = 1;
    strcpy(new_pakpath, pakpath);

    if (stat(pakpath, &sb) == 0) {
        error_exit("File already exists at destination %s", pakpath);
    }

    if (! (fp = compat_mkstemp_open("pakkaXXXXXX", tmp_pakpath, sizeof(tmp_pakpath)))) {
        error_exit("Cannot create temp pak file");
    }

    init_pak_header(pak);

    return pak;
}

int close_pakfile(Pak_t *pak) {
    Pakfileentry_t *e = pak->head;
    Pakfileentry_t *next;

    while (e != NULL) {
        next = e->next;
        free(e);
        e = next;
    }

    free(pak);

    /* fclose BEFORE rename: CRT fopen on Windows does not request
     * FILE_SHARE_DELETE, so MoveFileEx would otherwise be blocked by
     * the still-open handle. delete_entries may have already closed
     * fp on its own (it sets fp = NULL), so tolerate that. */
    if (fp != NULL) {
        fclose(fp);
        fp = NULL;
    }

    if (is_new) {
        /* No-replace semantics: between create_pakfile's stat() check
         * and now another process may have dropped a file at the
         * destination. Atomically refuse to overwrite rather than
         * silently clobbering. */
        if (compat_rename_noreplace(tmp_pakpath, new_pakpath) != 0) {
            error_exit("Could not create pak %s (destination may already exist)",
                       new_pakpath);
        }
    }

    return 0;
}

void load_pakfile(Pak_t *pak) {
    long size;

    /* Capture the actual file size up front so every bounds check below
     * compares against ground truth rather than self-reported header
     * values. */
    if (fseek(fp, 0L, SEEK_END) != 0) {
        error_exit("Cannot seek pak file");
    }
    size = ftell(fp);
    if (size < 0) {
        error_exit("Cannot determine pak file size");
    }
    pak->file_size = (uint64_t)size;

    rewind(fp);
    if (fread(pak->signature, PAKFILE_SIGNATURE_LEN, 1, fp) != 1
        || fread(&pak->diroffset, sizeof(uint32_t), 1, fp) != 1
        || fread(&pak->dirlength, sizeof(uint32_t), 1, fp) != 1) {
        error_exit("Cannot read pak header");
    }

    if (memcmp(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN) != 0) {
        error_exit("Not a pak file (bad signature)");
    }

    if ((pak->dirlength % PAKFILE_DIR_ENTRY_SIZE) != 0) {
        error_exit("Pak header is corrupt (dirlength not a multiple of %d)",
                   PAKFILE_DIR_ENTRY_SIZE);
    }

    if (pak->diroffset < PAKFILE_HEADER_SIZE) {
        error_exit("Pak header is corrupt (diroffset inside header)");
    }

    /* Guard the diroffset+dirlength sum against u32 wrap, then check it
     * sits within the actual file. Compare via subtraction to avoid the
     * overflow risk on the addition itself. */
    if ((uint64_t)pak->dirlength > pak->file_size
        || (uint64_t)pak->diroffset > pak->file_size - pak->dirlength) {
        error_exit("Pak header is corrupt (directory extends past EOF)");
    }

    pak->num_entries = pak->dirlength / PAKFILE_DIR_ENTRY_SIZE;

    if (pak->num_entries > PAKFILE_MAX_ENTRIES) {
        error_exit("Pak has too many entries (%" PRIu32 ", max %u)",
                   pak->num_entries, PAKFILE_MAX_ENTRIES);
    }
}

void load_directory(Pak_t *pak) {
    Pakfileentry_t *current = NULL;
    Pakfileentry_t *last = NULL;
    uint32_t i;
    uint64_t entry_pos;

    if (pak->num_entries == 0) {
        return;
    }

    for (i = 1; i <= pak->num_entries; i++) {
        /* Compute in 64-bit to avoid u32 wrap. load_pakfile has already
         * validated diroffset+dirlength <= file_size and num_entries
         * against the dirlength, so the subtraction can't underflow. */
        entry_pos = (uint64_t)pak->diroffset + (uint64_t)pak->dirlength
                    - (uint64_t)i * PAKFILE_DIR_ENTRY_SIZE;

        if (fseek(fp, (long)entry_pos, SEEK_SET) != 0) {
            error_exit("Cannot seek to pak directory entry %" PRIu32, i);
        }

        current = calloc(1, sizeof(Pakfileentry_t));
        if (current == NULL) {
            error_exit("Cannot allocate pak directory entry %" PRIu32, i);
        }

        if (fread(current->filename, PAKFILE_PATH_MAX, 1, fp) != 1
            || fread(&current->offset, sizeof(uint32_t), 1, fp) != 1
            || fread(&current->length, sizeof(uint32_t), 1, fp) != 1) {
            error_exit("Cannot read pak directory entry %" PRIu32, i);
        }

        /* The on-disk filename is exactly PAKFILE_PATH_MAX bytes with no
         * guaranteed NUL. The in-memory buffer is one byte larger so we
         * can force termination before any strlen/strcmp/printf("%s")
         * downstream overreads into the offset/length fields. */
        current->filename[PAKFILE_PATH_MAX] = '\0';

        /* Bounds-check the entry against the file we actually have on
         * disk. Reject anything that points into the header or wraps
         * past EOF. Subsequent extract/copy paths can then trust the
         * stored offset+length without re-validating. */
        if (current->offset < PAKFILE_HEADER_SIZE
            || (uint64_t)current->length > pak->file_size
            || (uint64_t)current->offset > pak->file_size - current->length) {
            error_exit("Pak entry %" PRIu32 " bytes out of range "
                       "(offset=%" PRIu32 ", length=%" PRIu32 ")",
                       i, current->offset, current->length);
        }

        if (last != NULL) {
            current->next = last;
        }

        last = current;
    }

    pak->head = last;
}

void list_files(Pak_t *pak) {
    Pakfileentry_t *current = pak->head;

    if (current == NULL) {
        printf("Pak is empty\n");
        return;
    }

    do {
        printf("%s (%" PRIu32 " bytes)\n", current->filename, current->length);
    } while ((current = current->next) != NULL);
}

void list_files_tree(Pak_t *pak) {
    Pakfileentry_t *current = pak->head;
    Paktreenode_t *root = create_tree_node(PAK_TREE_ROOT, 1);
    uint32_t dir_count = 0;
    uint32_t file_count = 0;

    printf("%s\n", PAK_TREE_ROOT);

    if (current != NULL) {
        do {
            insert_tree_path(root, current->filename, &dir_count, &file_count);
        } while ((current = current->next) != NULL);

        print_tree_children(root, "");
    }

    print_tree_summary(dir_count, file_count);
    free_tree(root);
}

Paktreenode_t *create_tree_node(const char *name, int is_dir) {
    Paktreenode_t *node = calloc(1, sizeof(Paktreenode_t));

    node->name = malloc(strlen(name) + 1);
    strcpy(node->name, name);
    node->is_dir = is_dir;

    return node;
}

void insert_tree_path(Paktreenode_t *root, const char *path,
                      uint32_t *dir_count, uint32_t *file_count) {
    char *path_copy;
    char *component;
    char *next;
    Paktreenode_t *parent = root;
    Paktreenode_t *dir;

    if (path[0] == '\0') {
        return;
    }

    path_copy = compat_strdup(path);
    component = strtok(path_copy, "/");

    while (component != NULL) {
        next = strtok(NULL, "/");

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
        }

        component = next;
    }

    free(path_copy);
}

Paktreenode_t *find_tree_dir(Paktreenode_t *parent, const char *name) {
    Paktreenode_t *current = parent->children;

    while (current != NULL) {
        if (current->is_dir && strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

void insert_tree_child(Paktreenode_t *parent, Paktreenode_t *node) {
    Paktreenode_t *current = parent->children;
    Paktreenode_t *previous = NULL;

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

void print_tree_children(Paktreenode_t *node, const char *prefix) {
    Paktreenode_t *current = node->children;
    char *next_prefix;
    const char *branch;
    const char *prefix_suffix;

    while (current != NULL) {
        branch        = current->next == NULL ? PAK_TREE_BRANCH_LAST : PAK_TREE_BRANCH_MID;
        prefix_suffix = current->next == NULL ? PAK_TREE_PREFIX_LAST : PAK_TREE_PREFIX_MID;

        printf("%s%s%s\n", prefix, branch, current->name);

        if (current->is_dir) {
            next_prefix = malloc(strlen(prefix) + strlen(prefix_suffix) + 1);
            strcpy(next_prefix, prefix);
            strcat(next_prefix, prefix_suffix);
            print_tree_children(current, next_prefix);
            free(next_prefix);
        }

        current = current->next;
    }
}

void print_tree_summary(uint32_t dir_count, uint32_t file_count) {
    printf("\n%" PRIu32 " %s, %" PRIu32 " %s\n",
           dir_count,
           dir_count == 1 ? "directory" : "directories",
           file_count,
           file_count == 1 ? "file" : "files");
}

void free_tree(Paktreenode_t *node) {
    Paktreenode_t *current = node;
    Paktreenode_t *next;

    while (current != NULL) {
        next = current->next;
        free_tree(current->children);
        free(current->name);
        free(current);
        current = next;
    }
}

int add_files(Pak_t *pak, char **paths, int path_count) {
    int i;

    for (i = 0; i < path_count; i++) {
        if (add_to_pak(pak, paths[i]) != 0) {
            error_exit("Could not add %s to pak", paths[i]);
        }
    }

    write_pak_directory(pak);

    return 0;
}

int add_to_pak(Pak_t *pak, char* path) {
    struct stat sb;

    /* lstat — not stat — so a symlink at the user-supplied path is
     * detected as a symlink instead of silently dereferenced. Pakka
     * has no --follow-symlinks flag yet; reject explicitly. */
    if (compat_lstat(path, &sb) != 0) {
        error_exit("Cannot add %s", path);
    } else if (S_ISLNK(sb.st_mode)) {
        error_exit("Refusing to add symlink %s (use the target path directly)", path);
    } else if (S_ISREG(sb.st_mode)) {
        add_file(pak, path);
    } else if (S_ISDIR(sb.st_mode)) {
        add_folder(pak, path);
    } else {
        error_exit("Error adding %s: can only add regular files and folders", path);
    }

    return 0;
}

int add_file(Pak_t *pak, char *path) {
    FILE *tfd;
    int64_t size;
    uint64_t append_offset;
    Pakfileentry_t *tail;
    Pakfileentry_t *entry;
    char *bytes;
    size_t path_len;

    printf("Adding %s to pak\n", path);

    /* The on-disk filename field is PAKFILE_PATH_MAX bytes including its
     * NUL terminator, so the longest legal name is PAKFILE_PATH_MAX-1
     * bytes. Reject before the strcpy that would otherwise overwrite
     * entry->offset / length / next on the heap. */
    path_len = strlen(path);
    if (path_len >= PAKFILE_PATH_MAX) {
        error_exit("Path '%s' too long for pak entry (%zu bytes, max %d)",
                   path, path_len, PAKFILE_PATH_MAX - 1);
    }

    /* Quake's pak format permits duplicate entry names, but find_entry
     * only ever returns the first match — every operation after a
     * duplicate add is ambiguous. Reject up front. */
    if (find_entry(pak, path) != NULL) {
        error_exit("Entry '%s' already exists in pak", path);
    }

    if (! (tfd = fopen(path, "rb"))) {
        error_exit("Cannot open %s", path);
    }

    size = filesize(tfd);
    if (size < 0) {
        error_exit("Cannot determine size of %s", path);
    }
    if ((uint64_t)size > UINT32_MAX) {
        error_exit("File %s too large for pak format (%" PRId64
                   " bytes, max %u)", path, size, UINT32_MAX);
    }

    entry = calloc(sizeof(Pakfileentry_t), 1);
    if (entry == NULL) {
        error_exit("Cannot allocate pak entry for %s", path);
    }

    /* Append at the highest byte-offset across all existing entries, not
     * at the directory-order tail. Quake's pak0.pak has entries out of
     * byte order and an orphan payload past the directory; trusting the
     * directory-tail's offset+length would overwrite live data. */
    tail = find_tail(pak);
    if (tail == NULL) {
        append_offset = PAKFILE_HEADER_SIZE;
    } else {
        append_offset = compute_payload_end(pak);
    }

    if (append_offset > UINT32_MAX - (uint64_t)size) {
        error_exit("Pak would exceed 4 GiB after adding %s", path);
    }

    if (fseek(fp, (long)append_offset, SEEK_SET) != 0) {
        error_exit("Cannot seek to append point in pak");
    }

    if (size > 0) {
        /* Stream in fixed-size chunks. Avoids a malloc that scales with
         * file size — large entries used to fail on 32-bit hosts and
         * burn peak RSS proportional to the payload on 64-bit hosts. */
        size_t remaining = (size_t)size;
        size_t chunk;
        bytes = malloc(PAKFILE_COPY_CHUNK);
        if (bytes == NULL) {
            error_exit("Cannot allocate copy buffer for %s", path);
        }
        while (remaining > 0) {
            chunk = remaining > PAKFILE_COPY_CHUNK ? PAKFILE_COPY_CHUNK : remaining;
            if (fread(bytes, 1, chunk, tfd) != chunk) {
                free(bytes);
                error_exit("Cannot read %s", path);
            }
            if (fwrite(bytes, 1, chunk, fp) != chunk) {
                free(bytes);
                error_exit("Could not add %s to pak", path);
            }
            remaining -= chunk;
        }
        free(bytes);
    }

    memcpy(entry->filename, path, path_len);
    entry->filename[path_len] = '\0';
    entry->length = (uint32_t)size;
    entry->offset = (uint32_t)append_offset;

    if (tail == NULL) {
        pak->head = entry;
    } else {
        tail->next = entry;
    }

    fclose(tfd);

    return 0;
}

int add_folder(Pak_t *pak, char *path) {
    return add_folder_r(pak, path, 0);
}

int add_folder_r(Pak_t *pak, char *path, int depth) {
    DIR *d;
    struct dirent *dirp;
    struct stat sb;
    char tmp[OS_PATH_MAX];

    if (depth >= ADD_FOLDER_MAX_DEPTH) {
        error_exit("Directory nesting too deep at '%s' (max %d)",
                   path, ADD_FOLDER_MAX_DEPTH);
    }

    if (! (d = opendir(path))) {
        error_exit("Cannot open directory %s", path);
    }

    while ((dirp = readdir(d)) != NULL) {
        if (strcmp(dirp->d_name, "..") == 0
           || strcmp(dirp->d_name, ".") == 0) {
            continue;
        }

        if (build_filename(tmp, sizeof(tmp), path, dirp->d_name) != 0) {
            closedir(d);
            error_exit("Path too long: %s/%s", path, dirp->d_name);
        }

        /* lstat per entry so a symlinked subdir doesn't escape the
         * tree the user asked us to pack. Skipping is the safer
         * default; loud-failing would break legitimate trees that
         * incidentally contain symlinks. */
        if (compat_lstat(tmp, &sb) == 0) {
            if (S_ISLNK(sb.st_mode)) {
                fprintf(stderr, "Skipping symlink %s\n", tmp);
            } else if (S_ISDIR(sb.st_mode)) {
                add_folder_r(pak, tmp, depth + 1);
            } else if (S_ISREG(sb.st_mode)) {
                add_file(pak, tmp);
            }
        } else {
            closedir(d);
            error_exit("Couldn't stat %s", tmp);
        }
    }

    closedir(d);
    return 0;
}

/* Write a single pak entry's payload under dest_dir. The compat
 * helper does the path join, creates missing directories, and refuses
 * to follow any existing symlink/reparse point under dest_dir — so
 * an attacker who plants `models -> /tmp/outside` in dest_dir can't
 * redirect the write. Caller has already validated current->filename
 * for path-traversal characters. */
static void extract_one_entry(Pakfileentry_t *current, const char *dest_dir) {
    unsigned char *buffer;
    FILE *tfd;

    printf("Writing %" PRIu32 " bytes to %s/%s\n",
           current->length, dest_dir, current->filename);

    tfd = compat_open_extract_target(dest_dir, current->filename);
    if (tfd == NULL) {
        error_exit("Cannot open %s/%s for writing "
                   "(symlink in path or filesystem error)",
                   dest_dir, current->filename);
    }

    /* Zero-length entries: skip the I/O entirely (malloc(0) and fread/
     * fwrite with size 0 are implementation-defined). The open above
     * still materializes the empty file on disk. */
    if (current->length > 0) {
        if (fseek(fp, (long)current->offset, SEEK_SET) != 0) {
            error_exit("Cannot seek to entry '%s' in pak", current->filename);
        }
        buffer = malloc(current->length);
        if (buffer == NULL) {
            error_exit("Cannot allocate %" PRIu32 " bytes for '%s'",
                       current->length, current->filename);
        }
        if (fread(buffer, current->length, 1, fp) != 1) {
            error_exit("Cannot read entry '%s' from pak", current->filename);
        }
        if (fwrite(buffer, current->length, 1, tfd) != 1) {
            error_exit("Cannot write entry '%s'", current->filename);
        }
        free(buffer);
    }

    fclose(tfd);
}

/*
 * Two-pass extract:
 *
 *   Pass 1 (preflight): walk every entry, match against requested
 *   paths, run is_unsafe_extract_path on the selected ones, verify
 *   every requested path is found in the pak. No filesystem side
 *   effects. error_exit anywhere here leaves the destination dir
 *   untouched.
 *
 *   Pass 2 (write): walk the same entries, extract the ones flagged
 *   by pass 1.
 *
 * Pre-fix the validation was interleaved with writes, so a pak with
 * "safe/a" followed by "../etc/passwd" would write safe/a to disk
 * before bailing on the traversal entry.
 */
void extract_files(Pak_t *pak, char *dest, char **paths, int path_count) {
    Pakfileentry_t *current;
    int *path_matched = NULL;
    int *should_extract = NULL;
    uint32_t idx;
    int i, matched;

    if (pak->head == NULL) {
        if (path_count > 0) {
            error_exit("Pak is empty; cannot extract %s", paths[0]);
        }
        printf("Pak is empty\n");
        return;
    }

    if (pak->num_entries > 0) {
        should_extract = calloc(pak->num_entries, sizeof(int));
        if (should_extract == NULL) {
            error_exit("Cannot allocate extract preflight buffer");
        }
    }
    if (path_count > 0) {
        path_matched = calloc(path_count, sizeof(int));
        if (path_matched == NULL) {
            error_exit("Cannot allocate path-match buffer");
        }
    }

    /* Pass 1: preflight. */
    idx = 0;
    for (current = pak->head; current != NULL; current = current->next) {
        if (idx >= pak->num_entries) {
            error_exit("Pak directory linked list longer than num_entries");
        }

        if (path_count == 0) {
            matched = 1;
        } else {
            matched = 0;
            /* Don't break on first match: `pakka -xf foo a a` must
             * mark both path_matched[] entries or the coverage check
             * below spuriously errors on the duplicate. */
            for (i = 0; i < path_count; i++) {
                if (strcmp(current->filename, paths[i]) == 0) {
                    matched = 1;
                    path_matched[i] = 1;
                }
            }
        }

        if (matched) {
            if (is_unsafe_extract_path(current->filename)) {
                error_exit("Refusing to extract '%s': entry name would escape destination",
                           current->filename);
            }
            should_extract[idx] = 1;
        }
        idx++;
    }

    if (path_count > 0) {
        for (i = 0; i < path_count; i++) {
            if (! path_matched[i]) {
                error_exit("Cannot find %s in pak file", paths[i]);
            }
        }
    }

    /* Pass 2: write. compat_open_extract_target performs the dest_dir +
     * rel_path join internally with per-component symlink rejection,
     * so we don't precompute a flat destfile string. */
    idx = 0;
    for (current = pak->head; current != NULL; current = current->next) {
        if (should_extract[idx]) {
            extract_one_entry(current, dest);
        }
        idx++;
    }

    free(should_extract);
    free(path_matched);
}

void delete_files(Pak_t *pak, char *paths[], int path_count) {
    int i;
    Pakfileentry_t *entry;
    Pakfileentry_t **to_delete;

    to_delete = calloc(sizeof(Pakfileentry_t *), path_count);

    for (i = 0; i < path_count; i++) {
        if ((entry = find_entry(pak, paths[i])) != NULL) {
            to_delete[i] = entry;
        } else {
            error_exit("Cannot find %s in pak file", paths[i]);
        }
    }

    if (delete_entries(pak, to_delete, path_count) != 0) {
        error_exit("Could not remove paths from pak file");
    }

    free(to_delete);
}

Pakfileentry_t *find_entry(Pak_t *pak, char *path) {
    Pakfileentry_t *current;

    if (pak->head == NULL) {
        return NULL;
    }

    current = pak->head;

    do {
        if (strcmp(current->filename, path) == 0) {
            return current;
        }
    } while ((current = current->next) != NULL);
    
    return NULL;
}

/*
 * Rebuild the pak in a temp file: copy each retained entry (in directory
 * order) by seeking to its source offset, write a fresh directory, and rename
 * the temp file over the original. This avoids any assumption about whether
 * file bytes are laid out in directory order — id's pak0.pak famously isn't.
 */
int delete_entries(Pak_t *pak, Pakfileentry_t *entries[], int num_entries) {
    Pakfileentry_t *current = NULL;
    Pakfileentry_t *last = NULL;
    FILE *tfd;
    FILE *tempfp;

    if ((current = pak->head) == NULL) {
        return -1;
    }

    tfd = create_tmp_pakfile();

    do {
        if (! in_array(current, entries, num_entries)) {
            copy_between_paks(current, fp, tfd);

            if (last == NULL) {
                pak->head = current;
            } else {
                last->next = current;
            }
            last = current;
        }
    } while ((current = current->next) != NULL);

    if (last != NULL) {
        last->next = NULL;
    } else {
        pak->head = NULL;
    }

    tempfp = fp;
    fp = tfd;
    write_pak_directory(pak);
    fp = tempfp;

    fclose(tfd);

    /* Close the original pak before the rename. On Windows the open
     * handle on cur_pakpath would block MoveFileEx; on POSIX this just
     * ensures buffered data is flushed before we replace the file
     * underneath ourselves. close_pakfile tolerates fp == NULL. */
    fclose(fp);
    fp = NULL;

    if (compat_rename_replace(tmp_pakpath, cur_pakpath) != 0) {
        error_exit("Could not rename tmp pak %s to final pak %s", tmp_pakpath, new_pakpath);
    }

    return 0;
}

int copy_between_paks(Pakfileentry_t *entry, FILE *ffd, FILE *tfd) {
    char *buffer;
    long new_offset;
    uint32_t src_offset = entry->offset;

    /* Seek the source pak to the entry's existing offset BEFORE we
     * overwrite entry->offset with the temp pak position. load_directory
     * already validated src_offset + entry->length against file_size. */
    if (fseek(ffd, (long)src_offset, SEEK_SET) != 0) {
        error_exit("Cannot seek to source entry '%s'", entry->filename);
    }

    new_offset = ftell(tfd);
    if (new_offset < 0) {
        error_exit("Cannot determine offset in temp pak");
    }
    /* The temp pak is built from scratch, so its offsets must fit u32.
     * Cap explicitly to keep a corrupt rebuild from silently wrapping. */
    if ((uint64_t)new_offset > UINT32_MAX) {
        error_exit("Temp pak exceeds 4 GiB during rebuild");
    }
    entry->offset = (uint32_t)new_offset;

    if (entry->length == 0) {
        return 0;
    }

    buffer = malloc(entry->length);
    if (buffer == NULL) {
        error_exit("Cannot allocate %" PRIu32 " bytes for '%s'",
                   entry->length, entry->filename);
    }

    if (fread(buffer, entry->length, 1, ffd) != 1) {
        free(buffer);
        error_exit("Error reading entry '%s' from source pak", entry->filename);
    }

    if (fwrite(buffer, entry->length, 1, tfd) != 1) {
        free(buffer);
        error_exit("Error writing entry '%s' to dest pak", entry->filename);
    }

    free(buffer);
    return 0;
}

int in_array(Pakfileentry_t *entry, Pakfileentry_t *entries[], int num_entries) {
    int i;
    for (i = 0; i < num_entries; i++) {
        if (entries[i] == entry) {
            return 1;
        }
    }

    return 0;
}

/* Bounded "basedir/filename" join. Returns 0 on success, non-zero if the
 * combined result wouldn't fit in dest_size (NUL included). Replaces the
 * old strcat-into-fixed-buffer pattern that overflowed stack buffers on
 * deep paths. */
int build_filename(char *dest, size_t dest_size,
                   const char *basedir, const char *filename) {
    int n = snprintf(dest, dest_size, "%s/%s", basedir, filename);
    if (n < 0 || (size_t)n >= dest_size) {
        return -1;
    }
    return 0;
}

/* Windows reserved device names. Matched against the segment's base
 * (everything before the first '.') case-insensitively, regardless of
 * extension: "CON.txt" is still the console. COM0/LPT0 are accepted by
 * recent Windows versions; include them for safety. */
static const struct { const char *name; size_t len; } win_reserved_names[] = {
    {"CON", 3}, {"PRN", 3}, {"AUX", 3}, {"NUL", 3},
    {"COM0", 4}, {"COM1", 4}, {"COM2", 4}, {"COM3", 4}, {"COM4", 4},
    {"COM5", 4}, {"COM6", 4}, {"COM7", 4}, {"COM8", 4}, {"COM9", 4},
    {"LPT0", 4}, {"LPT1", 4}, {"LPT2", 4}, {"LPT3", 4}, {"LPT4", 4},
    {"LPT5", 4}, {"LPT6", 4}, {"LPT7", 4}, {"LPT8", 4}, {"LPT9", 4}
};

static int is_reserved_device_name(const char *seg, size_t seg_len) {
    size_t base_len, i, j;
    char a, b;

    /* Match the name up to the first '.' so "CON.txt" still hits "CON". */
    base_len = 0;
    while (base_len < seg_len && seg[base_len] != '.') {
        base_len++;
    }

    for (i = 0; i < sizeof(win_reserved_names) / sizeof(win_reserved_names[0]); i++) {
        if (win_reserved_names[i].len != base_len) continue;
        for (j = 0; j < base_len; j++) {
            a = seg[j];
            b = win_reserved_names[i].name[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (a != b) break;
        }
        if (j == base_len) return 1;
    }
    return 0;
}

/* Per-segment safety check. Centralizes everything we know about a
 * single path component:
 *  - empty: allowed (lets "foo//bar" through; the duplicate slash
 *    collapses harmlessly on disk).
 *  - exact "..": rejected (path-traversal escape).
 *  - control bytes 0x01-0x1F and DEL: rejected (terminal injection
 *    and filesystem corner cases).
 *  - colon: rejected anywhere in the segment. On Windows this is the
 *    ADS (alternate data stream) separator; subsumes the old drive-
 *    letter check at position 1.
 *  - trailing '.' or ' ': rejected. Windows silently strips both, so
 *    "foo." resolves to "foo" — pak entries must never depend on
 *    that normalization (treats "foo" and "foo." as the same file).
 *    The literal "." segment is allowed (no-op normalization).
 *  - Windows reserved device names (CON/NUL/COM1/...): rejected on
 *    every host because paks are portable.
 */
static int is_unsafe_segment(const char *seg, size_t seg_len) {
    size_t i;
    unsigned char c;

    if (seg_len == 0) return 0;
    if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return 1;

    for (i = 0; i < seg_len; i++) {
        c = (unsigned char)seg[i];
        if (c < 0x20 || c == 0x7F) return 1;
        if (c == ':') return 1;
    }

    /* Trailing dot or space, except for the bare "." no-op segment. */
    if (! (seg_len == 1 && seg[0] == '.')) {
        if (seg[seg_len - 1] == '.' || seg[seg_len - 1] == ' ') return 1;
    }

    if (is_reserved_device_name(seg, seg_len)) return 1;

    return 0;
}

/* Validate a pak entry name before extracting. Pak format places no
 * constraints on entry names beyond a 56-byte upper bound, so a malicious
 * pak can name an entry "../../etc/passwd", "C:\windows\foo", "CON",
 * "foo:stream", or "trailing." and corrupt or redirect the extract.
 * Both POSIX and Windows shapes are checked here because pak archives
 * are portable. */
static int is_unsafe_extract_path(const char *path) {
    const char *p, *seg;

    if (path == NULL || *path == '\0') {
        return 1;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }

    seg = path;
    for (p = path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            if (is_unsafe_segment(seg, (size_t)(p - seg))) {
                return 1;
            }
            if (*p == '\0') {
                break;
            }
            seg = p + 1;
        }
    }

    return 0;
}

Pakfileentry_t *find_tail(Pak_t *pak) {
    Pakfileentry_t *tail;

    if (pak->head == NULL) {
        return NULL;
    }

    if (pak->head->next == NULL) {
        return pak->head;
    }

    tail = pak->head->next;

    while (tail->next != NULL) { tail = tail->next; }

    return tail;
}

/* Walk every entry and return the highest offset+length seen — i.e. the
 * byte one past the last live payload byte in the pak. Quake's original
 * pak0.pak has entries in non-sequential byte order with an orphan payload
 * past the directory, so the directory tail's offset+length is not safe
 * as an append point. load_directory has already validated every entry
 * against file_size, so the arithmetic here cannot overflow u32. */
uint64_t compute_payload_end(Pak_t *pak) {
    Pakfileentry_t *e;
    uint64_t max_end = PAKFILE_HEADER_SIZE;
    uint64_t end;

    for (e = pak->head; e != NULL; e = e->next) {
        end = (uint64_t)e->offset + (uint64_t)e->length;
        if (end > max_end) {
            max_end = end;
        }
    }

    return max_end;
}

void init_pak_header(Pak_t *pak) {
    memcpy(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN);
    pak->diroffset = PAKFILE_HEADER_SIZE;
    pak->dirlength = 0;
}

/* Serialize one pak header to fp. Writes exactly PAKFILE_HEADER_SIZE
 * bytes — the in-memory Pak_t struct has additional bookkeeping fields
 * after dirlength that must not leak to disk. */
static void write_pak_header(Pak_t *pak) {
    if (fwrite(pak->signature, PAKFILE_SIGNATURE_LEN, 1, fp) != 1
        || fwrite(&pak->diroffset, sizeof(uint32_t), 1, fp) != 1
        || fwrite(&pak->dirlength, sizeof(uint32_t), 1, fp) != 1) {
        error_exit("Cannot write pak header");
    }
}

/* Serialize one directory entry: 56-byte filename field (NUL-padded if
 * shorter) followed by two LE u32s. Written field-by-field instead of
 * struct-as-bytes so the in-memory filename[] buffer can be a different
 * size than the on-disk PAKFILE_PATH_MAX without breaking the format. */
static void write_pak_entry(Pakfileentry_t *entry) {
    char name_field[PAKFILE_PATH_MAX];
    size_t len = strlen(entry->filename);

    if (len > PAKFILE_PATH_MAX) {
        len = PAKFILE_PATH_MAX;
    }
    memcpy(name_field, entry->filename, len);
    if (len < PAKFILE_PATH_MAX) {
        memset(name_field + len, 0, PAKFILE_PATH_MAX - len);
    }

    if (fwrite(name_field, PAKFILE_PATH_MAX, 1, fp) != 1
        || fwrite(&entry->offset, sizeof(uint32_t), 1, fp) != 1
        || fwrite(&entry->length, sizeof(uint32_t), 1, fp) != 1) {
        error_exit("Cannot write new pak directory");
    }
}

void write_pak_directory(Pak_t *pak) {
    Pakfileentry_t *current = pak->head;
    Pakfileentry_t *tail;

    /* Empty pak still needs a valid PACK header so the file isn't malformed.
     * This happens after a delete that removes every entry. */
    if (current == NULL) {
        pak->diroffset = PAKFILE_HEADER_SIZE;
        pak->dirlength = 0;
        if (fseek(fp, 0L, SEEK_SET) != 0) {
            error_exit("Cannot seek to pak header");
        }
        write_pak_header(pak);
        return;
    }

    tail = find_tail(pak);
    pak->diroffset = tail->offset + tail->length;
    pak->dirlength = 0;
    if (fseek(fp, (long)pak->diroffset, SEEK_SET) != 0) {
        error_exit("Cannot seek to pak directory");
    }

    do {
        write_pak_entry(current);
        pak->dirlength += PAKFILE_DIR_ENTRY_SIZE;
#ifndef _DEBUG
        debug_directory_entry(current);
#endif
    } while ((current = current->next) != NULL);

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        error_exit("Cannot seek to pak header");
    }

#ifndef _DEBUG
    debug_header(pak);
#endif
    write_pak_header(pak);
}

FILE *create_tmp_pakfile(void) {
    FILE *tfd;

    if (! (tfd = compat_mkstemp_open("pakkaXXXXXX", tmp_pakpath, sizeof(tmp_pakpath)))) {
        error_exit("Cannot create temp pak file");
    }

    fseek(tfd, PAKFILE_HEADER_SIZE, SEEK_SET);

    return tfd;
}
