#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "filesystem.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX
#define PAKFILE_PATH_MAX 56
#define PAKFILE_DIR_ENTRY_SIZE 64
#define PAKFILE_HEADER_SIZE 12 

typedef struct Pakfileentry_s {
    char filename[PAKFILE_PATH_MAX];
    unsigned int offset;
    unsigned int length;
    struct Pakfileentry_s *next;
} Pakfileentry_t;

typedef struct {
    char signature[4];
    unsigned int diroffset;
    unsigned int dirlength;
    unsigned int num_entries;
    Pakfileentry_t *head;
} Pak_t;

void error_exit(const char *, ...);
Pak_t *open_pakfile(const char *);
Pak_t *create_pakfile(const char *);
int close_pakfile(Pak_t *);
void list_files(Pak_t *);
void extract_files(Pak_t *, char *);

int add_file(Pak_t *, char *);
int add_to_pak(Pak_t *, char *);
int add_folder(Pak_t *, char *);
int add_files(Pak_t *, char **, int);
