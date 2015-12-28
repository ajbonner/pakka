#include "header.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pathtopakfile>\n", argv[0]);
        exit(1);
    }

    if (! (fp = fopen(argv[1], "r"))) {
        fprintf(stderr, "Cannot open '%s': %s\n", argv[1], strerror(errno));
        exit(1);
    }

    load_pakfile(argv[1]);

    /*printf("%s\n", pak_header.signature);*/
    /*printf("%u\n", pak_header.offset);*/
    /*printf("%u\n", pak_header.length);*/
    /*printf("%d\n", pak_header.num_entries);*/

    load_directory();

    /*for (int i = 0; i < pak_header.num_entries; i++) {*/
        /*printf("%s\n", pak_contents[i].filename); */
    /*}*/

    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd() error");
    }

    char dest[4096] = "\0";

    strcat(dest, cwd);
    strcat(dest, "/tmp");
    
    extract_files(dest);

    fclose(fp);
}
