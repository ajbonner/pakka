#include "common.h"

#ifdef _WIN32
	#include "win/msdirent.h"
#endif

char *PKA_dirname(char *path) {
#ifdef _WIN32
	char *dir = malloc(sizeof(path));
	_splitpath(path, NULL, dir, NULL, NULL);
	return dir;
#else
	return dirname(path);
#endif
}

char *PKA_getcwd(char *buf, size_t size) {
#ifdef _WIN32
	return _getcwd(buf, size);
#else
	return getcwd(buf, size);
#endif
}

char *PKA_realpath(const char *path, char *resolved) {
#ifdef _WIN32
	return _fullpath(resolved, path, _MAX_PATH);
#else
	return realpath(path, resolved);
#endif
}

char *PKA_mkstemp(char *path) {
#ifdef _WIN32
	if (!_mktemp_s(path)) {
		error_exit("Cannot create temporary file \"%s\"", path);
	}
	return path;
#else 
	if (!mkstemp(path)) {
		error_exit("Cannot create temporary file \"%s\"", path);
	}
	return path;
#endif
}

int PKA_mkdir(const char *path, int mode) {
#ifdef _WIN32
	return mkdir(path);
#else 
	return mkdir(path, mode);
#endif
}

int file_exists(const char *filename) {
	struct stat sb;
	return stat(filename, &sb) == 0;
}

int mkdir_r(char *path) {
    struct stat sb;
    char *curpos = path;
    int mode = 0777;

    /* check if something already exists at path */
    if (stat(path, &sb) == 0) {
        if (S_ISDIR(sb.st_mode) == 0) {
            fprintf(stderr, "Could not create directory '%s': file exists but is not a directory\n", path);
            return -1;
        }

        return 0;
    }

    while (*curpos == PATH_SEPARATOR[0]) {
        curpos++;
    }

    while ((curpos = strchr(curpos, PATH_SEPARATOR[0]))) {
        *curpos = '\0';
        if (stat(path, &sb) != 0) {
            if (PKA_mkdir(path, mode)) {
                fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
                return -1;
            }
        } else if (S_ISDIR(sb.st_mode) == 0) {
            fprintf(stderr, "Could not create directory '%s': file exists but is not a directory\n", path);
            return -1;
        }
        
        *curpos++ = PATH_SEPARATOR[0];
        while (*curpos == PATH_SEPARATOR[0]) {
            curpos++;
        }
    }

    if (PKA_mkdir(path, mode)) {
        fprintf(stderr, "Could not create directory '%s': %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

int filesize(FILE *fd) {
    int size;

    fseek(fd, 0L, SEEK_END);
    size = ftell(fd);
    rewind(fd);

    return size;
}
