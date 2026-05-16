#include "common.h"

void debug_directory_entry(Pakfileentry_t *entry) {
    if (entry == NULL) { fprintf(stderr, "Null pak entry\n"); return; }

    fprintf(stderr, "\n");
    fprintf(stderr, "Pak Directory Entry\n");
    fprintf(stderr, "Filename: ");
    fprint_sanitized(stderr, entry->filename);
    fputc('\n', stderr);
    fprintf(stderr, "Position Offset: 0x%08" PRIx32 "\n", entry->offset);
    fprintf(stderr, "Length (bytes) %" PRIu32 "\n", entry->length);
    fprintf(stderr, "Next Sibling: %p\n", (void *)entry->next);
}

void debug_header(Pak_t *pak) {
    if (pak == NULL) { fprintf(stderr, "Null pak file\n"); return; }

    fprintf(stderr, "\n");
    fprintf(stderr, "Pak File Header\n");
    fprintf(stderr, "Signature: ");
    fwrite(pak->signature, PAKFILE_SIGNATURE_LEN, 1, stderr);
    fprintf(stderr, "\n");
    fprintf(stderr, "Directory Offset: 0x%08" PRIx32 "\n", pak->diroffset);
    fprintf(stderr, "Directory Length (bytes): %" PRIu32 "\n", pak->dirlength);
}
