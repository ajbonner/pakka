#include "common.h"

void debug_directory_entry(Pakfileentry_t *entry) {
    if (entry == NULL) { fprintf(stderr, "Null pak entry\n"); return; }

    fprintf(stderr, "\n");
    fprintf(stderr, "Pak Directory Entry\n");
    fprintf(stderr, "Filename: %s\n", entry->filename);
    fprintf(stderr, "Position Offset: 0x%08x\n", entry->offset);
    fprintf(stderr, "Length (bytes) %u\n", entry->length);
    fprintf(stderr, "Next Sibling: %p\n", (void *) entry->next);
}

void debug_header(Pak_t *pak) {
    if (pak == NULL) { fprintf(stderr, "Null pak file\n"); return; }

    fprintf(stderr, "\n");
    fprintf(stderr, "Pak File Header\n");
    fprintf(stderr, "Signature: ");
    fwrite(pak->signature, 4, 1, stderr);
    fprintf(stderr, "\n");
    fprintf(stderr, "Directory Offset: 0x%08x\n", pak->diroffset);
    fprintf(stderr, "Directory Length (bytes): %d\n", pak->dirlength);
}
