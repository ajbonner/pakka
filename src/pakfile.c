#include "common.h"

static void build_filename(char *basedir, char *filename, char *dest);
static void load_pakfile(Pak_t *pak);
static void load_directory(Pak_t *pak);

Pak_t *open_pakfile(const char *pakpath) {
    Pak_t *pak = malloc(sizeof(Pak_t));

    if (! (fp = fopen(pakpath, "r"))) {
        error_exit("Cannot open %s", pakpath);
    }

    load_pakfile(pak);
    load_directory(pak);

    return pak;
}

int close_pakfile(Pak_t *pak) {
    Pakfileentry_t *e = pak->head->next;
    
    if (e != NULL) {
        do {
            free(e);
        } while ((e = e->next) != NULL);
    }

    free(pak->head);
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

    fseek(fp, pak->diroffset, SEEK_SET);
    for (int i = 0; i < pak->num_entries; i++) {
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

void extract_files(Pak_t *pak, char *dest) {
    char *destfile, *destdir;
    unsigned char *buffer;
    FILE *tfd;
	int i;
	mode_t default_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    Pakfileentry_t *current = pak->head;

    do {
        /* + 2 one for trailing null and the / we concat between basedir and pakfile path */
        destfile = malloc(sizeof(char) * (strlen(dest) + strlen(current->filename) + 2));
		*destfile = '\0';
        build_filename(dest, current->filename, destfile);

        destdir = dirname(destfile);
		
        if (! (file_exists(destdir)) && (mkdir_r(destdir, default_mode, default_mode) != 0)) {
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
