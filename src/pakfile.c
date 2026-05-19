#include <stdarg.h>

#include "common.h"
#include "pakka.h"

/* err_fill: write the status, optional system error, operation, and a
 * formatted message into err. Tolerates err == NULL — every public
 * function must work when the caller passes NULL for err. Returns the
 * status passed in so callsites can
 * `return err_fill(err, PAKKA_ERR_FORMAT, ...);` directly. */
static pakka_status_t err_fill(pakka_error_t *err,
                               pakka_status_t status,
                               pakka_error_domain_t domain,
                               uint32_t system_code,
                               const char *operation,
                               const char *fmt, ...) {
    int written;
    size_t op_len;
    va_list args;

    if (err == NULL) {
        return status;
    }

    err->status = status;
    err->domain = domain;
    err->system_code = system_code;
    err->entry_name[0] = '\0';
    err->entry_name_truncated = 0;
    err->entry_index = (size_t)-1;
    err->offset = 0;
    err->length = 0;
    err->message_truncated = 0;

    if (operation == NULL) {
        err->operation[0] = '\0';
    } else {
        op_len = strlen(operation);
        if (op_len >= PAKKA_OPERATION_SIZE) {
            op_len = PAKKA_OPERATION_SIZE - 1;
        }
        memcpy(err->operation, operation, op_len);
        err->operation[op_len] = '\0';
    }

    if (fmt == NULL) {
        err->message[0] = '\0';
    } else {
        va_start(args, fmt);
        written = vsnprintf(err->message, PAKKA_MESSAGE_SIZE, fmt, args);
        va_end(args);
        if (written < 0) {
            err->message[0] = '\0';
        } else if ((size_t)written >= PAKKA_MESSAGE_SIZE) {
            err->message_truncated = 1;
        }
    }

    return status;
}

/* err_set_entry: layer entry-specific context onto an existing err
 * (entry name, directory index, file offset, declared length). Call
 * after err_fill so callers see a fully-populated error. */
static void err_set_entry(pakka_error_t *err,
                          const char *entry_name,
                          size_t entry_index,
                          uint64_t offset,
                          uint64_t length) {
    size_t n;

    if (err == NULL) {
        return;
    }

    err->entry_index = entry_index;
    err->offset = offset;
    err->length = length;

    if (entry_name == NULL) {
        err->entry_name[0] = '\0';
        err->entry_name_truncated = 0;
        return;
    }

    n = strlen(entry_name);
    if (n >= PAKKA_ENTRY_NAME_SIZE) {
        n = PAKKA_ENTRY_NAME_SIZE - 1;
        err->entry_name_truncated = 1;
    } else {
        err->entry_name_truncated = 0;
    }
    memcpy(err->entry_name, entry_name, n);
    err->entry_name[n] = '\0';
}

static pakka_status_t load_pakfile(Pak_t *pak, pakka_error_t *err);
static pakka_status_t load_directory(Pak_t *pak, pakka_error_t *err);
static uint64_t compute_payload_end(Pak_t *pak);
static Pakfileentry_t *find_tail(Pak_t *pak);
static void init_pak_header(Pak_t *pak);
static pakka_status_t write_pak_directory(Pak_t *pak, pakka_error_t *err);
static Pakfileentry_t *find_entry(Pak_t *pak, char *path);
static pakka_status_t copy_between_paks(Pakfileentry_t *entry,
                                        FILE *ffd, FILE *tfd,
                                        uint32_t *new_offset_out,
                                        pakka_error_t *err);

/* qsort comparator for an array of `const char *` (which qsort passes
 * as pointers-to-pointers). */
static int name_ptr_cmp(const void *a, const void *b) {
    const char *const *aa = (const char *const *)a;
    const char *const *bb = (const char *const *)b;
    return strcmp(*aa, *bb);
}

/* Used on the error paths of pakka_open and pakka_create, where the
 * Pak_t may have a live fp (fopen succeeded, validation later failed)
 * and a partial entry list. Distinct from pakka_close, which assumes
 * a fully-constructed archive. */
static void destroy_pak(Pak_t *pak) {
    Pakfileentry_t *e;
    Pakfileentry_t *next;
    if (pak == NULL) {
        return;
    }
    if (pak->fp != NULL) {
        fclose(pak->fp);
        pak->fp = NULL;
    }
    e = pak->head;
    while (e != NULL) {
        next = e->next;
        free(e);
        e = next;
    }
    free(pak);
}

/* Open-time duplicate-name validation. Builds a temporary sorted array
 * of name pointers and scans for adjacent equals. O(n log n); the array
 * is freed before pakka_open returns. */
static pakka_status_t validate_no_duplicates(Pak_t *pak, pakka_error_t *err) {
    Pakfileentry_t *entry;
    const char **names;
    size_t i = 0;
    size_t j;

    if (pak->num_entries <= 1) {
        return PAKKA_OK;
    }

    names = malloc(sizeof(const char *) * pak->num_entries);
    if (names == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Cannot allocate duplicate-name scratch for %" PRIu32
                        " entries", pak->num_entries);
    }

    for (entry = pak->head; entry != NULL && i < pak->num_entries;
         entry = entry->next) {
        names[i++] = entry->filename;
    }

    /* qsort instead of insertion sort: PAKFILE_MAX_ENTRIES = 1,048,576
     * makes O(n^2) untenable for hostile inputs. The comparator wraps
     * strcmp so we work on `const char **` rather than `char *`. */
    qsort(names, i, sizeof(const char *), name_ptr_cmp);

    for (j = 1; j < i; j++) {
        if (strcmp(names[j - 1], names[j]) == 0) {
            err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                     "open",
                     "Pak contains duplicate entry name");
            err_set_entry(err, names[j], (size_t)-1, 0, 0);
            free((void *)names);
            return PAKKA_ERR_DUPLICATE;
        }
    }

    free((void *)names);
    return PAKKA_OK;
}

pakka_status_t pakka_open(const char *path, pakka_open_mode_t mode,
                          pakka_archive_t **out, pakka_error_t *err) {
    Pak_t *pak;
    size_t path_len;
    int writable;
    pakka_status_t status;
    int saved_errno;

    if (out != NULL) {
        *out = NULL;
    }
    if (path == NULL || out == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                        "pakka_open: path and out must be non-NULL");
    }

    if (mode != PAKKA_OPEN_READ && mode != PAKKA_OPEN_READ_WRITE) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                        "pakka_open: unknown mode value %d", (int)mode);
    }
    writable = (mode == PAKKA_OPEN_READ_WRITE) ? 1 : 0;

    pak = calloc(sizeof(Pak_t), 1);
    if (pak == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Cannot allocate pakka_archive_t");
    }

    path_len = strlen(path);
    if (path_len >= sizeof(pak->cur_pakpath)) {
        free(pak);
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Pak path too long: %s", path);
    }
    memcpy(pak->cur_pakpath, path, path_len + 1);

    if (! (pak->fp = fopen(path, writable ? "r+b" : "rb"))) {
        saved_errno = errno;
        free(pak);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot open %s", path);
    }
    pak->writable = writable;

    /* Hold an exclusive non-blocking lock for mutating opens. Two
     * concurrent `pakka -af` invocations would otherwise both seek to
     * the same byte-tail, write payloads on top of each other, and
     * race the directory rewrite. The lock is released automatically
     * when pak->fp is closed. */
    if (writable && pakka_compat_try_exclusive_lock(pak->fp) != 0) {
        saved_errno = errno;
        destroy_pak(pak);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Could not lock %s for writing "
                        "(another pakka process may be modifying it)",
                        path);
    }

    status = load_pakfile(pak, err);
    if (status != PAKKA_OK) {
        destroy_pak(pak);
        return status;
    }

    /* PK3 path: load_pakfile dispatched into pakka_pk3_open_impl which
     * already parsed the CDR. The PAK on-disk directory loader doesn't
     * apply. Validate-no-duplicates still runs — it operates on the
     * in-memory entry list so it's format-agnostic. */
    if (pak->format != PAKKA_FORMAT_PK3) {
        status = load_directory(pak, err);
        if (status != PAKKA_OK) {
            destroy_pak(pak);
            return status;
        }
    }

    status = validate_no_duplicates(pak, err);
    if (status != PAKKA_OK) {
        destroy_pak(pak);
        return status;
    }

    *out = pak;
    return PAKKA_OK;
}

pakka_status_t pakka_create(const char *path, pakka_format_t format,
                            unsigned flags,
                            pakka_archive_t **out, pakka_error_t *err) {
    Pak_t *pak;
    struct stat sb;
    size_t path_len;
    int saved_errno;

    if (out != NULL) {
        *out = NULL;
    }
    if (path == NULL || out == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "create",
                        "pakka_create: path and out must be non-NULL");
    }

    if (flags != PAKKA_CREATE_DEFAULT) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "create",
                        "pakka_create: unknown flag bits 0x%x", flags);
    }

    if (format != PAKKA_FORMAT_PAK
        && format != PAKKA_FORMAT_PK3
        && format != PAKKA_FORMAT_AUTO) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "create",
                        "pakka_create: unknown format %d", (int)format);
    }

    pak = calloc(sizeof(Pak_t), 1);
    if (pak == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "create", "Cannot allocate pakka_archive_t");
    }

    path_len = strlen(path);
    if (path_len >= sizeof(pak->new_pakpath)) {
        free(pak);
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "create", "Pak path too long: %s", path);
    }

    pak->is_new = 1;
    pak->writable = 1;
    memcpy(pak->new_pakpath, path, path_len + 1);

    if (stat(path, &sb) == 0) {
        free(pak);
        return err_fill(err, PAKKA_ERR_EXISTS, PAKKA_ERR_DOMAIN_NONE, 0,
                        "create",
                        "File already exists at destination %s", path);
    }

    /* L_tmpnam on MSVC is 20 bytes — not enough for "C:\Users\...\Temp\
     * pakkaXXXXXX" — hence the dedicated OS_PATH_MAX buffer on Pak_t. */
    if (! (pak->fp = pakka_compat_mkstemp_open(path, "pakkaXXXXXX",
                                         pak->tmp_pakpath,
                                         sizeof(pak->tmp_pakpath)))) {
        saved_errno = errno;
        free(pak);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "create",
                        "Cannot create temp pak file for %s", path);
    }

    if (format == PAKKA_FORMAT_PK3) {
        pakka_status_t pk3_status = pakka_pk3_create_impl(pak, err);
        if (pk3_status != PAKKA_OK) {
            destroy_pak(pak);
            return pk3_status;
        }
        *out = pak;
        return PAKKA_OK;
    }

    /* PAK (default) path. */
    pak->format = PAKKA_FORMAT_PAK;
    init_pak_header(pak);

    /* Write a valid empty PACK header now so pakka_create + pakka_close
     * with no payload produces a well-formed 12-byte pak. A subsequent
     * commit overwrites this header on disk. */
    if (fwrite(pak->signature, PAKFILE_SIGNATURE_LEN, 1, pak->fp) != 1
        || pakka_write_u32_le(pak->fp, pak->diroffset) != 0
        || pakka_write_u32_le(pak->fp, pak->dirlength) != 0) {
        saved_errno = errno;
        destroy_pak(pak);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "create",
                        "Cannot write initial pak header to %s", path);
    }

    *out = pak;
    return PAKKA_OK;
}

pakka_status_t pakka_close(pakka_archive_t *pak, pakka_error_t *err) {
    Pakfileentry_t *e;
    Pakfileentry_t *next;
    int close_rc = 0;
    int saved_errno;
    pakka_status_t status = PAKKA_OK;

    if (pak == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "close",
                        "pakka_close: archive must be non-NULL");
    }

    /* Implicit commit so pakka_add_file / pakka_delete without an
     * explicit pakka_commit still get persisted. A commit failure here
     * is sticky: status is set so we still proceed with fclose/free
     * (the handle must be released), but we MUST skip the
     * is_new → new_pakpath rename so a broken create doesn't get
     * published. */
    if (pak->dirty) {
        pakka_status_t commit_s = pakka_commit(pak, err);
        if (commit_s != PAKKA_OK) {
            status = commit_s;
        }
    }

    /* fclose BEFORE rename and BEFORE free(pak): CRT fopen on Windows
     * does not request FILE_SHARE_DELETE, so MoveFileEx would otherwise
     * be blocked by the still-open handle. delete_entries may have
     * already closed pak->fp on its own (it sets pak->fp = NULL), so
     * tolerate that.
     *
     * Capture the close return: when pak->is_new=1, pak->fp IS the temp
     * pak about to be renamed into place, and buffered write errors
     * (disk full, NFS, quota) surface here, not on fwrite. Renaming a
     * half-flushed temp over a non-existent destination would leave a
     * corrupt pak. */
    if (pak->fp != NULL) {
        close_rc = fclose(pak->fp);
        saved_errno = errno;
        pak->fp = NULL;
    } else {
        saved_errno = 0;
    }

    if (pak->is_new && status == PAKKA_OK) {
        if (close_rc != 0) {
            err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                     (uint32_t)saved_errno, "close",
                     "Error finalizing pak %s (disk full or I/O error)",
                     pak->new_pakpath);
            status = PAKKA_ERR_IO;
        } else {
            uint32_t win32_code = 0;
            int rrc = pakka_compat_rename_noreplace(pak->tmp_pakpath,
                                              pak->new_pakpath,
                                              &win32_code);
            if (rrc != 0) {
                /* Differentiate "destination already exists" from
                 * generic I/O failure so callers can react. On POSIX
                 * errno carries the answer (link()+unlink() under
                 * the hood). On Windows pakka_compat_rename_noreplace
                 * captures GetLastError into win32_code before
                 * returning. */
                saved_errno = errno;
#ifdef _WIN32
                if (win32_code == 0xB7u /* ERROR_ALREADY_EXISTS */) {
                    err_fill(err, PAKKA_ERR_EXISTS,
                             PAKKA_ERR_DOMAIN_WIN32, win32_code, "close",
                             "Could not create pak %s "
                             "(destination already exists)",
                             pak->new_pakpath);
                    status = PAKKA_ERR_EXISTS;
                } else {
                    err_fill(err, PAKKA_ERR_IO,
                             PAKKA_ERR_DOMAIN_WIN32, win32_code, "close",
                             "Could not rename %s to %s (Win32 error %u)",
                             pak->tmp_pakpath, pak->new_pakpath,
                             (unsigned)win32_code);
                    status = PAKKA_ERR_IO;
                }
#else
                if (saved_errno == EEXIST) {
                    err_fill(err, PAKKA_ERR_EXISTS,
                             PAKKA_ERR_DOMAIN_ERRNO,
                             (uint32_t)saved_errno, "close",
                             "Could not create pak %s "
                             "(destination already exists)",
                             pak->new_pakpath);
                    status = PAKKA_ERR_EXISTS;
                } else {
                    err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                             (uint32_t)saved_errno, "close",
                             "Could not rename %s to %s",
                             pak->tmp_pakpath, pak->new_pakpath);
                    status = PAKKA_ERR_IO;
                }
                (void)win32_code;
#endif
            }
        }
    }

    e = pak->head;
    while (e != NULL) {
        next = e->next;
        free(e);
        e = next;
    }

    free(pak);

    return status;
}

pakka_status_t load_pakfile(Pak_t *pak, pakka_error_t *err) {
    int64_t size;
    int saved_errno;

    /* Capture the actual file size up front so every bounds check below
     * compares against ground truth rather than self-reported header
     * values. */
    if (pakka_compat_fseek(pak->fp, 0, SEEK_END) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot seek pak file");
    }
    size = pakka_compat_ftell(pak->fp);
    if (size < 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot determine pak file size");
    }
    pak->file_size = (uint64_t)size;

    /* Read the signature first and classify the format before fetching
     * the rest of the header. A 4-byte ZIP magic should report
     * PAKKA_ERR_UNSUPPORTED rather than I/O-truncation on the unread
     * diroffset/dirlength fields. */
    rewind(pak->fp);
    if (fread(pak->signature, PAKFILE_SIGNATURE_LEN, 1, pak->fp) != 1) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot read pak signature");
    }

    /* PK3/ZIP signatures: PK\3\4 = local file header (normal case),
     * PK\5\6 = empty PK3 (EOCD only). PK\7\8 (spanning) is refused
     * as multi-disk inside the PK3 loader. */
    if (memcmp(pak->signature, "PK\x03\x04", PAKFILE_SIGNATURE_LEN) == 0
        || memcmp(pak->signature, "PK\x05\x06", PAKFILE_SIGNATURE_LEN) == 0) {
        /* Hand off to the PK3 loader. It owns format detection from
         * here on, including the EOCD scan + CDR parse. */
        return pakka_pk3_open_impl(pak, err);
    }
    if (memcmp(pak->signature, "PK\x07\x08", PAKFILE_SIGNATURE_LEN) == 0) {
        return err_fill(err, PAKKA_ERR_UNSUPPORTED,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                        "Multi-disk PK3/ZIP archives are not supported");
    }

    if (memcmp(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN) != 0) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Not a pak file (bad signature)");
    }
    pak->format = PAKKA_FORMAT_PAK;

    if (pakka_read_u32_le(pak->fp, &pak->diroffset) != 0
        || pakka_read_u32_le(pak->fp, &pak->dirlength) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot read pak header (diroffset/dirlength)");
    }

    if ((pak->dirlength % PAKFILE_DIR_ENTRY_SIZE) != 0) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Pak header is corrupt (dirlength not a multiple of %d)",
                        PAKFILE_DIR_ENTRY_SIZE);
    }

    if (pak->diroffset < PAKFILE_HEADER_SIZE) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Pak header is corrupt (diroffset inside header)");
    }

    /* Guard the diroffset+dirlength sum against u32 wrap, then check it
     * sits within the actual file. Compare via subtraction to avoid the
     * overflow risk on the addition itself. */
    if ((uint64_t)pak->dirlength > pak->file_size
        || (uint64_t)pak->diroffset > pak->file_size - pak->dirlength) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Pak header is corrupt (directory extends past EOF)");
    }

    pak->num_entries = pak->dirlength / PAKFILE_DIR_ENTRY_SIZE;

    if (pak->num_entries > PAKFILE_MAX_ENTRIES) {
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Pak has too many entries (%" PRIu32 ", max %u)",
                        pak->num_entries, PAKFILE_MAX_ENTRIES);
    }

    return PAKKA_OK;
}

pakka_status_t load_directory(Pak_t *pak, pakka_error_t *err) {
    Pakfileentry_t *current = NULL;
    Pakfileentry_t *last = NULL;
    uint32_t i;
    uint64_t entry_pos;
    int saved_errno;

    if (pak->num_entries == 0) {
        return PAKKA_OK;
    }

    for (i = 1; i <= pak->num_entries; i++) {
        /* Compute in 64-bit to avoid u32 wrap. load_pakfile has already
         * validated diroffset+dirlength <= file_size and num_entries
         * against the dirlength, so the subtraction can't underflow. */
        entry_pos = (uint64_t)pak->diroffset + (uint64_t)pak->dirlength
                    - (uint64_t)i * PAKFILE_DIR_ENTRY_SIZE;

        if (pakka_compat_fseek(pak->fp, (int64_t)entry_pos, SEEK_SET) != 0) {
            saved_errno = errno;
            err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                     (uint32_t)saved_errno, "open",
                     "Cannot seek to pak directory entry %" PRIu32, i);
            err_set_entry(err, NULL, (size_t)i - 1, entry_pos, 0);
            return PAKKA_ERR_IO;
        }

        current = calloc(1, sizeof(Pakfileentry_t));
        if (current == NULL) {
            err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                     "open",
                     "Cannot allocate pak directory entry %" PRIu32, i);
            err_set_entry(err, NULL, (size_t)i - 1, entry_pos, 0);
            return PAKKA_ERR_NOMEM;
        }

        if (fread(current->filename, PAKFILE_PATH_MAX, 1, pak->fp) != 1
            || pakka_read_u32_le(pak->fp, &current->offset) != 0
            || pakka_read_u32_le(pak->fp, &current->length) != 0) {
            saved_errno = errno;
            free(current);
            err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                     (uint32_t)saved_errno, "open",
                     "Cannot read pak directory entry %" PRIu32, i);
            err_set_entry(err, NULL, (size_t)i - 1, entry_pos, 0);
            return PAKKA_ERR_IO;
        }

        /* The on-disk filename is exactly PAKFILE_PATH_MAX bytes with no
         * guaranteed NUL. The in-memory buffer is one byte larger so we
         * can force termination before any strlen/strcmp/printf("%s")
         * downstream overreads into the offset/length fields. */
        current->filename[PAKFILE_PATH_MAX] = '\0';

        /* Bounds-check the entry against the file we actually have on
         * disk. Reject anything that points into the header or wraps
         * past EOF. Subsequent extract/copy paths can then trust the
         * stored offset+length without re-validating. */
        if (current->offset < PAKFILE_HEADER_SIZE
            || (uint64_t)current->length > pak->file_size
            || (uint64_t)current->offset > pak->file_size - current->length) {
            err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                     "open",
                     "Pak entry %" PRIu32 " bytes out of range "
                     "(offset=%" PRIu32 ", length=%" PRIu32 ")",
                     i, current->offset, current->length);
            err_set_entry(err, current->filename, (size_t)i - 1,
                          (uint64_t)current->offset,
                          (uint64_t)current->length);
            free(current);
            return PAKKA_ERR_FORMAT;
        }

        if (last != NULL) {
            current->next = last;
        }

        last = current;
        /* Publish the partial chain on every iteration so destroy_pak()
         * can free it if a later entry fails. Without this, destroy_pak
         * walks pak->head (still NULL) and leaks the prefix. */
        pak->head = last;
    }

    return PAKKA_OK;
}







Pakfileentry_t *find_entry(Pak_t *pak, char *path) {
    Pakfileentry_t *current;

    if (pak->head == NULL) {
        return NULL;
    }

    current = pak->head;

    do {
        if (strcmp(current->filename, path) == 0) {
            return current;
        }
    } while ((current = current->next) != NULL);

    return NULL;
}

pakka_format_t pakka_format(const pakka_archive_t *archive) {
    /* NULL archive returns AUTO as a sentinel — callers should check for
     * AUTO before treating the result as authoritative. */
    if (archive == NULL) {
        return PAKKA_FORMAT_AUTO;
    }
    return archive->format;
}

size_t pakka_entry_count(const pakka_archive_t *archive) {
    if (archive == NULL) {
        return 0;
    }
    return (size_t)archive->num_entries;
}

pakka_status_t pakka_entry_at(const pakka_archive_t *archive, size_t index,
                              const pakka_entry_t **out) {
    Pakfileentry_t *current;
    size_t i = 0;

    if (out != NULL) {
        *out = NULL;
    }
    if (archive == NULL || out == NULL) {
        return PAKKA_ERR_INVALID_ARGUMENT;
    }

    if (index >= (size_t)archive->num_entries) {
        return PAKKA_ERR_NOT_FOUND;
    }

    for (current = archive->head; current != NULL; current = current->next) {
        if (i == index) {
            *out = current;
            return PAKKA_OK;
        }
        i++;
    }

    /* Should be unreachable given the num_entries guard above. */
    return PAKKA_ERR_NOT_FOUND;
}

pakka_status_t pakka_find_entry(const pakka_archive_t *archive,
                                const char *entry_name,
                                const pakka_entry_t **out) {
    Pakfileentry_t *current;

    if (out != NULL) {
        *out = NULL;
    }
    if (archive == NULL || entry_name == NULL || out == NULL) {
        return PAKKA_ERR_INVALID_ARGUMENT;
    }

    for (current = archive->head; current != NULL; current = current->next) {
        if (strcmp(current->filename, entry_name) == 0) {
            *out = current;
            return PAKKA_OK;
        }
    }

    return PAKKA_ERR_NOT_FOUND;
}

const char *pakka_entry_name(const pakka_entry_t *entry) {
    if (entry == NULL) {
        return NULL;
    }
    return entry->filename;
}

uint64_t pakka_entry_size(const pakka_entry_t *entry) {
    if (entry == NULL) {
        return 0;
    }
    return (uint64_t)entry->length;
}

uint64_t pakka_entry_offset(const pakka_entry_t *entry) {
    if (entry == NULL) {
        return 0;
    }
    return (uint64_t)entry->offset;
}

pakka_status_t pakka_open_entry(pakka_archive_t *archive,
                                const char *entry_name,
                                pakka_reader_t **out,
                                pakka_error_t *err) {
    const pakka_entry_t *entry = NULL;
    pakka_reader_t *reader;
    pakka_status_t s;

    if (out != NULL) {
        *out = NULL;
    }
    if (archive == NULL || entry_name == NULL || out == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                        "pakka_open_entry: archive, entry_name, "
                        "and out must be non-NULL");
    }
    if (archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                        "pakka_open_entry: archive has no live "
                        "file handle (closed or commit-failed)");
    }

    s = pakka_find_entry(archive, entry_name, &entry);
    if (s != PAKKA_OK) {
        err_fill(err, s, PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                 s == PAKKA_ERR_NOT_FOUND
                   ? "Entry not found: %s"
                   : "pakka_find_entry failed for %s",
                 entry_name);
        /* Populate the structured entry-name field so callers can
         * format diagnostics without re-parsing message. */
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return s;
    }

    reader = calloc(1, sizeof(pakka_reader_t));
    if (reader == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open_entry",
                        "Cannot allocate pakka_reader_t for %s",
                        entry_name);
    }

    reader->archive = archive;
    reader->next_offset = (uint64_t)entry->offset;
    reader->remaining = (uint64_t)entry->length;

    if (archive->format == PAKKA_FORMAT_PK3) {
        pakka_status_t pk3_status = pakka_pk3_open_entry_impl(
            archive, reader, (pakka_entry_t *)entry, err);
        if (pk3_status != PAKKA_OK) {
            free(reader);
            return pk3_status;
        }
    }

    *out = reader;
    return PAKKA_OK;
}

pakka_status_t pakka_reader_read(pakka_reader_t *reader, void *buf,
                                 size_t len, size_t *nread,
                                 pakka_error_t *err) {
    size_t chunk;
    size_t got;
    int saved_errno;

    if (nread != NULL) {
        *nread = 0;
    }
    if (reader == NULL || buf == NULL || nread == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "reader_read",
                        "pakka_reader_read: reader, buf, "
                        "and nread must be non-NULL");
    }
    if (reader->archive == NULL || reader->archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "reader_read",
                        "pakka_reader_read: backing archive has no "
                        "live file handle");
    }

    if (reader->remaining == 0 || len == 0) {
        return PAKKA_OK;            /* clean EOF (or no-op read) */
    }

    /* PK3 reader (inflated buffer for DEFLATE, otherwise STORED stream). */
    if (reader->archive->format == PAKKA_FORMAT_PK3) {
        return pakka_pk3_reader_read_impl(reader, buf, len, nread, err);
    }

    chunk = (len < reader->remaining) ? len : (size_t)reader->remaining;

    /* Re-seek every read: multiple readers may share the archive's
     * single FILE *, so a different reader's seek between calls would
     * otherwise leave this one mispositioned. fseek is cheap; coherence
     * is the priority. */
    if (pakka_compat_fseek(reader->archive->fp, (int64_t)reader->next_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "reader_read",
                        "Cannot seek to entry byte offset %" PRIu64,
                        reader->next_offset);
    }

    got = fread(buf, 1, chunk, reader->archive->fp);
    if (got != chunk) {
        saved_errno = errno;
        *nread = got;
        reader->next_offset += (uint64_t)got;
        reader->remaining -= (uint64_t)got;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "reader_read",
                        "Short read at offset %" PRIu64
                        " (got %zu of %zu)",
                        reader->next_offset - (uint64_t)got, got, chunk);
    }

    *nread = got;
    reader->next_offset += (uint64_t)got;
    reader->remaining -= (uint64_t)got;
    return PAKKA_OK;
}

void pakka_reader_close(pakka_reader_t *reader) {
    if (reader == NULL) {
        return;
    }
    /* PK3 readers may hold a malloc'd inflated buffer for DEFLATE
     * entries; release it before freeing the reader struct. */
    pakka_pk3_reader_close_impl(reader);
    /* The archive owns the FILE*; the reader only holds a re-seek
     * position. No fclose here. */
    free(reader);
}

pakka_status_t pakka_set_max_decompressed_size(pakka_archive_t *archive,
                                               uint64_t max_bytes,
                                               pakka_error_t *err) {
    if (archive == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0,
                        "set_max_decompressed_size",
                        "pakka_set_max_decompressed_size: archive must "
                        "be non-NULL");
    }
    archive->pk3_max_decompressed = max_bytes;
    return PAKKA_OK;
}

pakka_status_t pakka_add_file(pakka_archive_t *archive,
                              const char *source_path,
                              const char *entry_name,
                              pakka_error_t *err) {
    FILE *src_fp = NULL;
    int64_t src_size;
    uint64_t append_offset;
    Pakfileentry_t *tail;
    Pakfileentry_t *entry = NULL;
    char *bytes = NULL;
    size_t name_len;
    size_t remaining;
    int saved_errno;

    if (archive == NULL || source_path == NULL || entry_name == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_file",
                        "pakka_add_file: archive, source_path, "
                        "and entry_name must be non-NULL");
    }
    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_file",
                        "pakka_add_file: archive not writable "
                        "or no live file handle");
    }

    if (archive->format == PAKKA_FORMAT_PK3) {
        if (pakka_unsafe_entry_name(entry_name)) {
            err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                     "add_file", "Refusing unsafe entry name %s",
                     entry_name);
            err_set_entry(err, entry_name, (size_t)-1, 0, 0);
            return PAKKA_ERR_UNSAFE_NAME;
        }
        /* Reject duplicate entry name — same handling as PAK. */
        if (find_entry(archive, (char *)entry_name) != NULL) {
            err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                     "add_file",
                     "Refusing duplicate entry name %s", entry_name);
            err_set_entry(err, entry_name, (size_t)-1, 0, 0);
            return PAKKA_ERR_DUPLICATE;
        }
        return pakka_pk3_add_file_impl(archive, source_path, entry_name,
                                       err);
    }

    /* On-disk filename field is PAKFILE_PATH_MAX bytes; longest legal
     * name is one less. Reject before strcpy that would overwrite
     * adjacent struct fields. */
    name_len = strlen(entry_name);
    if (name_len >= PAKFILE_PATH_MAX) {
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "Entry name too long for pak (%zu bytes, max %d)",
                 name_len, PAKFILE_PATH_MAX - 1);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_LIMIT;
    }

    /* Refuse to bake an entry name that pakka itself would refuse to
     * extract. Catches absolute paths, traversal, Windows reserved
     * names, control bytes. */
    if (pakka_unsafe_entry_name(entry_name)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "Entry name is not safe to extract: %s", entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }

    if (find_entry(archive, (char *)entry_name) != NULL) {
        err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "Entry already exists in pak: %s", entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_DUPLICATE;
    }

    /* Source-side hardening: reject symlink/reparse points and any
     * non-regular file (FIFO, socket, char/block device). Mirrors the
     * checks legacy add_folder_r runs on every recursive entry; a
     * direct pakka_add_file caller needs the same. */
    if (pakka_compat_is_reparse_or_symlink(source_path)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "Refusing symlink/reparse source: %s", source_path);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }
    {
        struct stat sb;
        if (pakka_compat_lstat(source_path, &sb) != 0) {
            saved_errno = errno;
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "add_file",
                            "Cannot stat source %s", source_path);
        }
        if (!S_ISREG(sb.st_mode)) {
            err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE,
                     0, "add_file",
                     "Source is not a regular file: %s", source_path);
            err_set_entry(err, entry_name, (size_t)-1, 0, 0);
            return PAKKA_ERR_UNSAFE_NAME;
        }
    }

    src_fp = fopen(source_path, "rb");
    if (src_fp == NULL) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_file",
                        "Cannot open source %s", source_path);
    }

    src_size = pakka_filesize(src_fp);
    if (src_size < 0) {
        saved_errno = errno;
        fclose(src_fp);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_file",
                        "Cannot determine size of %s", source_path);
    }
    if ((uint64_t)src_size > UINT32_MAX) {
        fclose(src_fp);
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "File too large for pak format (%" PRId64 " bytes, max %u)",
                 src_size, UINT32_MAX);
        err_set_entry(err, entry_name, (size_t)-1, 0, (uint64_t)src_size);
        return PAKKA_ERR_LIMIT;
    }

    /* Append at the highest byte-offset across all existing entries, not
     * the directory-order tail. Quake's pak0.pak has out-of-order
     * payloads and an orphan blob past the directory; the directory
     * tail's offset+length would overwrite live data. */
    tail = find_tail(archive);
    append_offset = (tail == NULL)
                  ? (uint64_t)PAKFILE_HEADER_SIZE
                  : compute_payload_end(archive);

    if (append_offset > UINT32_MAX - (uint64_t)src_size) {
        fclose(src_fp);
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_file",
                 "Pak would exceed 4 GiB after adding %s", source_path);
        err_set_entry(err, entry_name, (size_t)-1, append_offset,
                      (uint64_t)src_size);
        return PAKKA_ERR_LIMIT;
    }

    /* Allocate the entry node BEFORE touching the on-disk archive. A
     * NOMEM here leaves the pak file unmodified; an in-progress fwrite
     * failure would leave orphan bytes either way, but at least an
     * allocation failure here can't desynchronize the in-memory entry
     * list from what's on disk. */
    entry = calloc(1, sizeof(Pakfileentry_t));
    if (entry == NULL) {
        fclose(src_fp);
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "add_file",
                        "Cannot allocate pak entry for %s", entry_name);
    }
    memcpy(entry->filename, entry_name, name_len);
    entry->filename[name_len] = '\0';
    entry->offset = (uint32_t)append_offset;
    entry->length = (uint32_t)src_size;

    if (pakka_compat_fseek(archive->fp, (int64_t)append_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        fclose(src_fp);
        free(entry);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_file",
                        "Cannot seek to append point in pak");
    }

    if (src_size > 0) {
        bytes = malloc(PAKFILE_COPY_CHUNK);
        if (bytes == NULL) {
            fclose(src_fp);
            free(entry);
            return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE,
                            0, "add_file",
                            "Cannot allocate copy buffer for %s",
                            source_path);
        }
        remaining = (size_t)src_size;
        while (remaining > 0) {
            size_t chunk = remaining > PAKFILE_COPY_CHUNK
                         ? PAKFILE_COPY_CHUNK : remaining;
            if (fread(bytes, 1, chunk, src_fp) != chunk) {
                saved_errno = errno;
                free(bytes);
                fclose(src_fp);
                free(entry);
                return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "add_file",
                                "Cannot read %s", source_path);
            }
            if (fwrite(bytes, 1, chunk, archive->fp) != chunk) {
                saved_errno = errno;
                free(bytes);
                fclose(src_fp);
                free(entry);
                return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "add_file",
                                "Cannot append %s to pak", entry_name);
            }
            remaining -= chunk;
        }
        free(bytes);
    }
    fclose(src_fp);

    if (tail == NULL) {
        archive->head = entry;
    } else {
        tail->next = entry;
    }
    archive->num_entries++;
    archive->dirty = 1;

    return PAKKA_OK;
}

pakka_status_t pakka_add_memory(pakka_archive_t *archive,
                                const char *entry_name,
                                const void *data, size_t len,
                                pakka_error_t *err) {
    Pakfileentry_t *tail;
    Pakfileentry_t *entry;
    uint64_t append_offset;
    size_t name_len;
    int saved_errno;

    if (archive == NULL || entry_name == NULL || (data == NULL && len > 0)) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_memory",
                        "pakka_add_memory: archive and entry_name must "
                        "be non-NULL; data may be NULL only when len == 0");
    }
    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_memory",
                        "pakka_add_memory: archive not writable "
                        "or no live file handle");
    }

    if (archive->format == PAKKA_FORMAT_PK3) {
        if (pakka_unsafe_entry_name(entry_name)) {
            err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                     "add_memory", "Refusing unsafe entry name %s",
                     entry_name);
            err_set_entry(err, entry_name, (size_t)-1, 0, 0);
            return PAKKA_ERR_UNSAFE_NAME;
        }
        if (find_entry(archive, (char *)entry_name) != NULL) {
            err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                     "add_memory",
                     "Refusing duplicate entry name %s", entry_name);
            err_set_entry(err, entry_name, (size_t)-1, 0, 0);
            return PAKKA_ERR_DUPLICATE;
        }
        return pakka_pk3_add_memory_impl(archive, entry_name, data, len,
                                         err);
    }

    name_len = strlen(entry_name);
    if (name_len >= PAKFILE_PATH_MAX) {
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_memory",
                 "Entry name too long for pak (%zu bytes, max %d)",
                 name_len, PAKFILE_PATH_MAX - 1);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_LIMIT;
    }
    if (pakka_unsafe_entry_name(entry_name)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_memory",
                 "Entry name is not safe to extract: %s", entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }
    if (find_entry(archive, (char *)entry_name) != NULL) {
        err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_memory",
                 "Entry already exists in pak: %s", entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_DUPLICATE;
    }
    if (len > UINT32_MAX) {
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_memory",
                 "Payload too large for pak format (%zu bytes, max %u)",
                 len, UINT32_MAX);
        err_set_entry(err, entry_name, (size_t)-1, 0, (uint64_t)len);
        return PAKKA_ERR_LIMIT;
    }

    tail = find_tail(archive);
    append_offset = (tail == NULL)
                  ? (uint64_t)PAKFILE_HEADER_SIZE
                  : compute_payload_end(archive);
    if (append_offset > UINT32_MAX - (uint64_t)len) {
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 "add_memory",
                 "Pak would exceed 4 GiB after adding %s", entry_name);
        err_set_entry(err, entry_name, (size_t)-1, append_offset,
                      (uint64_t)len);
        return PAKKA_ERR_LIMIT;
    }

    entry = calloc(1, sizeof(Pakfileentry_t));
    if (entry == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "add_memory",
                        "Cannot allocate pak entry for %s", entry_name);
    }
    memcpy(entry->filename, entry_name, name_len);
    entry->filename[name_len] = '\0';
    entry->offset = (uint32_t)append_offset;
    entry->length = (uint32_t)len;

    if (pakka_compat_fseek(archive->fp, (int64_t)append_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        free(entry);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_memory",
                        "Cannot seek to append point in pak");
    }
    if (len > 0 && fwrite(data, 1, len, archive->fp) != len) {
        saved_errno = errno;
        free(entry);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_memory",
                        "Cannot append memory payload for %s", entry_name);
    }

    if (tail == NULL) {
        archive->head = entry;
    } else {
        tail->next = entry;
    }
    archive->num_entries++;
    archive->dirty = 1;

    return PAKKA_OK;
}

pakka_status_t pakka_read_entry_alloc(pakka_archive_t *archive,
                                      const char *entry_name,
                                      void **data, size_t *len,
                                      pakka_error_t *err) {
    pakka_reader_t *reader = NULL;
    const pakka_entry_t *entry = NULL;
    pakka_status_t s;
    unsigned char *buf;
    uint64_t total;
    size_t nread;
    size_t consumed = 0;

    if (data != NULL) {
        *data = NULL;
    }
    if (len != NULL) {
        *len = 0;
    }
    if (archive == NULL || entry_name == NULL
        || data == NULL || len == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "read_entry_alloc",
                        "pakka_read_entry_alloc: archive, entry_name, "
                        "data, and len must be non-NULL");
    }

    s = pakka_find_entry(archive, entry_name, &entry);
    if (s != PAKKA_OK) {
        err_fill(err, s, PAKKA_ERR_DOMAIN_NONE, 0, "read_entry_alloc",
                 s == PAKKA_ERR_NOT_FOUND
                   ? "Entry not found: %s"
                   : "pakka_find_entry failed for %s",
                 entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return s;
    }
    total = pakka_entry_size(entry);
    if (total == 0) {
        return PAKKA_OK;        /* *data stays NULL, *len stays 0 */
    }
    if (total > (uint64_t)SIZE_MAX) {
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "read_entry_alloc",
                        "Entry size exceeds SIZE_MAX on this host");
    }

    buf = malloc((size_t)total);
    if (buf == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "read_entry_alloc",
                        "Cannot allocate %" PRIu64 " bytes for %s",
                        total, entry_name);
    }

    s = pakka_open_entry(archive, entry_name, &reader, err);
    if (s != PAKKA_OK) {
        free(buf);
        return s;
    }
    while (consumed < (size_t)total) {
        s = pakka_reader_read(reader, buf + consumed,
                              (size_t)total - consumed, &nread, err);
        if (s != PAKKA_OK) {
            free(buf);
            pakka_reader_close(reader);
            return s;
        }
        if (nread == 0) {
            free(buf);
            pakka_reader_close(reader);
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_NONE, 0,
                            "read_entry_alloc",
                            "Short stream: expected %" PRIu64
                            " bytes, got %zu", total, consumed);
        }
        consumed += nread;
    }
    pakka_reader_close(reader);

    *data = buf;
    *len = consumed;
    return PAKKA_OK;
}

void pakka_free(void *ptr) {
    /* Thin wrapper around free() so callers don't cross the C-runtime
     * boundary on Windows when the library and consumer happen to link
     * different CRTs. POSIX consumers can use free() interchangeably,
     * but using pakka_free everywhere makes the contract portable. */
    free(ptr);
}

pakka_status_t pakka_delete(pakka_archive_t *archive,
                            const char *entry_name,
                            pakka_error_t *err) {
    Pakfileentry_t *current;
    Pakfileentry_t *prev = NULL;

    if (archive == NULL || entry_name == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "delete",
                        "pakka_delete: archive and entry_name "
                        "must be non-NULL");
    }
    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "delete",
                        "pakka_delete: archive not writable "
                        "or no live file handle");
    }

    for (current = archive->head; current != NULL; current = current->next) {
        if (strcmp(current->filename, entry_name) == 0) {
            if (prev == NULL) {
                archive->head = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            archive->num_entries--;
            archive->dirty = 1;
            archive->needs_rebuild = 1;
            return PAKKA_OK;
        }
        prev = current;
    }

    err_fill(err, PAKKA_ERR_NOT_FOUND, PAKKA_ERR_DOMAIN_NONE, 0, "delete",
             "Entry not found: %s", entry_name);
    err_set_entry(err, entry_name, (size_t)-1, 0, 0);
    return PAKKA_ERR_NOT_FOUND;
}

/* Commit pending changes to disk. Three modes:
 *   - clean state (!dirty): no-op
 *   - dirty without rebuild (adds only): write directory in place at the
 *     new diroffset (= byte tail after appended payloads), update header
 *   - dirty with rebuild (any pakka_delete happened): copy retained
 *     entries to a temp file, write directory, fclose original, rename
 *     temp over cur_pakpath. pak->fp is reopened on the renamed file so
 *     subsequent operations remain valid.
 *
 * The is_new case (after pakka_create) writes header+directory the same
 * as a regular add commit; the temp file is renamed to new_pakpath by
 * pakka_close, not here. */
pakka_status_t pakka_commit(pakka_archive_t *archive, pakka_error_t *err) {
    Pakfileentry_t *current;
    Pakfileentry_t *new_head = NULL;
    Pakfileentry_t *new_tail = NULL;
    FILE *tfd = NULL;
    FILE *old_fp;
    int saved_errno;
    pakka_status_t status = PAKKA_OK;

    if (archive == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                        "pakka_commit: archive must be non-NULL");
    }

    if (!archive->dirty) {
        return PAKKA_OK;        /* no-op */
    }

    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                        "pakka_commit: archive not writable or fp closed");
    }

    if (archive->format == PAKKA_FORMAT_PK3) {
        return pakka_pk3_commit_impl(archive, err);
    }

    if (!archive->needs_rebuild) {
        /* Add-only path: just rewrite the directory + header in place.
         * write_pak_directory positions itself at byte-tail before
         * writing the directory, then rewinds and writes the header. */
        status = write_pak_directory(archive, err);
        if (status != PAKKA_OK) {
            return status;
        }
        /* Flush so a subsequent fopen on the same file sees the bytes. */
        if (fflush(archive->fp) != 0) {
            saved_errno = errno;
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot flush pak directory write");
        }
        archive->dirty = 0;
        return PAKKA_OK;
    }

    /* Rebuild path (needs_rebuild=1 because at least one pakka_delete).
     * The byte layout no longer matches the directory order — id's
     * non-sequential pak0.pak would shift wrong if we tried in-place.
     *
     * Use a LOCAL scratch path rather than the archive's tmp_pakpath:
     * for an is_new archive, tmp_pakpath is the path of the original
     * pakka_create temp which pakka_close will rename to new_pakpath.
     * Overwriting tmp_pakpath here would lose that target. */
    {
        char rebuild_scratch[OS_PATH_MAX];
        const char *dir_hint;
        const char *rename_target;
        /* Parallel arrays of survivors. entry_ptrs[] is the in-memory
         * list pointer; new_offsets[] is the byte offset each entry
         * will have AFTER the rebuild. We install new offsets only
         * once every copy AND the directory write AND the fclose of
         * the temp succeed. Until then, entry->offset still describes
         * archive->fp's pre-rebuild byte layout, so an error path can
         * return without touching archive state. */
        uint32_t *new_offsets = NULL;
        Pakfileentry_t **entry_ptrs = NULL;
        size_t n_entries = 0;
        size_t i;

        dir_hint = archive->is_new ? archive->tmp_pakpath
                                   : archive->cur_pakpath;
        rename_target = dir_hint;       /* same path, after rename */

        for (current = archive->head; current != NULL;
             current = current->next) {
            n_entries++;
        }
        if (n_entries > 0) {
            new_offsets = malloc(sizeof(uint32_t) * n_entries);
            entry_ptrs = malloc(sizeof(Pakfileentry_t *) * n_entries);
            if (new_offsets == NULL || entry_ptrs == NULL) {
                free(new_offsets);
                free(entry_ptrs);
                return err_fill(err, PAKKA_ERR_NOMEM,
                                PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                                "Cannot allocate rebuild bookkeeping");
            }
            i = 0;
            for (current = archive->head; current != NULL;
                 current = current->next) {
                entry_ptrs[i++] = current;
            }
        }

        tfd = pakka_compat_mkstemp_open(dir_hint, "pakkaXXXXXX",
                                  rebuild_scratch,
                                  sizeof(rebuild_scratch));
        if (tfd == NULL) {
            saved_errno = errno;
            free(new_offsets);
            free(entry_ptrs);
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot create rebuild scratch near %s",
                            dir_hint);
        }
        if (pakka_compat_fseek(tfd, PAKFILE_HEADER_SIZE, SEEK_SET) != 0) {
            saved_errno = errno;
            fclose(tfd);
            (void)remove(rebuild_scratch);
            free(new_offsets);
            free(entry_ptrs);
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot seek in rebuild scratch");
        }

        /* Copy survivors. entry->offset is NOT mutated by
         * copy_between_paks (it returns the new offset via out param).
         * On failure here, nothing in archive state has changed. */
        for (i = 0; i < n_entries; i++) {
            status = copy_between_paks(entry_ptrs[i], archive->fp, tfd,
                                       &new_offsets[i], err);
            if (status != PAKKA_OK) {
                fclose(tfd);
                (void)remove(rebuild_scratch);
                free(new_offsets);
                free(entry_ptrs);
                return status;
            }
            if (new_tail == NULL) {
                new_head = entry_ptrs[i];
            } else {
                new_tail->next = entry_ptrs[i];
            }
            new_tail = entry_ptrs[i];
        }
        if (new_tail != NULL) {
            new_tail->next = NULL;
        }

        /* write_pak_directory needs to see the new offsets. Install
         * them now (still rollback-safe; archive->head order is
         * already the new layout, and pak->fp hasn't been swapped to
         * the scratch yet). Save the prior values in old_offsets_swap
         * so we can revert if the directory write or close fails. */
        archive->head = new_head;
        {
            uint32_t *old_offsets_swap;
            old_offsets_swap = malloc(sizeof(uint32_t) * n_entries);
            if (old_offsets_swap == NULL && n_entries > 0) {
                fclose(tfd);
                (void)remove(rebuild_scratch);
                free(new_offsets);
                free(entry_ptrs);
                return err_fill(err, PAKKA_ERR_NOMEM,
                                PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                                "Cannot allocate rollback buffer");
            }
            for (i = 0; i < n_entries; i++) {
                old_offsets_swap[i] = entry_ptrs[i]->offset;
                entry_ptrs[i]->offset = new_offsets[i];
            }

            old_fp = archive->fp;
            archive->fp = tfd;
            status = write_pak_directory(archive, err);
            archive->fp = old_fp;
            if (status != PAKKA_OK) {
                for (i = 0; i < n_entries; i++) {
                    entry_ptrs[i]->offset = old_offsets_swap[i];
                }
                fclose(tfd);
                (void)remove(rebuild_scratch);
                free(old_offsets_swap);
                free(new_offsets);
                free(entry_ptrs);
                return status;
            }
            if (fclose(tfd) != 0) {
                saved_errno = errno;
                for (i = 0; i < n_entries; i++) {
                    entry_ptrs[i]->offset = old_offsets_swap[i];
                }
                (void)remove(rebuild_scratch);
                free(old_offsets_swap);
                free(new_offsets);
                free(entry_ptrs);
                return err_fill(err, PAKKA_ERR_IO,
                                PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "commit",
                                "Cannot finalize rebuild scratch %s "
                                "(disk full or I/O error)",
                                rebuild_scratch);
            }

            /* Past this point the new layout is durably on disk in
             * rebuild_scratch. Close original, rename. If rename or
             * reopen fails we keep the new offsets installed (the
             * scratch IS the canonical content even if not yet
             * renamed; rolling back would mismatch what we just wrote). */
            fclose(archive->fp);
            archive->fp = NULL;

            {
                uint32_t win32_code = 0;
                if (pakka_compat_rename_replace(rebuild_scratch, rename_target,
                                          &win32_code) != 0) {
                    saved_errno = errno;
                    /* Best-effort restore: revert offsets, drop the
                     * scratch, and reopen the un-renamed original so
                     * the archive handle is at least readable. */
                    for (i = 0; i < n_entries; i++) {
                        entry_ptrs[i]->offset = old_offsets_swap[i];
                    }
                    (void)remove(rebuild_scratch);
                    archive->fp = fopen(rename_target, "r+b");
                    free(old_offsets_swap);
                    free(new_offsets);
                    free(entry_ptrs);
#ifdef _WIN32
                    return err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_WIN32, win32_code,
                                    "commit",
                                    "Could not rename %s to %s "
                                    "(Win32 error %u)",
                                    rebuild_scratch, rename_target,
                                    (unsigned)win32_code);
#else
                    (void)win32_code;
                    return err_fill(err, PAKKA_ERR_IO,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)saved_errno, "commit",
                                    "Could not rename %s to %s",
                                    rebuild_scratch, rename_target);
#endif
                }
            }

            free(old_offsets_swap);
        }

        archive->fp = fopen(rename_target, "r+b");
        if (archive->fp == NULL) {
            saved_errno = errno;
            free(new_offsets);
            free(entry_ptrs);
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot reopen %s after rebuild",
                            rename_target);
        }

        free(new_offsets);
        free(entry_ptrs);
        archive->dirty = 0;
        archive->needs_rebuild = 0;
    }
    return status;
}

/* { normalized, original } record used by pakka_verify's collision
 * scan. Keeping both in one struct lets qsort sort them together so a
 * detected collision can name both original entries. */
typedef struct {
    char *norm;
    const char *original;
} verify_collision_record_t;

static int verify_collision_cmp(const void *a, const void *b) {
    const verify_collision_record_t *aa = (const verify_collision_record_t *)a;
    const verify_collision_record_t *bb = (const verify_collision_record_t *)b;
    return strcmp(aa->norm, bb->norm);
}

/* No-op when report is NULL. */
static void verify_report(pakka_report_fn report, void *userdata,
                          pakka_report_severity_t severity,
                          pakka_status_t status,
                          const char *entry_name,
                          const char *fmt, ...) {
    char buf[PAKKA_MESSAGE_SIZE];
    va_list args;
    if (report == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    report(userdata, severity, status, entry_name, buf);
}

pakka_status_t pakka_verify(pakka_archive_t *archive, unsigned flags,
                            pakka_report_fn report, void *userdata,
                            pakka_error_t *err) {
    Pakfileentry_t *current;
    pakka_status_t first_error = PAKKA_OK;
    char buf[PAKFILE_COPY_CHUNK];
    size_t n_entries = 0;
    size_t i;

    if (archive == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                        "pakka_verify: archive must be non-NULL");
    }
    /* PAKKA_VERIFY_DEEP is the only defined flag bit. */
    if ((flags & ~(unsigned)PAKKA_VERIFY_DEEP) != 0u) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                        "pakka_verify: unknown flag bits 0x%x", flags);
    }
    if (archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                        "pakka_verify: archive has no live file handle");
    }

    /* Pass A: per-entry name and payload integrity. */
    for (current = archive->head; current != NULL; current = current->next) {
        n_entries++;

        if (pakka_unsafe_entry_name(current->filename)) {
            verify_report(report, userdata, PAKKA_REPORT_ERROR,
                          PAKKA_ERR_UNSAFE_NAME, current->filename,
                          "Entry name is not safe to extract");
            if (first_error == PAKKA_OK) {
                err_fill(err, PAKKA_ERR_UNSAFE_NAME,
                         PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                         "Entry name is not safe to extract");
                err_set_entry(err, current->filename, (size_t)-1,
                              (uint64_t)current->offset,
                              (uint64_t)current->length);
                first_error = PAKKA_ERR_UNSAFE_NAME;
            }
            continue;        /* skip payload read for unsafe names */
        }

        if (current->length == 0) {
            verify_report(report, userdata, PAKKA_REPORT_INFO,
                          PAKKA_OK, current->filename,
                          "OK (0 bytes)");
            continue;
        }

        /* PK3 entries: on-disk payload is pk3_compressed_size bytes
         * at pk3_payload_offset; entry->length is the UNCOMPRESSED
         * size (which the PAK reads-stream-length convention doesn't
         * match for DEFLATE entries). */
        {
            uint32_t verify_offset = current->offset;
            uint32_t verify_length = current->length;
            if (archive->format == PAKKA_FORMAT_PK3) {
                verify_offset = current->pk3_payload_offset;
                verify_length = current->pk3_compressed_size;
            }
            if (pakka_compat_fseek(archive->fp, (int64_t)verify_offset, SEEK_SET) != 0) {
                int seek_errno = errno;
                verify_report(report, userdata, PAKKA_REPORT_ERROR,
                              PAKKA_ERR_IO, current->filename,
                              "Cannot seek to entry payload at offset %"
                              PRIu32, verify_offset);
                if (first_error == PAKKA_OK) {
                    err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                             (uint32_t)seek_errno, "verify",
                             "Cannot seek to entry payload");
                    err_set_entry(err, current->filename, (size_t)-1,
                                  (uint64_t)verify_offset,
                                  (uint64_t)verify_length);
                    first_error = PAKKA_ERR_IO;
                }
                continue;
            }
            {
                uint64_t remaining = (uint64_t)verify_length;
                int io_failed = 0;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(buf)
                                 ? sizeof(buf) : (size_t)remaining;
                    if (fread(buf, 1, chunk, archive->fp) != chunk) {
                        int read_errno = errno;
                        uint64_t at = (uint64_t)verify_offset
                                    + ((uint64_t)verify_length - remaining);
                        verify_report(report, userdata, PAKKA_REPORT_ERROR,
                                      PAKKA_ERR_IO, current->filename,
                                      "Cannot read entry payload "
                                      "(short read at offset %" PRIu64 ")",
                                      at);
                        if (first_error == PAKKA_OK) {
                            err_fill(err, PAKKA_ERR_IO,
                                     PAKKA_ERR_DOMAIN_ERRNO,
                                     (uint32_t)read_errno, "verify",
                                     "Short read on entry payload");
                            err_set_entry(err, current->filename, (size_t)-1,
                                          at, (uint64_t)verify_length);
                            first_error = PAKKA_ERR_IO;
                        }
                        io_failed = 1;
                        break;
                    }
                    remaining -= chunk;
                }
                if (!io_failed) {
                    /* Deep verify (PK3 only): decompress + CRC32 check.
                     * Structural read above already confirmed the
                     * compressed bytes are readable; this catches
                     * bit-rot in the payload, CRC mismatch, and
                     * trailing junk in the deflate stream. */
                    if (archive->format == PAKKA_FORMAT_PK3
                        && (flags & PAKKA_VERIFY_DEEP)) {
                        pakka_error_t deep_err;
                        pakka_status_t dv = pakka_pk3_deep_verify_entry(
                            archive, current, &deep_err);
                        if (dv != PAKKA_OK) {
                            verify_report(report, userdata,
                                          PAKKA_REPORT_ERROR, dv,
                                          current->filename,
                                          "%s", deep_err.message);
                            if (first_error == PAKKA_OK) {
                                if (err != NULL) *err = deep_err;
                                first_error = dv;
                            }
                            continue;
                        }
                    }
                    verify_report(report, userdata, PAKKA_REPORT_INFO,
                                  PAKKA_OK, current->filename,
                                  "OK (%" PRIu32 " bytes)",
                                  current->length);
                }
            }
        }
    }

    /* Pass B: normalized-collision scan. Builds one array of
     * { norm, original } records so we can sort with qsort
     * (O(n log n)) while still naming both colliding originals. */
    if (n_entries > 1) {
        verify_collision_record_t *recs;
        recs = malloc(sizeof(verify_collision_record_t) * n_entries);
        if (recs == NULL) {
            return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE,
                            0, "verify",
                            "Cannot allocate collision scratch");
        }
        i = 0;
        for (current = archive->head; current != NULL;
             current = current->next) {
            size_t name_len = strlen(current->filename);
            recs[i].norm = malloc(name_len + 1);
            if (recs[i].norm == NULL) {
                size_t j;
                for (j = 0; j < i; j++) free(recs[j].norm);
                free(recs);
                return err_fill(err, PAKKA_ERR_NOMEM,
                                PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                                "Cannot allocate normalized-name buffer");
            }
            pakka_normalize_entry_name(current->filename, recs[i].norm,
                                       name_len + 1);
            recs[i].original = current->filename;
            i++;
        }
        qsort(recs, n_entries, sizeof(verify_collision_record_t),
              verify_collision_cmp);
        for (i = 1; i < n_entries; i++) {
            if (strcmp(recs[i - 1].norm, recs[i].norm) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Normalized collision with '%s'",
                         recs[i - 1].original);
                verify_report(report, userdata, PAKKA_REPORT_ERROR,
                              PAKKA_ERR_DUPLICATE,
                              recs[i].original, "%s", msg);
                if (first_error == PAKKA_OK) {
                    err_fill(err, PAKKA_ERR_DUPLICATE,
                             PAKKA_ERR_DOMAIN_NONE, 0, "verify",
                             "%s", msg);
                    err_set_entry(err, recs[i].original,
                                  (size_t)-1, 0, 0);
                    first_error = PAKKA_ERR_DUPLICATE;
                }
            }
        }
        for (i = 0; i < n_entries; i++) free(recs[i].norm);
        free(recs);
    }

    return first_error;
}

/* Copy one entry's payload from ffd to tfd, returning the new byte
 * offset in tfd via *new_offset_out. The caller is responsible for
 * deciding when to commit *new_offset_out into entry->offset — leaving
 * the entry untouched here lets a failing pakka_commit roll back its
 * in-memory layout. */
pakka_status_t copy_between_paks(Pakfileentry_t *entry, FILE *ffd, FILE *tfd,
                                 uint32_t *new_offset_out,
                                 pakka_error_t *err) {
    char *buffer;
    int64_t new_offset;
    uint32_t src_offset = entry->offset;
    int saved_errno;

    if (pakka_compat_fseek(ffd, (int64_t)src_offset, SEEK_SET) != 0) {
        saved_errno = errno;
        err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                 (uint32_t)saved_errno, "commit",
                 "Cannot seek to source entry");
        err_set_entry(err, entry->filename, (size_t)-1,
                      (uint64_t)src_offset, (uint64_t)entry->length);
        return PAKKA_ERR_IO;
    }

    new_offset = pakka_compat_ftell(tfd);
    if (new_offset < 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "commit",
                        "Cannot determine offset in temp pak");
    }
    if ((uint64_t)new_offset > UINT32_MAX) {
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "commit",
                        "Temp pak exceeds 4 GiB during rebuild");
    }
    if (new_offset_out != NULL) {
        *new_offset_out = (uint32_t)new_offset;
    }

    if (entry->length == 0) {
        return PAKKA_OK;
    }

    /* Stream in PAKFILE_COPY_CHUNK chunks rather than malloc(length)
     * so a deleting a 100 MiB-retained entry doesn't fail on RAM
     * pressure. peak RSS bounded at 64 KiB regardless of payload. */
    buffer = malloc(PAKFILE_COPY_CHUNK);
    if (buffer == NULL) {
        err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                 "commit",
                 "Cannot allocate copy buffer for entry");
        err_set_entry(err, entry->filename, (size_t)-1,
                      (uint64_t)src_offset, (uint64_t)entry->length);
        return PAKKA_ERR_NOMEM;
    }

    {
        size_t remaining = (size_t)entry->length;
        while (remaining > 0) {
            size_t chunk = remaining > PAKFILE_COPY_CHUNK
                         ? PAKFILE_COPY_CHUNK : remaining;
            if (fread(buffer, 1, chunk, ffd) != chunk) {
                saved_errno = errno;
                free(buffer);
                err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                         (uint32_t)saved_errno, "commit",
                         "Cannot read entry from source pak");
                err_set_entry(err, entry->filename, (size_t)-1,
                              (uint64_t)src_offset,
                              (uint64_t)entry->length);
                return PAKKA_ERR_IO;
            }
            if (fwrite(buffer, 1, chunk, tfd) != chunk) {
                saved_errno = errno;
                free(buffer);
                err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                         (uint32_t)saved_errno, "commit",
                         "Cannot write entry to dest pak");
                err_set_entry(err, entry->filename, (size_t)-1,
                              (uint64_t)src_offset,
                              (uint64_t)entry->length);
                return PAKKA_ERR_IO;
            }
            remaining -= chunk;
        }
    }

    free(buffer);
    return PAKKA_OK;
}


/* Windows reserved device names. Matched against the segment's base
 * (everything before the first '.') case-insensitively, regardless of
 * extension: "CON.txt" is still the console. COM0/LPT0 are accepted by
 * recent Windows versions; include them for safety. */
static const struct { const char *name; size_t len; } win_reserved_names[] = {
    {"CON", 3}, {"PRN", 3}, {"AUX", 3}, {"NUL", 3},
    {"COM0", 4}, {"COM1", 4}, {"COM2", 4}, {"COM3", 4}, {"COM4", 4},
    {"COM5", 4}, {"COM6", 4}, {"COM7", 4}, {"COM8", 4}, {"COM9", 4},
    {"LPT0", 4}, {"LPT1", 4}, {"LPT2", 4}, {"LPT3", 4}, {"LPT4", 4},
    {"LPT5", 4}, {"LPT6", 4}, {"LPT7", 4}, {"LPT8", 4}, {"LPT9", 4}
};

static int is_reserved_device_name(const char *seg, size_t seg_len) {
    size_t base_len, i, j;
    char a, b;

    /* Match the name up to the first '.' so "CON.txt" still hits "CON". */
    base_len = 0;
    while (base_len < seg_len && seg[base_len] != '.') {
        base_len++;
    }

    for (i = 0; i < sizeof(win_reserved_names) / sizeof(win_reserved_names[0]); i++) {
        if (win_reserved_names[i].len != base_len) continue;
        for (j = 0; j < base_len; j++) {
            a = seg[j];
            b = win_reserved_names[i].name[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (a != b) break;
        }
        if (j == base_len) return 1;
    }
    return 0;
}

/* Per-segment safety check. Rejects:
 *  - empty (would alias "foo//bar" with "foo/bar")
 *  - exact "." (aliases "./foo" with "foo") or ".." (directory escape)
 *  - control bytes 0x01-0x1F and DEL
 *  - colon anywhere in the segment (Windows ADS separator; also
 *    subsumes drive-letter checks)
 *  - trailing '.' or ' ' (Windows silently strips both, so "foo."
 *    resolves to "foo")
 *  - Windows reserved device names (CON/NUL/COM1/...); checked on
 *    every host because paks are portable
 */
static int is_unsafe_segment(const char *seg, size_t seg_len) {
    size_t i;
    unsigned char c;

    if (seg_len == 0) return 1;
    if (seg_len == 1 && seg[0] == '.') return 1;
    if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return 1;

    for (i = 0; i < seg_len; i++) {
        c = (unsigned char)seg[i];
        if (c < 0x20 || c == 0x7F) return 1;
        if (c == ':') return 1;
    }

    if (seg[seg_len - 1] == '.' || seg[seg_len - 1] == ' ') return 1;

    if (is_reserved_device_name(seg, seg_len)) return 1;

    return 0;
}

/* Collapse a pak entry name to the form Windows / HFS+ extraction would
 * see: backslashes become forward slashes, A-Z fold to a-z, trailing
 * '.' and ' ' are stripped from each segment. Applied on every host so
 * paks remain safe across destinations. dst must be at least src + 1
 * bytes; output is NUL-terminated. */
void pakka_normalize_entry_name(const char *src, char *dst, size_t dstsz) {
    size_t in = 0;
    size_t out = 0;
    size_t seg_start = 0;

    if (dstsz == 0) {
        return;
    }

    while (out + 1 < dstsz) {
        char c = src[in];
        if (c == '\0' || c == '/' || c == '\\') {
            /* End of segment: strip trailing dots and spaces */
            while (out > seg_start
                   && (dst[out - 1] == '.' || dst[out - 1] == ' ')) {
                out--;
            }
            if (c == '\0') {
                break;
            }
            dst[out++] = '/';
            seg_start = out;
            in++;
        } else {
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            dst[out++] = c;
            in++;
        }
    }
    dst[out] = '\0';
}

/* Reject pak entry names that would corrupt or redirect an extract.
 * The pak format places no constraints beyond a 56-byte upper bound,
 * so an attacker can ship "../../etc/passwd", "C:\windows\foo", "CON",
 * "foo:stream", or "trailing." in a directory entry. Both POSIX and
 * Windows path forms are checked because paks are portable. */
int pakka_unsafe_entry_name(const char *path) {
    const char *p, *seg;

    if (path == NULL || *path == '\0') {
        return 1;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }

    seg = path;
    for (p = path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            if (is_unsafe_segment(seg, (size_t)(p - seg))) {
                return 1;
            }
            if (*p == '\0') {
                break;
            }
            seg = p + 1;
        }
    }

    return 0;
}

Pakfileentry_t *find_tail(Pak_t *pak) {
    Pakfileentry_t *tail;

    if (pak->head == NULL) {
        return NULL;
    }

    if (pak->head->next == NULL) {
        return pak->head;
    }

    tail = pak->head->next;

    while (tail->next != NULL) { tail = tail->next; }

    return tail;
}

/* Walk every entry and return the highest offset+length seen — i.e. the
 * byte one past the last live payload byte in the pak. Quake's original
 * pak0.pak has entries in non-sequential byte order with an orphan payload
 * past the directory, so the directory tail's offset+length is not safe
 * as an append point. load_directory has already validated every entry
 * against file_size, so the arithmetic here cannot overflow u32. */
uint64_t compute_payload_end(Pak_t *pak) {
    Pakfileentry_t *e;
    uint64_t max_end = PAKFILE_HEADER_SIZE;
    uint64_t end;

    for (e = pak->head; e != NULL; e = e->next) {
        end = (uint64_t)e->offset + (uint64_t)e->length;
        if (end > max_end) {
            max_end = end;
        }
    }

    return max_end;
}

void init_pak_header(Pak_t *pak) {
    memcpy(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN);
    pak->diroffset = PAKFILE_HEADER_SIZE;
    pak->dirlength = 0;
}

/* Serialize one pak header to pak->fp. Writes exactly
 * PAKFILE_HEADER_SIZE bytes — the in-memory Pak_t struct has additional
 * bookkeeping fields after dirlength that must not leak to disk. */
static pakka_status_t write_pak_header(Pak_t *pak, pakka_error_t *err) {
    int saved_errno;
    if (fwrite(pak->signature, PAKFILE_SIGNATURE_LEN, 1, pak->fp) != 1
        || pakka_write_u32_le(pak->fp, pak->diroffset) != 0
        || pakka_write_u32_le(pak->fp, pak->dirlength) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "commit",
                        "Cannot write pak header");
    }
    return PAKKA_OK;
}

/* Serialize one directory entry: 56-byte filename field (NUL-padded if
 * shorter) followed by two LE u32s. Written field-by-field instead of
 * struct-as-bytes so the in-memory filename[] buffer can be a different
 * size than the on-disk PAKFILE_PATH_MAX without breaking the format. */
static pakka_status_t write_pak_entry(Pak_t *pak, Pakfileentry_t *entry,
                                      pakka_error_t *err) {
    char name_field[PAKFILE_PATH_MAX];
    size_t len = strlen(entry->filename);
    int saved_errno;

    if (len > PAKFILE_PATH_MAX) {
        len = PAKFILE_PATH_MAX;
    }
    memcpy(name_field, entry->filename, len);
    if (len < PAKFILE_PATH_MAX) {
        memset(name_field + len, 0, PAKFILE_PATH_MAX - len);
    }

    if (fwrite(name_field, PAKFILE_PATH_MAX, 1, pak->fp) != 1
        || pakka_write_u32_le(pak->fp, entry->offset) != 0
        || pakka_write_u32_le(pak->fp, entry->length) != 0) {
        saved_errno = errno;
        err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                 (uint32_t)saved_errno, "commit",
                 "Cannot write directory entry");
        err_set_entry(err, entry->filename, (size_t)-1, 0,
                      (uint64_t)entry->length);
        return PAKKA_ERR_IO;
    }
    return PAKKA_OK;
}

pakka_status_t write_pak_directory(Pak_t *pak, pakka_error_t *err) {
    Pakfileentry_t *current = pak->head;
    Pakfileentry_t *tail;
    pakka_status_t s;
    int saved_errno;
    uint64_t total_dirlength;
    Pakfileentry_t *count_it;

    /* Empty pak still needs a valid PACK header so the file isn't malformed.
     * This happens after a delete that removes every entry. */
    if (current == NULL) {
        pak->diroffset = PAKFILE_HEADER_SIZE;
        pak->dirlength = 0;
        if (pakka_compat_fseek(pak->fp, 0, SEEK_SET) != 0) {
            saved_errno = errno;
            return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                            (uint32_t)saved_errno, "commit",
                            "Cannot seek to pak header");
        }
        return write_pak_header(pak, err);
    }

    tail = find_tail(pak);
    /* Per-entry add and rebuild-copy paths already cap each entry's
     * offset+length at UINT32_MAX, so tail->offset+tail->length fits.
     * The remaining overflow is diroffset + dirlength crossing 4 GiB
     * once the directory is appended; reject before any write. */
    total_dirlength = 0;
    for (count_it = current; count_it != NULL; count_it = count_it->next) {
        total_dirlength += PAKFILE_DIR_ENTRY_SIZE;
    }
    if ((uint64_t)tail->offset + (uint64_t)tail->length + total_dirlength
            > UINT32_MAX) {
        return err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "commit",
                        "Pak directory would push file past 4 GiB");
    }

    pak->diroffset = tail->offset + tail->length;
    pak->dirlength = 0;
    if (pakka_compat_fseek(pak->fp, (int64_t)pak->diroffset, SEEK_SET) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "commit",
                        "Cannot seek to pak directory");
    }

    do {
        s = write_pak_entry(pak, current, err);
        if (s != PAKKA_OK) {
            return s;
        }
        pak->dirlength += PAKFILE_DIR_ENTRY_SIZE;
    } while ((current = current->next) != NULL);

    if (pakka_compat_fseek(pak->fp, 0, SEEK_SET) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "commit",
                        "Cannot seek to pak header");
    }

    return write_pak_header(pak, err);
}

