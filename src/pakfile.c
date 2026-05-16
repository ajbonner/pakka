#include "common.h"
#include "dirent.h"

static void build_filename(char *basedir, char *filename, char *dest);
static void load_pakfile(Pak_t *pak);
static void load_directory(Pak_t *pak);
static Pakfileentry_t *find_tail(Pak_t *pak);
static void init_pak_header(Pak_t *pak);
static void write_pak_directory(Pak_t *pak);
static Pakfileentry_t *find_entry(Pak_t *pak, char *path);
static int delete_entries(Pak_t *pak, Pakfileentry_t **entries, int num_entries);
static int copy_between_paks(Pakfileentry_t *entry, FILE *ffd, FILE *tfd);
static FILE *create_tmp_pakfile(void);
static int in_array(Pakfileentry_t *entry, Pakfileentry_t **entries, int num_entries);

static int is_new = 0;
static char cur_pakpath[OS_PATH_MAX];
static char new_pakpath[OS_PATH_MAX];
static char tmp_pakpath[L_tmpnam];
static FILE *fp;

Pak_t *open_pakfile(const char *pakpath) {
    strcpy(cur_pakpath, pakpath);
    Pak_t *pak = calloc(sizeof(Pak_t), 1);

    if (! (fp = fopen(pakpath, "r+"))) {
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
    strcpy(tmp_pakpath, "/tmp/pakkaXXXXXX");
    strcpy(new_pakpath, pakpath);

    if (stat(pakpath, &sb) == 0) {
        error_exit("File already exists at destination %s", pakpath);
    }

    int fd;
    if ((fd = mkstemp(tmp_pakpath)) == -1) {
        error_exit("Cannot create temp pak file");
    }

    if (! (fp = fdopen(fd, "w"))) {
        close(fd);
        error_exit("Cannot open %s", tmp_pakpath);
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

    if (is_new) {
        if (rename(tmp_pakpath, new_pakpath) == -1) {
            error_exit("Could not rename tmp pak %s to final pak %s", tmp_pakpath, new_pakpath);
        }
    }

    fclose(fp);

    return 0;
}

void load_pakfile(Pak_t *pak) {
    rewind(fp);
    if (fread(pak->signature, PAKFILE_SIGNATURE_LEN, 1, fp) != 1
        || fread(&pak->diroffset, sizeof(uint32_t), 1, fp) != 1
        || fread(&pak->dirlength, sizeof(uint32_t), 1, fp) != 1) {
        error_exit("Cannot read pak header");
    }

    if ((pak->dirlength % PAKFILE_DIR_ENTRY_SIZE) != 0) {
        error_exit("Pak header is corrupt");
    }

    pak->num_entries = pak->dirlength / PAKFILE_DIR_ENTRY_SIZE;
}

void load_directory(Pak_t *pak) {
    Pakfileentry_t *current = NULL;
    Pakfileentry_t *last = NULL;
    int i, entry_pos;

    if (pak->num_entries > 0) {
        for (i = 1; i <= pak->num_entries; i++) {
            entry_pos = pak->diroffset + pak->dirlength 
                - (i * PAKFILE_DIR_ENTRY_SIZE);
            fseek(fp, entry_pos, SEEK_SET);

            current = calloc(1, sizeof(Pakfileentry_t));
            if (fread(current->filename, PAKFILE_PATH_MAX, 1, fp) != 1
                || fread(&current->offset, sizeof(uint32_t), 1, fp) != 1
                || fread(&current->length, sizeof(uint32_t), 1, fp) != 1) {
                error_exit("Cannot read pak directory entry");
            }

            if (last != NULL) {
                current->next = last;
            }

            last = current;
        }
        
        pak->head = last;
    }
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

    if (stat(path, &sb) != 0) {
        error_exit("Cannot add %s", path);
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
    int size;
    Pakfileentry_t *tail;
    Pakfileentry_t *entry;
    char *bytes;

    printf("Adding %s to pak\n", path);
    
    if (! (tfd = fopen(path, "r"))) {
        error_exit("Cannot open %s", path);
    }

    entry = calloc(sizeof(Pakfileentry_t), 1);

    tail = find_tail(pak);
    if (tail == NULL) {
        fseek(fp, PAKFILE_HEADER_SIZE, SEEK_SET);
    } else {
        fseek(fp, tail->offset + tail->length, SEEK_SET);
    }
    
    size = filesize(tfd);
    bytes = malloc(sizeof(char) * size);
    while (fread(bytes, size, 1, tfd)) {
        if (fwrite(bytes, size, 1, fp) != 1)  {
            free(bytes);
            error_exit("Could not add %s to pak\n", path);
        }
    }
    free(bytes);

    if (tail == NULL) {
        strcpy(entry->filename, path);
        entry->length = size;
        entry->offset = PAKFILE_HEADER_SIZE;
        pak->head = entry;
    } else {
        strcpy(entry->filename, path);
        entry->length = size;
        entry->offset = tail->offset + tail->length;
        tail->next = entry;
    }

    fclose(tfd);

    return 0;
}

int add_folder(Pak_t *pak, char *path) {
    DIR *d;
    struct dirent *dirp;
    struct stat sb;
    char tmp[255];

    if (! (d = opendir(path))) {
        error_exit("Cannot open directory %s", path);
    }

    while ((dirp = readdir(d)) != NULL) {
        tmp[0] = '\0';
        if (strcmp(dirp->d_name, "..") == 0
           || strcmp(dirp->d_name, ".") == 0) {
            continue;
        }
        
        build_filename(path, dirp->d_name, tmp);
        
        if (stat(tmp, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                add_folder(pak, tmp);
            } else if (S_ISREG(sb.st_mode)) {
                add_file(pak, tmp);
            }
        } else {
            error_exit("Couldn't stat %s", tmp);
        }
    }

    return 0;
}

void extract_files(Pak_t *pak, char *dest, char **paths, int path_count) {
    char *destfile, *destdir, *destdir_copy;
    unsigned char *buffer;
    FILE *tfd;
    int i, matched;
    int *path_matched = NULL;

    Pakfileentry_t *current = pak->head;

    if (current == NULL) {
        if (path_count > 0) {
            error_exit("Pak is empty; cannot extract %s", paths[0]);
        }
        printf("Pak is empty\n");
        return;
    }

    if (path_count > 0) {
        path_matched = calloc(path_count, sizeof(int));
    }

    do {
        if (path_count > 0) {
            matched = 0;
            /* Don't break on first match: a duplicate argument like
             * `pakka -xf foo default.cfg default.cfg` needs both
             * path_matched[] entries set, or the final completeness
             * check spuriously errors on the duplicate. */
            for (i = 0; i < path_count; i++) {
                if (strcmp(current->filename, paths[i]) == 0) {
                    matched = 1;
                    path_matched[i] = 1;
                }
            }
            if (! matched) continue;
        }

        /* + 2 one for trailing null and the / we concat between basedir and pakfile path */
        destfile = malloc(sizeof(char) * (strlen(dest) + strlen(current->filename) + 2));
		*destfile = '\0';
        build_filename(dest, current->filename, destfile);

        /* POSIX dirname() may modify its argument and/or return a static buffer
         * that subsequent calls overwrite. Pass it a throwaway copy. */
        destdir_copy = strdup(destfile);
        destdir = dirname(destdir_copy);

        if (! (file_exists(destdir)) && (mkdir_r(destdir) != 0)) {
            error_exit("Cannot create directory %s", destdir);
        }
        free(destdir_copy);

        printf("Writing %" PRIu32 " bytes to file %s\n", current->length, destfile);

        fseek(fp, current->offset, SEEK_SET);
        buffer = malloc(sizeof(unsigned char) * current->length);
        fread(buffer, current->length, 1, fp);

        if (! (tfd = fopen(destfile, "w"))) {
            error_exit("Cannot open %s for writing", destfile);
        }

        if (fwrite(buffer, current->length, 1, tfd) != 1) {
            error_exit("Cannot write to %s", destfile);
        }

        fclose(tfd);
        free(buffer);
		free(destfile);
    } while ((current = current->next) != NULL);

    if (path_count > 0) {
        for (i = 0; i < path_count; i++) {
            if (! path_matched[i]) {
                error_exit("Cannot find %s in pak file", paths[i]);
            }
        }
        free(path_matched);
    }
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
    
    if (rename(tmp_pakpath, cur_pakpath) == -1) {
        error_exit("Could not rename tmp pak %s to final pak %s", tmp_pakpath, new_pakpath);
    }

    return 0;
}

int copy_between_paks(Pakfileentry_t *entry, FILE *ffd, FILE *tfd) {
    char *buffer;
    buffer = calloc(entry->length, 1);
    fseek(ffd, entry->offset, SEEK_SET);

    if (fread(buffer, entry->length, 1, ffd) <= 0) {
        free(buffer);
        error_exit("Error reading from source pak\n");
    }
   
    entry->offset = ftell(tfd);
    if (fwrite(buffer, entry->length, 1, tfd) <= 0) {
        free(buffer);
        error_exit("Error writing to dest pak\n");
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

void build_filename(char *basedir, char *filename, char *dest) {
    strcat(dest, basedir);
    strcat(dest, "/");
    strcat(dest, filename);
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

void init_pak_header(Pak_t *pak) {
    memcpy(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN);
    pak->diroffset = PAKFILE_HEADER_SIZE;
    pak->dirlength = 0;
}

void write_pak_directory(Pak_t *pak) {
    Pakfileentry_t *current = pak->head;
    Pakfileentry_t *tail;

    /* Empty pak still needs a valid PACK header so the file isn't malformed.
     * This happens after a delete that removes every entry. */
    if (current == NULL) {
        pak->diroffset = PAKFILE_HEADER_SIZE;
        pak->dirlength = 0;
        fseek(fp, 0L, SEEK_SET);
        if (fwrite(pak, PAKFILE_HEADER_SIZE, 1, fp) != 1) {
            error_exit("Cannot write pak header");
        }
        return;
    }

    tail = find_tail(pak);
    pak->diroffset = tail->offset + tail->length;
    pak->dirlength = 0;
    fseek(fp, pak->diroffset, SEEK_SET);

    do {
        if (fwrite(current, PAKFILE_DIR_ENTRY_SIZE, 1, fp) != 1) {
            error_exit("Cannot write new pak directory");
        }
        pak->dirlength += PAKFILE_DIR_ENTRY_SIZE;
#ifndef _DEBUG
        debug_directory_entry(current);
#endif
    } while ((current = current->next) != NULL);

    fseek(fp, 0L, SEEK_SET);
        
#ifndef _DEBUG
    debug_header(pak);
#endif
    if (fwrite(pak, PAKFILE_HEADER_SIZE, 1, fp) != 1) {
        error_exit("Cannot write pak header");
    }
}

FILE *create_tmp_pakfile(void) {
    FILE *tfd;
    int fd;

    strcpy(tmp_pakpath, "/tmp/pakkaXXXXXX");

    if ((fd = mkstemp(tmp_pakpath)) == -1) {
        error_exit("Cannot create temp pak file");
    }

    if (! (tfd = fdopen(fd, "w"))) {
        close(fd);
        error_exit("Cannot open %s", tmp_pakpath);
    }

    fseek(tfd, PAKFILE_HEADER_SIZE, SEEK_SET);

    return tfd;
}
