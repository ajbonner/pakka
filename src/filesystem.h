#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "compat.h"

int pakka_file_exists(const char *dirname);
int pakka_mkdir_r(char *path);
int64_t pakka_filesize(FILE *fd);
