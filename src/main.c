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

static void listpakfiles(Pak_t *);
static void extract(Pak_t *, char *, char **, int);
static void usage(char **);
static void usage_banner(char **);
static void help(char **);
static int parseopts(int, char **, opts_t *);
static void setmodetype(opts_t *, short, char **);

int main(int argc, char *argv[]) {
    opts_t opts;
    parseopts(argc, argv, &opts);

    Pak_t *pak = open_pakfile(opts.pakfile);
    
    switch (opts.mode) {
        case PAK_LIST:
            listpakfiles(pak);
            break;
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
        usage(argv);
    }

    while ((c = getopt(argc, argv, "lxcarhf:d:")) != -1) {
        switch (c) {
            case 'l':
                setmodetype(opts, PAK_LIST, argv);
                break;
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

void usage_banner(char *argv[]) {
    fprintf(stderr, "%s %s (%s).\n", APP_NAME, VERSION, BUILD_DATE);
    fprintf(stderr, "Usage: %s -h [-lxcar] -f [pak file] [path(s)] -d [destination]\n", argv[0]);
}

void usage(char *argv[]) {
    usage_banner(argv);
    exit(1);
}

void help(char *argv[]) {
    usage_banner(argv);

    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -xf pak1.pak               # Extract pak1.pak to current dir\n", argv[0]);
    fprintf(stderr, "  %s -xf pak1.pak -d /some/path # Extract pak1.pak to /some/path\n", argv[0]);
    fprintf(stderr, "  # Extract models/weapons/g_blast/base.pcx from pak1.pak to current dir\n");
    fprintf(stderr, "  %s -xf pak1.pak models/weapons/g_blast/base.pcx \n", argv[0]);
    
    exit(1);
}
