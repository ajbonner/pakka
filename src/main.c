#include "common.h"

static int parseopts(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    parseopts(argc, argv);

    Pak_t *pak = open_pakfile(argv[1]);

    /*if (opt.list) {*/
    for (int i = 0; i < pak->num_entries; i++) {
        printf("%s\n", pak->files[i].filename); 
    }
    /*}*/

    /*if (opt.extract) {*/
    /*char cwd[OS_PATH_MAX];*/
    /*if (getcwd(cwd, sizeof(cwd)) == NULL) {*/
        /*error_exit("Cannot get current working directory");*/
    /*}*/

    /*char dest[OS_PATH_MAX] = "\0";*/
    /*strcat(dest, cwd);*/
    /*strcat(dest, "/tmp");*/
    
    /*extract_files(dest);*/
    /*}*/

    return close_pakfile(pak);
}

int parseopts(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "%s %s (%s). Usage:\n", APP_NAME, VERSION, BUILD_DATE);
        fprintf(stderr, "Usage: %s [options] [pakfile]\n", argv[0]);
        exit(1);
    }

    return 0;
}
