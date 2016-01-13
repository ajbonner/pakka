#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>

int file_exists(const char *dirname);
int mkdir_r(char *path);
