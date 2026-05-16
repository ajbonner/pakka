#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "compat.h"
#include "filesystem.h"

#define OS_PATH_MAX PATH_MAX
#define OS_NAME_MAX NAME_MAX
#define PAKFILE_SIGNATURE_LEN 4
/* On-disk filename field is 56 bytes. The in-memory buffer is one byte
 * larger so we can force a NUL terminator after fread() — names that fill
 * all 56 bytes on disk would otherwise overread into adjacent struct
 * fields when handed to strlen/strcmp/printf("%s"). */
#define PAKFILE_PATH_MAX 56
#define PAKFILE_PATH_BUF 57
#define PAKFILE_DIR_ENTRY_SIZE 64
#define PAKFILE_HEADER_SIZE 12
/* Sanity cap on directory entry count. id's pak0.pak has 339 entries;
 * a million is generous and prevents calloc-loop DoS on crafted headers. */
#define PAKFILE_MAX_ENTRIES 1048576u
/* Buffer size for streaming file copies during add. Bounds peak RSS at
 * 64 KiB regardless of input file size. */
#define PAKFILE_COPY_CHUNK 65536u

typedef struct Pakfileentry_s {
    char filename[PAKFILE_PATH_BUF];
    uint32_t offset;
    uint32_t length;
    struct Pakfileentry_s *next;
} Pakfileentry_t;

typedef struct {
    char signature[PAKFILE_SIGNATURE_LEN];
    uint32_t diroffset;
    uint32_t dirlength;
    uint32_t num_entries;
    uint64_t file_size;
    Pakfileentry_t *head;
} Pak_t;

void error_exit(const char *format, ...);
Pak_t *open_pakfile(const char *pakpath);
Pak_t *create_pakfile(const char *pakpath);
int close_pakfile(Pak_t *pak);
void list_files(Pak_t *pak);
void list_files_tree(Pak_t *pak);
void extract_files(Pak_t *pak, char *dest, char **paths, int path_count);
void delete_files(Pak_t *pak, char **paths, int path_count);
void debug_header(Pak_t *pak);
void debug_directory_entry(Pakfileentry_t *entry);

int add_file(Pak_t *pak, char *path);
int add_to_pak(Pak_t *pak, char *path);
int add_folder(Pak_t *pak, char *path);
int add_files(Pak_t *pak, char **paths, int path_count);
