#include "common.h"

#define PAK_EXTRACT 1
#define PAK_CREATE  2
#define PAK_ADD     4
#define PAK_REMOVE  8
#define PAK_LIST    16

typedef struct opts_s {
    short mode;
    char *pakfile;
    char *destination;
    char **paths;
    int path_count;
} opts_t;

char *name;
static void listpakfiles(Pak_t *);
static void extract(Pak_t *, char *, char **, int);
static void usage();
static void usage_banner();
static void help();
static int parseopts(int, char **, opts_t *);
static void setmodetype(opts_t *, short);

int main(int argc, char *argv[]) {
    name = argv[0];
    opts_t opts;
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

int parseopts(int argc, char* argv[], opts_t *opts) {
    int c;
    
    if (argc < 2) {
        usage();
    }

    while ((c = getopt(argc, argv, "lxcarhf:d:")) != -1) {
        switch (c) {
            case 'l':
                setmodetype(opts, PAK_LIST);
                break;
            case 'x':
                setmodetype(opts, PAK_EXTRACT);
                break;
            case 'c':
                setmodetype(opts, PAK_CREATE);
                break;
            case 'a':
                setmodetype(opts, PAK_ADD);
                break;
            case 'r':
                setmodetype(opts, PAK_REMOVE);
                break;
            case 'f':
                opts->pakfile = optarg;
                break;
            case 'd':
                opts->destination = optarg;
                break;
            case 'h':
                help();
                break;
        }
    }
    
    if (optind < argc) {
         opts->paths = (char **) malloc(sizeof(char *) * (argc - optind));
        while (optind < argc) {
            opts->paths[opts->path_count++] = argv[optind++];
        }
    }

    if (! opts->mode) {
        fprintf(stderr, "You must specify one -lxcar option\n");
        usage();
    }

    if (! opts->pakfile) {
        fprintf(stderr, "You must specify a pakfile name with -f\n");
        usage();
    }

    return 0;
}

void setmodetype(opts_t *opt, short mode) {
    if (! opt->mode) {
        opt->mode = mode;
    } else {
        fprintf(stderr, "You may not specify more than one -lxcar option\n");
        usage();
    }
}

void usage_banner() {
    fprintf(stderr, "%s %s (%s).\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h [-lxcar] -f [pak file] [path(s)] -d [destination]\n", name);
}

void usage() {
    usage_banner();
    exit(1);
}

void help() {
    usage_banner();

    fprintf(stderr, "\nOperation Modes:\n");
    fprintf(stderr, "You must specify one [lxcar] option\n");
    fprintf(stderr, " -l    list files contained in pak file\n");
    fprintf(stderr, " -x    extract files from pak file\n");
    fprintf(stderr, " -c    create a new pak file\n");
    fprintf(stderr, " -a    add files to pak file\n");
    fprintf(stderr, " -r    remove files from pak file\n");
    fprintf(stderr, " -h    this help\n");

    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -lf pak1.pak               # List contents of pak1.pak\n", name);
    fprintf(stderr, "  %s -xf pak1.pak               # Extract pak1.pak to current dir\n", name);
    fprintf(stderr, "  %s -xf pak1.pak -d /some/path # Extract pak1.pak to /some/path\n", name);
    fprintf(stderr, "  # Extract models/weapons/g_blast/base.pcx from pak1.pak to current dir\n");
    fprintf(stderr, "  %s -xf pak1.pak models/weapons/g_blast/base.pcx \n", name);
    
    exit(1);
}
