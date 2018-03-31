#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
	#include "win/wingetopt-0.95/src/getopt.h"
	#include "win/msdirent.h"
	#define realpath(N,R) _fullpath((R),(N),_MAX_PATH)
	#define dirname(P) _dirname(P)
	#define mkstemp(P) mktemp(P)
	#define getcwd(D,S) _getcwd(D,S)
	#define PATH_SEPARATOR "\\"
#else 
	#include <unistd.h>
	#include <dirent.h>
	#include <libgen.h>
	#define PATH_SEPARATOR "/"
#endif
#include "options.h"
#include <limits.h>
#include "filesystem.h"

#define PAK_EXTRACT 1
#define PAK_CREATE  2
#define PAK_ADD     4
#define PAK_REMOVE  8
#define PAK_LIST    16

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

char *_dirname(char *);
void error_exit(const char *, ...);
Pak_t *open_pakfile(const char *);
Pak_t *create_pakfile(const char *);
int close_pakfile(Pak_t *);
void list_files(Pak_t *);
void extract_files(Pak_t *, char *);
void delete_files(Pak_t *, char* [] , int);
void debug_header(Pak_t *);
void debug_directory_entry(Pakfileentry_t *);

int add_file(Pak_t *, char *);
int add_to_pak(Pak_t *, char *);
int add_folder(Pak_t *, char *);
int add_files(Pak_t *, char **, int);

void usage();
void usage_banner();
void help();
