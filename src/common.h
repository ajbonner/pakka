#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "filesystem.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX
#define PAKFILE_SIGNATURE_LEN 4
#define PAKFILE_PATH_MAX 56
#define PAKFILE_DIR_ENTRY_SIZE 64
#define PAKFILE_HEADER_SIZE 12

typedef struct Pakfileentry_s {
    char filename[PAKFILE_PATH_MAX];
    uint32_t offset;
    uint32_t length;
    struct Pakfileentry_s *next;
} Pakfileentry_t;

typedef struct {
    char signature[PAKFILE_SIGNATURE_LEN];
    uint32_t diroffset;
    uint32_t dirlength;
    uint32_t num_entries;
    Pakfileentry_t *head;
} Pak_t;

void error_exit(const char *format, ...);
Pak_t *open_pakfile(const char *pakpath);
Pak_t *create_pakfile(const char *pakpath);
int close_pakfile(Pak_t *pak);
void list_files(Pak_t *pak);
void extract_files(Pak_t *pak, char *dest, char **paths, int path_count);
void delete_files(Pak_t *pak, char **paths, int path_count);
void debug_header(Pak_t *pak);
void debug_directory_entry(Pakfileentry_t *entry);

int add_file(Pak_t *pak, char *path);
int add_to_pak(Pak_t *pak, char *path);
int add_folder(Pak_t *pak, char *path);
int add_files(Pak_t *pak, char **paths, int path_count);
