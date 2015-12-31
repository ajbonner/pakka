#include "common.h"

#define PAK_EXTRACT 1
#define PAK_CREATE  2
#define PAK_ADD     4
#define PAK_REMOVE  8

typedef struct opts_s {
    short mode;
    char *pakfile;
    char *destination;
    char **paths;
    int path_count;
} opts_t;

static void extract(Pak_t *, char *, char **, int);
static void usage(char **);
static void help(char **);
static int parseopts(int, char **, opts_t *);
static void setmodetype(opts_t *, short, char **);

int main(int argc, char *argv[]) {
    opts_t opts;
    parseopts(argc, argv, &opts);

    Pak_t *pak = open_pakfile(opts.pakfile);
    Pakfileentry_t *e = pak->head;
    
    do {
        printf("%s\n", e->filename);
    } while ((e = e->next) != NULL);

    switch (opts.mode) {
        case PAK_EXTRACT:
            extract(pak, opts.destination, opts.paths, opts.path_count);
            break;
        case PAK_ADD:
            break;
        case PAK_REMOVE:
            break;
        case PAK_CREATE:
            break;
        default:
            fprintf(stderr, "Unknown operation mode selected\n");
            exit(1);
    }

    close_pakfile(pak);
    free(opts.paths);

    return 0;
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
        usage(argv);
    }

    while ((c = getopt(argc, argv, "xcarhf:d:")) != -1) {
        switch (c) {
            case 'x':
                setmodetype(opts, PAK_EXTRACT, argv);
                break;
            case 'c':
                setmodetype(opts, PAK_CREATE, argv);
                break;
            case 'a':
                setmodetype(opts, PAK_ADD, argv);
                break;
            case 'r':
                setmodetype(opts, PAK_REMOVE, argv);
                break;
            case 'f':
                opts->pakfile = optarg;
                break;
            case 'd':
                opts->destination = optarg;
                break;
            case 'h':
                help(argv);
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
        fprintf(stderr, "You must specify one -xcar option\n");
        usage(argv);
    }

    if (! opts->pakfile) {
        fprintf(stderr, "You must specify a pakfile name with -f\n");
        usage(argv);
    }

    return 0;
}

void setmodetype(opts_t *opt, short mode, char* argv[]) {
    if (! opt->mode) {
        opt->mode = mode;
    } else {
        fprintf(stderr, "You may not specify more than one -xcar option\n");
        usage(argv);
    }
}

void usage(char *argv[]) {
    fprintf(stderr, "%s %s (%s). Usage:\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h [-xcar] -f [pak file] [path(s)] -d [destination]\n", argv[0]);
    exit(1);
}

void help(char *argv[]) {
    fprintf(stderr, "%s %s (%s). Usage:\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h [-xcar] -f [pak file] [path(s)] -d [destination]\n", argv[0]);

    fprintf(stderr, "Examples\n");
    fprintf(stderr, "Extract contents of pak1.pak to current working path\n");
    fprintf(stderr, "%s -x -f pak1.pak\n\n", argv[0]);

    fprintf(stderr, "Extract contents of pak1.pak to specified path\n");
    fprintf(stderr, "%s -x -f pak1.pak -d /some/path\n\n", argv[0]);

    fprintf(stderr, "Extract file models/weapons/g_blast/base.pcx from pak1.pak to current working path\n");
    fprintf(stderr, "%s -x -f pak1.pak models/weapons/g_blast/base.pcx\n\n", argv[0]);
    
    exit(1);
}
