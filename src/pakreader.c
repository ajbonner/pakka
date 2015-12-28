#include "header.h"

static void build_filename(char *basedir, char *filename, char *dest);

void load_pakfile() {
    rewind(fp);

    fread(pak_header.signature, 4, 1, fp);
    pak_header.signature[4] = '\0';
    fread(&pak_header.offset, 4, 1, fp);
    fread(&pak_header.length, 4, 1, fp);
    
    if ((pak_header.length % 64) != 0) {
        fprintf(stderr, "Fatal error pak header is corrupt");
        exit(1);
    }

    pak_header.num_entries = pak_header.length / 64; 
}

void load_directory(char* pak_path) {
    pak_contents = (Pakfileentry_t *) malloc(pak_header.num_entries * sizeof(Pakfileentry_t)); 
    fseek(fp, pak_header.offset, SEEK_SET);

    for (int i = 0; i < pak_header.num_entries; i++) {
        fread(&pak_contents[i].filename, 56, 1, fp);
        fread(&pak_contents[i].offset, 4, 1, fp);
        fread(&pak_contents[i].length, 4, 1, fp);
    }
}

void extract_files(char *dest) {
    char *destfile, *destdir;
    unsigned char *buffer;
    FILE *tfd;
	int i;
	mode_t default_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    for (i = 0; i < pak_header.num_entries; i++) {
		Pakfileentry_t *file = &pak_contents[i];
        /* + 2 one for trailing null and the / we concat between basedir and pakfile path */ 
        destfile = malloc(sizeof(char) * (strlen(dest) + strlen(file->filename) + 2)); 
		*destfile = '\0';
        build_filename(dest, file->filename, destfile);

        destdir = dirname(destfile);
		
        if (! (file_exists(destdir)) && (mkdir_r(destdir, default_mode, default_mode) != 0)) {
			fprintf(stderr, "Cannot create directory %s: %s\n", destdir, strerror(errno));
			exit(1);
        }

        printf("Writing %d bytes to file %s\n", file->length, destfile);

        fseek(fp, file->offset, SEEK_SET);
        buffer = malloc(sizeof(unsigned char) * file->length); 
        fread(buffer, file->length, 1, fp);

        if (! (tfd = fopen(destfile, "w"))) {
            fprintf(stderr, "Cannot open %s for writing: %s", destfile, strerror(errno));
            exit(1);
        }

        if (fwrite(buffer, file->length, 1, tfd) == -1) {
            fprintf(stderr, "Cannot write to %s: %s", destfile, strerror(errno));
            exit(1);
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
