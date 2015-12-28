#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

void load_pakfile(char *);
void load_directory();
void extract_files(char *dest);

int file_exists(const char *dirname);
int mkdir_r(char *path, int rmove, int parent_mode);

typedef struct {
    unsigned char signature[5];
    unsigned int offset;
    unsigned int length;
    unsigned int num_entries;
} Header_t;

typedef struct Pakfileentry_s {
    char filename[56];
    unsigned int offset;
    unsigned int length;
} Pakfileentry_t;

FILE *fp;
Header_t pak_header;
Pakfileentry_t *pak_contents;
