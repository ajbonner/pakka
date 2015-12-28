#include "header.h"

int file_exists(const char *filename) {
	struct stat sb;
	return stat(filename, &sb) == 0;
}

/*
 * The contents of this function are a minor modification of a bash 4.1
 * recursive mkdir implementation see ftp://ftp.cwru.edu/pub/bash/bash-4.1.tar.gz
 * (examples/loadables/mkdir.c)
 */
int mkdir_r(char *path, int nmode, int parent_mode) {
	int oumask;
	struct stat sb;
	char *p, *npath;

	if (stat(path, &sb) == 0) {
		if (S_ISDIR(sb.st_mode) == 0) {
			fprintf(stderr, "`%s': file exists but is not a directory", path);
			return 1;
		}

		if (chmod(path, nmode)) {
			fprintf(stderr, "%s: %s", path, strerror(errno));
			return 1;
		}

		return 0;
	}

	oumask = umask(0);
	npath = malloc(sizeof(char) * strlen(path));    /* So we can write to it. */
	strcpy(npath, path);

	/* Check whether or not we need to do anything with intermediate dirs. */

	/* Skip leading slashes. */
	p = npath;
	while (*p == '/') {
		p++;
	}

	while ((p = strchr(p, '/'))) {
		*p = '\0';
		if (stat(npath, &sb) != 0) {
			if (mkdir(npath, parent_mode)) {
				fprintf(stderr, "cannot create directory `%s': %s", npath, strerror(errno));
				umask(oumask);
				free(npath);
				return 1;
			}
		} else if (S_ISDIR(sb.st_mode) == 0) {
			fprintf(stderr, "`%s': file exists but is not a directory", npath);
			umask(oumask);
			free(npath);
			return 1;
		}

		*p++ = '/';   /* restore slash */
		while (*p == '/') {
			p++;
		}
	}

	/* Create the final directory component. */
	if (stat(npath, &sb) && mkdir(npath, nmode)) {
		fprintf(stderr, "cannot create directory `%s': %s", npath, strerror (errno));
		umask(oumask);
		free(npath);
		return 1;
	}

	umask(oumask);
	free(npath);
	return 0;
}
