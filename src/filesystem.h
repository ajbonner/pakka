#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "compat.h"

int file_exists(const char *dirname);
int mkdir_r(char *path);
int64_t filesize(FILE *fd);
