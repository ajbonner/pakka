#include "common.h"

char *name;
void listpakfiles(Pak_t *);
void extract(Pak_t *, char *, char **, int);
void usage();
void usage_banner();
void help();

int main(int argc, char* argv[]) {
    name = argv[0];
	opts_t opts = { 0, NULL, NULL, NULL, 0 };
    parseopts(argc, argv, &opts);
    Pak_t *pak;
    
    if (opts.mode == PAK_CREATE) {
        pak = create_pakfile(opts.pakfile);
    } else {
        pak = open_pakfile(opts.pakfile);
    }
    
    switch (opts.mode) {
        case PAK_LIST:
            listpakfiles(pak);
            break;
        case PAK_EXTRACT:
            extract(pak, opts.destination, opts.paths, opts.path_count);
            break;
        case PAK_ADD:
        case PAK_CREATE:
            add_files(pak, opts.paths, opts.path_count);
            break;
        case PAK_REMOVE:
            delete_files(pak, opts.paths, opts.path_count);
            break;
        default:
            fprintf(stderr, "Unknown operation mode selected\n");
            exit(1);
    }

    close_pakfile(pak);
    free(opts.paths);

    return 0;
}

void listpakfiles(Pak_t *pak) {
    list_files(pak);
}

void extract(Pak_t *pak, char *destination, char **paths, int path_count) {
    char *realdest = malloc(sizeof(char) * OS_PATH_MAX);

    if (destination != NULL) {
        if (realpath(destination, realdest) == NULL) {
           error_exit("Cannot open destination path '%s'", destination);
        }
	} else {
		if (getcwd(realdest, sizeof(realdest) * OS_PATH_MAX) == NULL) {
			error_exit("Cannot get current working directory");
		}
	}
	
    extract_files(pak, realdest);
    free(realdest);
}

void usage_banner() {
    fprintf(stderr, "%s %s (%s).\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h [-lxcad] -f [pak file] -C [destination path] [path(s)]\n", name);
}

void usage() {
    usage_banner();
    exit(1);
}

void help() {
    usage_banner();

    fprintf(stderr, "\nOperation Modes:\n");
    fprintf(stderr, "You must specify one [lxcad] option\n");
    fprintf(stderr, " -l    list files contained in pak file\n");
    fprintf(stderr, " -x    extract files from pak file\n");
    fprintf(stderr, " -c    create a new pak file\n");
    fprintf(stderr, " -a    add files to pak file\n");
    fprintf(stderr, " -d    remove files from pak file\n");

    fprintf(stderr, " -h    this help\n");

    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -lf pak1.pak               # List contents of pak1.pak\n", name);
    fprintf(stderr, "  %s -xf pak1.pak               # Extract pak1.pak to current dir\n", name);
    fprintf(stderr, "  %s -xf pak1.pak -C /some/path # Extract pak1.pak to /some/path\n", name);
    fprintf(stderr, "  # Extract models/weapons/g_blast/base.pcx from pak1.pak to current dir\n");
    fprintf(stderr, "  %s -xf pak1.pak models/weapons/g_blast/base.pcx\n", name);
    
    exit(1);
}
