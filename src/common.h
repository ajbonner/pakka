#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "filesystem.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX

typedef struct Pakfileentry_s {
    char filename[56];
    unsigned int offset;
    unsigned int length;
} Pakfileentry_t;

typedef struct {
    unsigned char signature[5];
    unsigned int diroffset;
    unsigned int dirlength;
    unsigned int num_entries;
    Pakfileentry_t *files;
} Pak_t;

void error_exit(const char *, ...);
Pak_t* open_pakfile(const char *);
int close_pakfile(Pak_t *);
void extract_files(char *);

FILE *fp;
Pak_t pak;
Pakfileentry_t *pak_contents;
