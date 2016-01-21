#include "common.h"
#include "dirent.h"

static void build_filename(char *basedir, char *filename, char *dest);
static void load_pakfile(Pak_t *pak);
static void load_directory(Pak_t *pak);
static Pakfileentry_t *find_tail(Pak_t *pak);
static int write_pak_header(Pak_t *pak);
static void write_pak_directory(Pak_t *pak);
static void debug_directory_entry(Pakfileentry_t *entry);

int is_new = 0;
char new_pakpath[OS_PATH_MAX];
char tmp_pakpath[L_tmpnam];
FILE *fp;

Pak_t *open_pakfile(const char *pakpath) {
    Pak_t *pak = calloc(sizeof(Pak_t), 1);

    if (! (fp = fopen(pakpath, "r"))) {
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

    if (! mkstemp(tmp_pakpath)) {
        error_exit("Cannot create temp pak file");
    }

    if (! (fp = fopen(tmp_pakpath, "w"))) {
        error_exit("Cannot open %s", tmp_pakpath);
    }

    if (write_pak_header(pak) != 0) {
        error_exit("Cannot write to temp pak file");
    }

    return pak;
}

int close_pakfile(Pak_t *pak) {
    Pakfileentry_t *e;

    if (pak->head != NULL) {
        e = pak->head->next;
        
        if (e != NULL) {
            do {
                free(e);
            } while ((e = e->next) != NULL);
        }

        free(pak->head);
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
    fread(pak->signature, 4, 1, fp);
    fread(&pak->diroffset, 4, 1, fp);
    fread(&pak->dirlength, 4, 1, fp);
    
    if ((pak->dirlength % PAKFILE_DIR_ENTRY_SIZE) != 0) {
        error_exit("Pak header is corrupt");
    }

    pak->num_entries = pak->dirlength / PAKFILE_DIR_ENTRY_SIZE;
}

void load_directory(Pak_t *pak) {
    Pakfileentry_t *current = NULL;
    Pakfileentry_t *last = NULL;
    int i;

    fseek(fp, pak->diroffset, SEEK_SET);
    if (pak->num_entries > 0) {
        for (i = 0; i < pak->num_entries; i++) {
            current = malloc(sizeof(Pakfileentry_t));
            fread(current->filename, 56, 1, fp);
            fread(&current->offset, 4, 1, fp);
            fread(&current->length, 4, 1, fp);

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
        printf("%s\n", current->filename);
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

    /* add data to pack */
    size = filesize(tfd);
    bytes = malloc(sizeof(char) * size);
    while (fread(bytes, size, 1, tfd)) {
        if (fwrite(bytes, size, 1, fp) == -1)  {
            free(bytes);
            error_exit("Could not add %s to pak\n", path);
        }
    }
    free(bytes);

    /* add entry to directory linked list */
    tail = find_tail(pak);
    if (tail == NULL) { /* this will be the new head */
        strcpy(entry->filename, path);
        entry->length = size;
        entry->offset = pak->diroffset;
        pak->head = entry;
    } else { /* this will be added to the tail */
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

void extract_files(Pak_t *pak, char *dest) {
    char *destfile, *destdir;
    unsigned char *buffer;
    FILE *tfd;

    Pakfileentry_t *current = pak->head;

    do {
        /* + 2 one for trailing null and the / we concat between basedir and pakfile path */
        destfile = malloc(sizeof(char) * (strlen(dest) + strlen(current->filename) + 2));
		*destfile = '\0';
        build_filename(dest, current->filename, destfile);

        destdir = dirname(destfile);
		
        if (! (file_exists(destdir)) && (mkdir_r(destdir) != 0)) {
            error_exit("Cannot create directory %s", destdir);
        }

        printf("Writing %d bytes to file %s\n", current->length, destfile);

        fseek(fp, current->offset, SEEK_SET);
        buffer = malloc(sizeof(unsigned char) * current->length);
        fread(buffer, current->length, 1, fp);

        if (! (tfd = fopen(destfile, "w"))) {
            error_exit("Cannot open %s for writing", destfile);
        }

        if (fwrite(buffer, current->length, 1, tfd) == -1) {
            error_exit("Cannot write to %s", destfile);
        }

        fclose(tfd);
        free(buffer);
		free(destfile);
    } while ((current = current->next) != NULL);
}

void build_filename(char *basedir, char *filename, char *dest) {
    strcat(dest, basedir) ;
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

int write_pak_header(Pak_t *pak) {
    strncpy(pak->signature, "PACK", 4);
    pak->diroffset = 12;
    pak->dirlength = 0;
    
    if (fwrite(pak, PAKFILE_HEADER_SIZE, 1, fp) == -1) {
        return -1;
    }

    return 0;
}

void write_pak_directory(Pak_t *pak) {
    fseek(fp, 0L, SEEK_END);
    Pakfileentry_t *current = pak->head; 

    /* i.e. pak contains no entries */
    if (current == NULL) {
        return;
    }

    pak->diroffset = ftell(fp);

    do {
        if (fwrite(current, PAKFILE_DIR_ENTRY_SIZE, 1, fp) == -1) {
            error_exit("Cannot write new pak directory");
        }
        pak->dirlength += PAKFILE_DIR_ENTRY_SIZE;
    } while ((current = current->next) != NULL);

    fseek(fp, 0L, SEEK_SET);
    fwrite(pak, PAKFILE_HEADER_SIZE, 1, fp);
}

void debug_directory_entry(Pakfileentry_t *entry) {
    if (entry == NULL) { fprintf(stderr, "Null pak entry\n"); }

    fprintf(stderr, "%s\n", entry->filename);
    fprintf(stderr, "%u\n", entry->offset);
    fprintf(stderr, "%u\n", entry->length);
    fprintf(stderr, "%p\n", entry->next);
}
