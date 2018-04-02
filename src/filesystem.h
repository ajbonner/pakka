#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEPARATOR "\\"
#else
#include <libgen.h>
#define PATH_SEPARATOR "/"
#endif

char *PKA_dirname(char *);
char *PKA_getcwd(char *, size_t);
char *PKA_realpath(const char *, char *);
char *PKA_mkstemp(char *);
int PKA_mkdir(const char *, int);

int file_exists(const char *);
int mkdir_r(char *);
int filesize (FILE *);
