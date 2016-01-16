#include "common.h"

static void build_filename(char *basedir, char *filename, char *dest);
static void load_pakfile(Pak_t *pak);
static void load_directory(Pak_t *pak);
static Pakfileentry_t *find_tail(Pak_t *pak);
static int filesize (FILE *fd);
static void write_pak_directory(Pak_t *pak);

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

    if (stat(pakpath, &sb) == 0) {
        error_exit("File already exists at destination %s", pakpath);
    }

    if (! (fp = fopen(pakpath, "w"))) {
        error_exit("Cannot open pak file %s", pakpath); 
    }

    strncpy(pak->signature, "PACK", 4);
    pak->diroffset = 4;
    pak->dirlength = 0;
    
    if (fwrite(pak, PAKFILE_HEADER_SIZE, 1, fp) == -1) {
        error_exit("Cannot write to %s", pakpath);
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

    return fclose(fp);
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
        if (add_file(pak, paths[i]) != 0) {
            error_exit("Could not add %s to pak", paths[i]);
        }
    }

    write_pak_directory(pak);

    return 0;
}

int add_file(Pak_t *pak, char* path) {
    struct stat sb;
    FILE *tfd;
    int size;
    Pakfileentry_t *tail;
    Pakfileentry_t *entry;
    unsigned char byte[1];

    if (stat(path, &sb) != 0) {
        error_exit("Cannot add %s", path);
    } else {
        if (! S_ISREG(sb.st_mode)) {
            error_exit("Error adding %s: can only add regular files", path);
        }
    }

    printf("Adding %s to pak\n", path);
    
    if (! (tfd = fopen(path, "r"))) {
        error_exit("Cannot open %s", path);
    }

    entry = calloc(sizeof(Pakfileentry_t), 1);
    
    size = filesize(tfd);
    tail = find_tail(pak);
    /* this will be the new head */
    if (tail == NULL) {
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

    while (fread(byte, 1, 1, tfd)) {
        if (fwrite(byte, 1, 1, fp) == -1)  {
            error_exit("Could not add %s to pak\n", path);
        }
    }

    fclose(tfd);
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

int filesize(FILE *fd) {
    int size;

    fseek(fd, 0L, SEEK_END);
    size = ftell(fd);
    rewind(fd);

    return size;
}

Pakfileentry_t *find_tail(Pak_t *pak) {
    Pakfileentry_t *tail;

    if (pak->head == NULL) {
        return NULL;
    }

    if (pak->head->next == NULL) {
        return pak->head;
    }

    tail = pak->head;

    while ((tail = tail->next) != NULL);

    return tail;
}

void write_pak_directory(Pak_t *pak) {
    fseek(fp, 0L, SEEK_END);
    Pakfileentry_t *current = pak->head; 

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
