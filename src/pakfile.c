#include "common.h"

static void build_filename(char *basedir, char *filename, char *dest);
static void load_pakfile();
static void load_directory();

Pak_t* open_pakfile(const char *pakpath) {
    if (! (fp = fopen(pakpath, "r"))) {
        error_exit("Cannot open %s", pakpath);
    }

    load_pakfile();
    load_directory();

    return &pak;
}

int close_pakfile(Pak_t *pak) {
    return fclose(fp);
}

void load_pakfile() {
    rewind(fp);
    fread(pak.signature, 4, 1, fp);
    pak.signature[4] = '\0';
    fread(&pak.diroffset, 4, 1, fp);
    fread(&pak.dirlength, 4, 1, fp);
    
    if ((pak.dirlength % 64) != 0) {
        error_exit("Pak header is corrupt");
    }

    pak.num_entries = pak.dirlength / 64; 
    pak.files = (Pakfileentry_t *) malloc(sizeof(Pakfileentry_t) * pak.num_entries);
}

void load_directory() {
    fseek(fp, pak.diroffset, SEEK_SET);

    for (int i = 0; i < pak.num_entries; i++) {
        fread(&pak.files[i].filename, 56, 1, fp);
        fread(&pak.files[i].offset, 4, 1, fp);
        fread(&pak.files[i].length, 4, 1, fp);
    }
}

void extract_files(char *dest) {
    char *destfile, *destdir;
    unsigned char *buffer;
    FILE *tfd;
	int i;
	mode_t default_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    for (i = 0; i < pak.num_entries; i++) {
		Pakfileentry_t *file = &pak.files[i];
        /* + 2 one for trailing null and the / we concat between basedir and pakfile path */ 
        destfile = malloc(sizeof(char) * (strlen(dest) + strlen(file->filename) + 2)); 
		*destfile = '\0';
        build_filename(dest, file->filename, destfile);

        destdir = dirname(destfile);
		
        if (! (file_exists(destdir)) && (mkdir_r(destdir, default_mode, default_mode) != 0)) {
            error_exit("Cannot create directory %s", destdir);
        }

        printf("Writing %d bytes to file %s\n", file->length, destfile);

        fseek(fp, file->offset, SEEK_SET);
        buffer = malloc(sizeof(unsigned char) * file->length); 
        fread(buffer, file->length, 1, fp);

        if (! (tfd = fopen(destfile, "w"))) {
            error_exit("Cannot open %s for writing", destfile);
        }

        if (fwrite(buffer, file->length, 1, tfd) == -1) {
            error_exit("Cannot write to %s", destfile);
        }

        fclose(tfd);
        free(buffer);
		free(destfile);
    }
}

void build_filename(char *basedir, char *filename, char *dest) {
    strcat(dest, basedir) ;
    strcat(dest, "/");
    strcat(dest, filename);
}
