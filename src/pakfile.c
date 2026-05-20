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

/* Pick PK3 vs PK4 from the archive path's extension. The on-disk
 * content is byte-identical for both formats, so the filename is the
 * only available signal at open time. Case-insensitive match on `.pk4`
 * (mirrors the CLI's create-side sniffer); anything else is PK3. */
static pakka_format_t pakka_zip_format_from_path(const char *path) {
    size_t n;
    if (path == NULL) {
        return PAKKA_FORMAT_PK3;
    }
    n = strlen(path);
    if (n >= 4 && path[n - 4] == '.'
        && (path[n - 3] == 'p' || path[n - 3] == 'P')
        && (path[n - 2] == 'k' || path[n - 2] == 'K')
        && path[n - 1] == '4') {
        return PAKKA_FORMAT_PK4;
    }
    return PAKKA_FORMAT_PK3;
}

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
        pakka_entry_free(e);
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
    return pakka_open_ex(path, mode, PAKKA_FORMAT_AUTO, out, err);
}

pakka_status_t pakka_open_ex(const char *path, pakka_open_mode_t mode,
                             pakka_format_t format_hint,
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
    if (format_hint != PAKKA_FORMAT_AUTO
        && format_hint != PAKKA_FORMAT_PAK
        && format_hint != PAKKA_FORMAT_PK3
        && format_hint != PAKKA_FORMAT_PK4
        && format_hint != PAKKA_FORMAT_SIN
        && format_hint != PAKKA_FORMAT_DAIKATANA) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                        "pakka_open: unknown format_hint value %d",
                        (int)format_hint);
    }
    writable = (mode == PAKKA_OPEN_READ_WRITE) ? 1 : 0;

    pak = calloc(sizeof(Pak_t), 1);
    if (pak == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Cannot allocate pakka_archive_t");
    }
    pak->format_hint = format_hint;
    /* Default decompression cap applies to every archive; pk3file.c and
     * pakka_dk_inflate() both consult pak->max_decompressed. Previously
     * only the PK3 open path set this — DK archives, which take the
     * PAK-class open path, would otherwise inherit calloc-zero. */
    pak->max_decompressed = PK3_DEFAULT_MAX_DECOMPRESSED;

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
     * concurrent `pakka -a` invocations would otherwise both seek to
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

    /* ZIP-class path (PK3/PK4): load_pakfile dispatched into
     * pakka_pk3_open_impl which already parsed the CDR. The PAK on-disk
     * directory loader doesn't apply. Validate-no-duplicates still
     * runs — it operates on the in-memory entry list so it's
     * format-agnostic. */
    if (!pakka_format_is_zip(pak->format)) {
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

    if (format == PAKKA_FORMAT_DAIKATANA) {
        return err_fill(err, PAKKA_ERR_UNSUPPORTED,
                        PAKKA_ERR_DOMAIN_NONE, 0, "create",
                        "Daikatana archives are read-only "
                        "(custom codec has no encoder)");
    }
    if (format != PAKKA_FORMAT_PAK
        && format != PAKKA_FORMAT_PK3
        && format != PAKKA_FORMAT_PK4
        && format != PAKKA_FORMAT_SIN
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
    /* PK3/PK4 create sets pak->max_decompressed inside
     * pakka_pk3_create_impl; PAK/SiN have no compressed entries; DK
     * create is rejected above. No init needed here. */

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

    if (pakka_format_is_zip(format)) {
        pakka_status_t pk3_status;
        /* Caller owns the label: stamp PK3 vs PK4 here so pakka_format()
         * reports back what the caller asked for. pakka_pk3_create_impl
         * is shared between the two and does not touch pak->format. */
        pak->format = format;
        pk3_status = pakka_pk3_create_impl(pak, err);
        if (pk3_status != PAKKA_OK) {
            destroy_pak(pak);
            return pk3_status;
        }
        *out = pak;
        return PAKKA_OK;
    }

    /* PAK-class path (PAK / SiN). The signature stamped into the
     * initial header comes from the format geometry — "PACK" for
     * Quake/Doom-class, "SPAK" for SiN. */
    pak->format = (format == PAKKA_FORMAT_SIN) ? PAKKA_FORMAT_SIN
                                                : PAKKA_FORMAT_PAK;
    init_pak_header(pak);

    /* Write a valid empty header now so pakka_create + pakka_close with
     * no payload produces a well-formed 12-byte archive. A subsequent
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
        pakka_entry_free(e);
        e = next;
    }

    free(pak);

    return status;
}

/* Probe a candidate PAK-class layout against the on-disk directory. For
 * each entry header, advance through `dir_entry_size` bytes (which
 * varies between Q2 64-byte and DK 72-byte) and validate the embedded
 * offset+length against the file size, using compressed_size for DK
 * compressed entries. Returns PAKKA_OK if every entry parses cleanly,
 * PAKKA_ERR_FORMAT otherwise. Used by the PACK ambiguity probe. */
static pakka_status_t probe_pak_layout(Pak_t *pak,
                                       const pakka_pak_geometry_t *geom) {
    uint32_t i;
    uint32_t entry_count;
    uint64_t entry_pos;
    uint32_t off, len, dk_clen = 0, dk_flag = 0;

    if ((pak->dirlength % geom->dir_entry_size) != 0) {
        return PAKKA_ERR_FORMAT;
    }
    entry_count = pak->dirlength / (uint32_t)geom->dir_entry_size;
    if (entry_count > PAKFILE_MAX_ENTRIES) {
        return PAKKA_ERR_FORMAT;
    }

    for (i = 0; i < entry_count; i++) {
        uint64_t extent;
        entry_pos = (uint64_t)pak->diroffset + (uint64_t)i * geom->dir_entry_size;
        /* Skip the name field; we only care whether the entry's
         * offset+extent fits in the file under this candidate
         * layout. */
        if (pakka_compat_fseek(pak->fp,
                               (int64_t)(entry_pos + geom->name_field_len),
                               SEEK_SET) != 0) {
            return PAKKA_ERR_FORMAT;
        }
        if (pakka_read_u32_le(pak->fp, &off) != 0
            || pakka_read_u32_le(pak->fp, &len) != 0) {
            return PAKKA_ERR_FORMAT;
        }
        if (geom->has_compression) {
            if (pakka_read_u32_le(pak->fp, &dk_clen) != 0
                || pakka_read_u32_le(pak->fp, &dk_flag) != 0) {
                return PAKKA_ERR_FORMAT;
            }
        }
        /* For DK compressed entries the on-disk payload is
         * dk_compressed_size bytes; for everything else, `length`. */
        extent = (geom->has_compression && dk_flag != 0)
                     ? (uint64_t)dk_clen
                     : (uint64_t)len;
        if (off < PAKFILE_HEADER_SIZE
            || extent > pak->file_size
            || (uint64_t)off > pak->file_size - extent) {
            return PAKKA_ERR_FORMAT;
        }
    }
    return PAKKA_OK;
}

pakka_status_t load_pakfile(Pak_t *pak, pakka_error_t *err) {
    int64_t size;
    int saved_errno;
    const pakka_pak_geometry_t *geom;

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

    /* ZIP-class signatures: PK\3\4 = local file header (normal case),
     * PK\5\6 = empty ZIP (EOCD only). PK\7\8 (spanning) is refused
     * as multi-disk inside the ZIP loader. PK3 vs PK4 is decided by
     * the filename extension unless the caller pinned the format hint. */
    if (memcmp(pak->signature, "PK\x03\x04", PAKFILE_SIGNATURE_LEN) == 0
        || memcmp(pak->signature, "PK\x05\x06", PAKFILE_SIGNATURE_LEN) == 0) {
        if (pak->format_hint != PAKKA_FORMAT_AUTO
            && pak->format_hint != PAKKA_FORMAT_PK3
            && pak->format_hint != PAKKA_FORMAT_PK4) {
            return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "format_hint does not match on-disk ZIP magic");
        }
        if (pak->format_hint == PAKKA_FORMAT_PK3
            || pak->format_hint == PAKKA_FORMAT_PK4) {
            pak->format = pak->format_hint;
        } else {
            pak->format = pakka_zip_format_from_path(pak->cur_pakpath);
        }
        return pakka_pk3_open_impl(pak, err);
    }
    if (memcmp(pak->signature, "PK\x07\x08", PAKFILE_SIGNATURE_LEN) == 0) {
        return err_fill(err, PAKKA_ERR_UNSUPPORTED,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open",
                        "Multi-disk PK3/PK4 archives are not supported");
    }

    /* SPAK magic — SiN. The hint must be AUTO or SIN. */
    if (memcmp(pak->signature, "SPAK", PAKFILE_SIGNATURE_LEN) == 0) {
        if (pak->format_hint != PAKKA_FORMAT_AUTO
            && pak->format_hint != PAKKA_FORMAT_SIN) {
            return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "format_hint does not match SPAK magic");
        }
        pak->format = PAKKA_FORMAT_SIN;
    } else if (memcmp(pak->signature, "PACK", PAKFILE_SIGNATURE_LEN) == 0) {
        if (pak->format_hint != PAKKA_FORMAT_AUTO
            && pak->format_hint != PAKKA_FORMAT_PAK
            && pak->format_hint != PAKKA_FORMAT_DAIKATANA) {
            return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                            PAKKA_ERR_DOMAIN_NONE, 0, "open",
                            "format_hint does not match PACK magic");
        }
        /* Provisional; the AUTO probe below decides between PAK and DK. */
        pak->format = (pak->format_hint == PAKKA_FORMAT_DAIKATANA)
                          ? PAKKA_FORMAT_DAIKATANA
                          : PAKKA_FORMAT_PAK;
    } else {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Not a pak file (bad signature)");
    }

    if (pakka_read_u32_le(pak->fp, &pak->diroffset) != 0
        || pakka_read_u32_le(pak->fp, &pak->dirlength) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "open",
                        "Cannot read pak header (diroffset/dirlength)");
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

    /* Probe between Q2 PAK (64-byte entries) and Daikatana (72-byte
     * entries) for PACK magic. SPAK is unambiguous. Empty archive
     * (dirlength == 0) parses cleanly under both layouts — bias to PAK
     * unless DAIKATANA was explicitly requested. */
    if (pak->format != PAKKA_FORMAT_SIN
        && pak->format_hint == PAKKA_FORMAT_AUTO) {
        pakka_status_t ok_pak = PAKKA_ERR_FORMAT;
        pakka_status_t ok_dk  = PAKKA_ERR_FORMAT;

        if (pak->dirlength == 0) {
            pak->format = PAKKA_FORMAT_PAK;
        } else {
            ok_pak = probe_pak_layout(pak, pakka_pak_geometry(PAKKA_FORMAT_PAK));
            ok_dk  = probe_pak_layout(pak, pakka_pak_geometry(PAKKA_FORMAT_DAIKATANA));
            if (ok_pak == PAKKA_OK && ok_dk == PAKKA_OK) {
                return err_fill(err, PAKKA_ERR_FORMAT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "Ambiguous PACK archive (parses as both "
                                "Quake and Daikatana); pass --format pak "
                                "or --format daikatana");
            }
            if (ok_pak == PAKKA_OK) {
                pak->format = PAKKA_FORMAT_PAK;
            } else if (ok_dk == PAKKA_OK) {
                pak->format = PAKKA_FORMAT_DAIKATANA;
            } else {
                return err_fill(err, PAKKA_ERR_FORMAT,
                                PAKKA_ERR_DOMAIN_NONE, 0, "open",
                                "PACK archive does not parse as Quake or "
                                "Daikatana (entry offset/length out of range)");
            }
        }
    }

    geom = pakka_pak_geometry(pak->format);
    if (geom == NULL) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open", "Internal: no geometry for format %d",
                        (int)pak->format);
    }
    if ((pak->dirlength % geom->dir_entry_size) != 0) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Pak header is corrupt (dirlength not a multiple of %zu)",
                        geom->dir_entry_size);
    }
    pak->num_entries = pak->dirlength / (uint32_t)geom->dir_entry_size;

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
    const pakka_pak_geometry_t *geom;
    uint64_t extent;

    if (pak->num_entries == 0) {
        return PAKKA_OK;
    }

    geom = pakka_pak_geometry(pak->format);
    if (geom == NULL) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open",
                        "Internal: no geometry for format %d",
                        (int)pak->format);
    }

    for (i = 1; i <= pak->num_entries; i++) {
        /* Compute in 64-bit to avoid u32 wrap. load_pakfile has already
         * validated diroffset+dirlength <= file_size and num_entries
         * against the dirlength, so the subtraction can't underflow. */
        entry_pos = (uint64_t)pak->diroffset + (uint64_t)pak->dirlength
                    - (uint64_t)i * geom->dir_entry_size;

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

        if (fread(current->filename, geom->name_field_len, 1, pak->fp) != 1
            || pakka_read_u32_le(pak->fp, &current->offset) != 0
            || pakka_read_u32_le(pak->fp, &current->length) != 0) {
            saved_errno = errno;
            pakka_entry_free(current);
            err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                     (uint32_t)saved_errno, "open",
                     "Cannot read pak directory entry %" PRIu32, i);
            err_set_entry(err, NULL, (size_t)i - 1, entry_pos, 0);
            return PAKKA_ERR_IO;
        }
        if (geom->has_compression) {
            uint32_t flag_u32;
            if (pakka_read_u32_le(pak->fp, &current->dk_compressed_size) != 0
                || pakka_read_u32_le(pak->fp, &flag_u32) != 0) {
                saved_errno = errno;
                pakka_entry_free(current);
                err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                         (uint32_t)saved_errno, "open",
                         "Cannot read DK directory entry %" PRIu32
                         " (compressed_size/flag)", i);
                err_set_entry(err, NULL, (size_t)i - 1, entry_pos, 0);
                return PAKKA_ERR_IO;
            }
            current->dk_is_compressed = (flag_u32 != 0) ? 1 : 0;
        }

        /* The on-disk filename is exactly name_field_len bytes with no
         * guaranteed NUL. The in-memory buffer is one byte larger so we
         * can force termination before any strlen/strcmp/printf("%s")
         * downstream overreads into the offset/length fields. */
        current->filename[geom->name_field_len] = '\0';

        /* On-disk extent: for DK compressed entries this is the
         * compressed payload size; for everything else it's `length`.
         * Bounds-check against the file we actually have on disk so
         * subsequent extract/copy paths can trust the stored values
         * without re-validating. */
        extent = (current->dk_is_compressed != 0)
                     ? (uint64_t)current->dk_compressed_size
                     : (uint64_t)current->length;
        if (current->offset < PAKFILE_HEADER_SIZE
            || extent > pak->file_size
            || (uint64_t)current->offset > pak->file_size - extent) {
            err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                     "open",
                     "Pak entry %" PRIu32 " bytes out of range "
                     "(offset=%" PRIu32 ", length=%" PRIu32 ")",
                     i, current->offset, current->length);
            err_set_entry(err, current->filename, (size_t)i - 1,
                          (uint64_t)current->offset,
                          (uint64_t)current->length);
            pakka_entry_free(current);
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
    /* Check the live-handle state BEFORE the name lookup so a closed
     * archive surfaces the same "no live file handle" error
     * regardless of whether the requested name exists. Without this,
     * a closed archive + missing name returns NOT_FOUND, which is
     * misleading. */
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

    return pakka_open_entry_handle(archive, entry, out, err);
}

pakka_status_t pakka_open_entry_handle(pakka_archive_t *archive,
                                       const pakka_entry_t *entry,
                                       pakka_reader_t **out,
                                       pakka_error_t *err) {
    pakka_reader_t *reader;
    const char *entry_name;

    if (out != NULL) {
        *out = NULL;
    }
    if (archive == NULL || entry == NULL || out == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open_entry_handle",
                        "pakka_open_entry_handle: archive, entry, "
                        "and out must be non-NULL");
    }
    if (archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "open_entry_handle",
                        "pakka_open_entry_handle: archive has no live "
                        "file handle (closed or commit-failed)");
    }
    entry_name = entry->filename;

    reader = calloc(1, sizeof(pakka_reader_t));
    if (reader == NULL) {
        return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                        "open_entry_handle",
                        "Cannot allocate pakka_reader_t for %s",
                        entry_name);
    }

    reader->archive = archive;
    reader->next_offset = (uint64_t)entry->offset;
    reader->remaining = (uint64_t)entry->length;

    if (pakka_format_is_zip(archive->format)) {
        pakka_status_t pk3_status = pakka_pk3_open_entry_impl(
            archive, reader, (pakka_entry_t *)entry, err);
        if (pk3_status != PAKKA_OK) {
            free(reader);
            return pk3_status;
        }
    } else if (archive->format == PAKKA_FORMAT_DAIKATANA
               && entry->dk_is_compressed) {
        /* Read compressed payload + inflate into reader->inflated_buf.
         * Subsequent pakka_reader_read serves bytes through the
         * inflated_cursor — same machinery PK3 DEFLATE uses. */
        unsigned char *compressed = NULL;
        unsigned char *uncompressed = NULL;
        uint32_t clen = entry->dk_compressed_size;
        uint32_t ulen = entry->length;
        pakka_status_t dk_status;
        int saved_errno;

        if (archive->max_decompressed > 0) {
            if ((uint64_t)ulen > archive->max_decompressed
                || (uint64_t)clen > archive->max_decompressed) {
                err_fill(err, PAKKA_ERR_LIMIT,
                         PAKKA_ERR_DOMAIN_NONE, 0, "open_entry",
                         "Daikatana entry %s exceeds max_decompressed_size "
                         "(uncompressed=%u, compressed=%u, cap=%" PRIu64 ")",
                         entry_name, (unsigned)ulen, (unsigned)clen,
                         archive->max_decompressed);
                err_set_entry(err, entry_name, (size_t)-1,
                              (uint64_t)entry->offset, (uint64_t)ulen);
                free(reader);
                return PAKKA_ERR_LIMIT;
            }
        }

        compressed = (clen > 0) ? malloc(clen) : NULL;
        if (clen > 0 && compressed == NULL) {
            err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE, 0,
                     "open_entry",
                     "Cannot allocate DK compressed buffer (%u bytes)",
                     (unsigned)clen);
            free(reader);
            return PAKKA_ERR_NOMEM;
        }
        if (clen > 0) {
            if (pakka_compat_fseek(archive->fp,
                                   (int64_t)entry->offset, SEEK_SET) != 0) {
                saved_errno = errno;
                free(compressed);
                free(reader);
                return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "open_entry",
                                "Cannot seek to DK payload for %s",
                                entry_name);
            }
            if (fread(compressed, 1, clen, archive->fp) != clen) {
                saved_errno = errno;
                free(compressed);
                free(reader);
                return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "open_entry",
                                "Cannot read DK payload for %s",
                                entry_name);
            }
        }

        uncompressed = (ulen > 0) ? malloc(ulen) : NULL;
        if (ulen > 0 && uncompressed == NULL) {
            free(compressed);
            free(reader);
            return err_fill(err, PAKKA_ERR_NOMEM, PAKKA_ERR_DOMAIN_NONE,
                            0, "open_entry",
                            "Cannot allocate DK output buffer (%u bytes)",
                            (unsigned)ulen);
        }

        dk_status = pakka_dk_inflate(compressed, clen,
                                     uncompressed, ulen, err);
        free(compressed);
        if (dk_status != PAKKA_OK) {
            if (err != NULL) {
                err_set_entry(err, entry_name, (size_t)-1,
                              (uint64_t)entry->offset, (uint64_t)ulen);
            }
            free(uncompressed);
            free(reader);
            return dk_status;
        }

        reader->inflated_buf = uncompressed;
        reader->inflated_len = ulen;
        reader->inflated_cursor = 0;
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

    /* ZIP reader (inflated buffer for DEFLATE, otherwise STORED stream). */
    if (pakka_format_is_zip(reader->archive->format)) {
        return pakka_pk3_reader_read_impl(reader, buf, len, nread, err);
    }

    /* Daikatana compressed entries: serve from the pre-inflated buffer
     * populated in pakka_open_entry. STORED DK entries fall through to
     * the same fread loop the Quake PAK path uses. */
    if (reader->inflated_buf != NULL) {
        size_t available = reader->inflated_len - reader->inflated_cursor;
        chunk = (len < available) ? len : available;
        if (chunk == 0) {
            return PAKKA_OK;
        }
        memcpy(buf, reader->inflated_buf + reader->inflated_cursor, chunk);
        reader->inflated_cursor += chunk;
        reader->remaining -= (uint64_t)chunk;
        *nread = chunk;
        return PAKKA_OK;
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
    /* ZIP readers may hold a malloc'd inflated buffer for DEFLATE
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
    archive->max_decompressed = max_bytes;
    return PAKKA_OK;
}

/* Daikatana archives are read-only. The custom codec has no published
 * encoder, so add/delete/commit on a DK pak would either corrupt the
 * file (rebuild-copy treats `length` as the byte extent — wrong for
 * compressed entries) or require encoding bytes we can't encode. Reject
 * before any state mutation so a later pakka_close implicit-commit is a
 * no-op. */
static pakka_status_t refuse_dk_mutation(pakka_archive_t *archive,
                                         const char *operation,
                                         pakka_error_t *err) {
    if (archive != NULL && archive->format == PAKKA_FORMAT_DAIKATANA) {
        return err_fill(err, PAKKA_ERR_UNSUPPORTED,
                        PAKKA_ERR_DOMAIN_NONE, 0, operation,
                        "Daikatana archives are read-only "
                        "(custom codec has no encoder)");
    }
    return PAKKA_OK;
}

/* Format-specific entry-name length cap. PAK rows are sized by
 * geometry (Quake 56, SiN 120); ZIP uses PAKKA_ENTRY_NAME_SIZE (256). */
static size_t pak_entry_name_cap(const Pak_t *archive) {
    if (pakka_format_is_zip(archive->format)) {
        return PAKKA_ENTRY_NAME_SIZE;
    } else {
        const pakka_pak_geometry_t *g = pakka_pak_geometry(archive->format);
        return (g != NULL) ? g->name_field_len : PAKFILE_PATH_MAX;
    }
}

/* Cross-format add preflight: entry-name cap + safety + duplicate.
 * Runs for both PAK and ZIP before format dispatch so a single source
 * of truth covers every add. Order matches the pre-libpakka PAK path:
 * name-cap first (most specific), unsafe-name, then duplicate. Drift
 * here previously let .pk3/.pk4 adds skip source-side hardening — keep
 * new checks in one branch only. */
static pakka_status_t pak_add_preflight(Pak_t *archive,
                                        const char *entry_name,
                                        const char *op_name,
                                        pakka_error_t *err) {
    size_t name_len = strlen(entry_name);
    size_t name_cap = pak_entry_name_cap(archive);
    if (name_len >= name_cap) {
        err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                 op_name, "Entry name too long for pak "
                 "(%zu bytes, max %zu)", name_len, name_cap - 1);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_LIMIT;
    }
    if (pakka_unsafe_entry_name(entry_name)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 op_name, "Entry name is not safe to extract: %s",
                 entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }
    if (find_entry(archive, (char *)entry_name) != NULL) {
        err_fill(err, PAKKA_ERR_DUPLICATE, PAKKA_ERR_DOMAIN_NONE, 0,
                 op_name, "Duplicate entry: %s already exists in pak",
                 entry_name);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_DUPLICATE;
    }
    return PAKKA_OK;
}

/* Source-side hardening: reject symlink/reparse points and any
 * non-regular file (FIFO, socket, char/block device). add_memory has no
 * source path so it skips this; add_file calls it for both PAK and ZIP. */
static pakka_status_t pak_add_source_preflight(const char *source_path,
                                               const char *entry_name,
                                               const char *op_name,
                                               pakka_error_t *err) {
    struct stat sb;
    int saved_errno;
    if (pakka_compat_is_reparse_or_symlink(source_path)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 op_name, "Refusing symlink/reparse source: %s",
                 source_path);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }
    if (pakka_compat_lstat(source_path, &sb) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, op_name,
                        "Cannot stat source %s", source_path);
    }
    if (!S_ISREG(sb.st_mode)) {
        err_fill(err, PAKKA_ERR_UNSAFE_NAME, PAKKA_ERR_DOMAIN_NONE, 0,
                 op_name, "Source is not a regular file: %s",
                 source_path);
        err_set_entry(err, entry_name, (size_t)-1, 0, 0);
        return PAKKA_ERR_UNSAFE_NAME;
    }
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
    pakka_status_t dk_s;
    pakka_status_t pre_s;

    if (archive == NULL || source_path == NULL || entry_name == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_file",
                        "pakka_add_file: archive, source_path, "
                        "and entry_name must be non-NULL");
    }
    dk_s = refuse_dk_mutation(archive, "add_file", err);
    if (dk_s != PAKKA_OK) return dk_s;
    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_file",
                        "pakka_add_file: archive not writable "
                        "or no live file handle");
    }

    pre_s = pak_add_preflight(archive, entry_name, "add_file", err);
    if (pre_s != PAKKA_OK) return pre_s;
    pre_s = pak_add_source_preflight(source_path, entry_name,
                                     "add_file", err);
    if (pre_s != PAKKA_OK) return pre_s;

    if (pakka_format_is_zip(archive->format)) {
        return pakka_pk3_add_file_impl(archive, source_path, entry_name,
                                       err);
    }

    /* PAK path. Preflight already enforced the name cap for this
     * format's row (Quake 56, SiN 120). */
    name_len = strlen(entry_name);

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
        pakka_entry_free(entry);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_file",
                        "Cannot seek to append point in pak");
    }

    if (src_size > 0) {
        bytes = malloc(PAKFILE_COPY_CHUNK);
        if (bytes == NULL) {
            fclose(src_fp);
            pakka_entry_free(entry);
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
                pakka_entry_free(entry);
                return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                                (uint32_t)saved_errno, "add_file",
                                "Cannot read %s", source_path);
            }
            if (fwrite(bytes, 1, chunk, archive->fp) != chunk) {
                saved_errno = errno;
                free(bytes);
                fclose(src_fp);
                pakka_entry_free(entry);
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
    pakka_status_t dk_s;

    if (archive == NULL || entry_name == NULL || (data == NULL && len > 0)) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_memory",
                        "pakka_add_memory: archive and entry_name must "
                        "be non-NULL; data may be NULL only when len == 0");
    }
    dk_s = refuse_dk_mutation(archive, "add_memory", err);
    if (dk_s != PAKKA_OK) return dk_s;
    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "add_memory",
                        "pakka_add_memory: archive not writable "
                        "or no live file handle");
    }

    {
        pakka_status_t pre_s = pak_add_preflight(archive, entry_name,
                                                 "add_memory", err);
        if (pre_s != PAKKA_OK) return pre_s;
    }

    if (pakka_format_is_zip(archive->format)) {
        return pakka_pk3_add_memory_impl(archive, entry_name, data, len,
                                         err);
    }

    /* PAK path. Preflight already enforced the name cap. */
    name_len = strlen(entry_name);
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
        pakka_entry_free(entry);
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "add_memory",
                        "Cannot seek to append point in pak");
    }
    if (len > 0 && fwrite(data, 1, len, archive->fp) != len) {
        saved_errno = errno;
        pakka_entry_free(entry);
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
    /* Enforce the configured cap BEFORE the malloc, but only for
     * formats where pakka_open_entry would also enforce it: ZIP
     * (any method) and DK compressed. PAK / SiN / DK-STORED are
     * uncompressed and the public docs at include/pakka.h say the
     * cap does not apply there — so allocate without a gate. Without
     * this restriction we'd reject a legitimate >64 MiB PAK read at
     * the default cap. */
    {
        int cap_applies = pakka_format_is_zip(archive->format)
            || (archive->format == PAKKA_FORMAT_DAIKATANA
                && entry->dk_is_compressed);
        if (cap_applies
            && archive->max_decompressed > 0
            && total > archive->max_decompressed) {
            err_fill(err, PAKKA_ERR_LIMIT, PAKKA_ERR_DOMAIN_NONE, 0,
                     "read_entry_alloc",
                     "Entry %s size (%" PRIu64 ") exceeds "
                     "max_decompressed_size (%" PRIu64 ")",
                     entry_name, total, archive->max_decompressed);
            err_set_entry(err, entry_name, (size_t)-1, 0, total);
            return PAKKA_ERR_LIMIT;
        }
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
    pakka_status_t dk_s;

    if (archive == NULL || entry_name == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "delete",
                        "pakka_delete: archive and entry_name "
                        "must be non-NULL");
    }
    dk_s = refuse_dk_mutation(archive, "delete", err);
    if (dk_s != PAKKA_OK) return dk_s;
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
            pakka_entry_free(current);
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

    {
        pakka_status_t dk_s = refuse_dk_mutation(archive, "commit", err);
        if (dk_s != PAKKA_OK) return dk_s;
    }

    if (!archive->dirty) {
        return PAKKA_OK;        /* no-op */
    }

    if (!archive->writable || archive->fp == NULL) {
        return err_fill(err, PAKKA_ERR_INVALID_ARGUMENT,
                        PAKKA_ERR_DOMAIN_NONE, 0, "commit",
                        "pakka_commit: archive not writable or fp closed");
    }

    if (pakka_format_is_zip(archive->format)) {
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

/* Combined verify-report + first-error capture. Every error-level
 * finding in pakka_verify needs to: notify the caller's report
 * callback, populate the out pakka_error_t on the first error so a
 * non-callback caller can still see a structured failure, and set
 * the sticky first_error so the function returns the first error
 * status rather than the last. Folding all three into one call
 * removes ~8 lines of boilerplate per finding site (~70 LOC across
 * pakka_verify). */
static void verify_record_error(pakka_report_fn report, void *userdata,
                                pakka_status_t status,
                                pakka_error_t *err,
                                pakka_status_t *first_error,
                                const char *filename,
                                pakka_error_domain_t domain,
                                uint32_t system_code,
                                uint64_t offset, uint64_t length,
                                const char *fmt, ...) {
    char buf[PAKKA_MESSAGE_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (report != NULL) {
        report(userdata, PAKKA_REPORT_ERROR, status, filename, buf);
    }
    if (*first_error == PAKKA_OK) {
        err_fill(err, status, domain, system_code, "verify", "%s", buf);
        err_set_entry(err, filename, (size_t)-1, offset, length);
        *first_error = status;
    }
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
            verify_record_error(report, userdata, PAKKA_ERR_UNSAFE_NAME,
                                err, &first_error, current->filename,
                                PAKKA_ERR_DOMAIN_NONE, 0,
                                (uint64_t)current->offset,
                                (uint64_t)current->length,
                                "Entry name is not safe to extract");
            continue;        /* skip payload read for unsafe names */
        }

        if (current->length == 0) {
            verify_report(report, userdata, PAKKA_REPORT_INFO,
                          PAKKA_OK, current->filename,
                          "OK (0 bytes)");
            continue;
        }

        /* Skip queued-add ZIP entries (H2): their payload isn't on
         * pak->fp yet — it lives in pk3_pending_source/_data and gets
         * published at commit time. A seek to pk3_payload_offset == 0
         * would read unrelated header bytes and report spurious
         * findings. Pending entries are visible to pakka_verify so
         * the entry-count is honest, just without an on-disk read. */
        if (pakka_format_is_zip(archive->format)
            && (current->pk3_pending_source != NULL
                || current->pk3_pending_data != NULL)) {
            verify_report(report, userdata, PAKKA_REPORT_INFO,
                          PAKKA_OK, current->filename,
                          "pending add (not yet committed)");
            continue;
        }

        /* ZIP entries (PK3/PK4): on-disk payload is pk3_compressed_size bytes
         * at pk3_payload_offset; entry->length is the UNCOMPRESSED
         * size (which the PAK reads-stream-length convention doesn't
         * match for DEFLATE entries). DK compressed entries use
         * dk_compressed_size for the same reason — entry->length is the
         * uncompressed payload size in that case. */
        {
            uint32_t verify_offset = current->offset;
            uint32_t verify_length = current->length;
            if (pakka_format_is_zip(archive->format)) {
                verify_offset = current->pk3_payload_offset;
                verify_length = current->pk3_compressed_size;
            } else if (current->dk_is_compressed) {
                verify_length = current->dk_compressed_size;
            }
            if (pakka_compat_fseek(archive->fp, (int64_t)verify_offset, SEEK_SET) != 0) {
                int seek_errno = errno;
                verify_record_error(report, userdata, PAKKA_ERR_IO, err,
                                    &first_error, current->filename,
                                    PAKKA_ERR_DOMAIN_ERRNO,
                                    (uint32_t)seek_errno,
                                    (uint64_t)verify_offset,
                                    (uint64_t)verify_length,
                                    "Cannot seek to entry payload at "
                                    "offset %" PRIu32, verify_offset);
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
                        verify_record_error(report, userdata, PAKKA_ERR_IO,
                                            err, &first_error,
                                            current->filename,
                                            PAKKA_ERR_DOMAIN_ERRNO,
                                            (uint32_t)read_errno,
                                            at, (uint64_t)verify_length,
                                            "Cannot read entry payload "
                                            "(short read at offset %"
                                            PRIu64 ")", at);
                        io_failed = 1;
                        break;
                    }
                    remaining -= chunk;
                }
                if (!io_failed) {
                    /* Deep verify (ZIP only): decompress + CRC32 check.
                     * Structural read above already confirmed the
                     * compressed bytes are readable; this catches
                     * bit-rot in the payload, CRC mismatch, and
                     * trailing junk in the deflate stream. */
                    if (pakka_format_is_zip(archive->format)
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
                    /* Deep verify for Daikatana compressed entries:
                     * decode through pakka_dk_inflate() and confirm the
                     * produced byte count matches entry->length. DK has
                     * no per-entry CRC, so byte count is the only
                     * post-decode signal available. */
                    if (archive->format == PAKKA_FORMAT_DAIKATANA
                        && current->dk_is_compressed
                        && (flags & PAKKA_VERIFY_DEEP)) {
                        pakka_error_t deep_err;
                        pakka_status_t dv;
                        unsigned char *cbuf = NULL;
                        unsigned char *ubuf = NULL;
                        uint32_t clen = current->dk_compressed_size;
                        uint32_t ulen = current->length;

                        /* Same cap that pakka_open_entry enforces.
                         * Without this, --verify --deep on a hostile DK
                         * archive can allocate ulen bytes regardless of
                         * pakka_set_max_decompressed_size. */
                        if (archive->max_decompressed > 0
                            && ((uint64_t)ulen > archive->max_decompressed
                                || (uint64_t)clen > archive->max_decompressed)) {
                            verify_record_error(report, userdata,
                                                PAKKA_ERR_LIMIT, err,
                                                &first_error,
                                                current->filename,
                                                PAKKA_ERR_DOMAIN_NONE, 0,
                                                0, (uint64_t)ulen,
                                                "DK entry exceeds "
                                                "max_decompressed_size "
                                                "(uncompressed=%u, "
                                                "compressed=%u, "
                                                "cap=%" PRIu64 ")",
                                                (unsigned)ulen,
                                                (unsigned)clen,
                                                archive->max_decompressed);
                            continue;
                        }

                        cbuf = (clen > 0) ? malloc(clen) : NULL;
                        if (clen > 0 && cbuf == NULL) {
                            verify_report(report, userdata,
                                          PAKKA_REPORT_ERROR,
                                          PAKKA_ERR_NOMEM, current->filename,
                                          "Cannot allocate DK deep-verify "
                                          "compressed buffer (%u bytes)",
                                          (unsigned)clen);
                            if (first_error == PAKKA_OK) {
                                first_error = PAKKA_ERR_NOMEM;
                            }
                            continue;
                        }
                        if (clen > 0) {
                            if (pakka_compat_fseek(archive->fp,
                                                   (int64_t)current->offset,
                                                   SEEK_SET) != 0
                                || fread(cbuf, 1, clen, archive->fp) != clen) {
                                free(cbuf);
                                verify_report(report, userdata,
                                              PAKKA_REPORT_ERROR,
                                              PAKKA_ERR_IO, current->filename,
                                              "Cannot reread DK payload for "
                                              "deep verify");
                                if (first_error == PAKKA_OK) {
                                    first_error = PAKKA_ERR_IO;
                                }
                                continue;
                            }
                        }
                        ubuf = (ulen > 0) ? malloc(ulen) : NULL;
                        if (ulen > 0 && ubuf == NULL) {
                            free(cbuf);
                            verify_report(report, userdata,
                                          PAKKA_REPORT_ERROR,
                                          PAKKA_ERR_NOMEM, current->filename,
                                          "Cannot allocate DK deep-verify "
                                          "output buffer (%u bytes)",
                                          (unsigned)ulen);
                            if (first_error == PAKKA_OK) {
                                first_error = PAKKA_ERR_NOMEM;
                            }
                            continue;
                        }
                        dv = pakka_dk_inflate(cbuf, clen, ubuf, ulen,
                                              &deep_err);
                        free(cbuf);
                        free(ubuf);
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
                verify_record_error(report, userdata,
                                    PAKKA_ERR_DUPLICATE, err,
                                    &first_error, recs[i].original,
                                    PAKKA_ERR_DOMAIN_NONE, 0, 0, 0,
                                    "Normalized collision with '%s'",
                                    recs[i - 1].original);
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
 * The on-disk format places no constraints on byte content (only on
 * field width, which varies per format), so an attacker can ship
 * "../../etc/passwd", "C:\windows\foo", "CON", "foo:stream", or
 * "trailing." in a directory entry. This validator is format-
 * independent — it inspects the bytes, not the field — and both POSIX
 * and Windows path forms are checked because paks are portable. */
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
    const pakka_pak_geometry_t *geom = pakka_pak_geometry(pak->format);
    /* Default to PACK when the format hasn't resolved to a PAK-class
     * row yet (defense-in-depth — every caller that hits this function
     * sets pak->format first). */
    const char *sig = (geom != NULL) ? geom->signature : "PACK";
    memcpy(pak->signature, sig, PAKFILE_SIGNATURE_LEN);
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

/* Serialize one directory entry: name field (NUL-padded if shorter)
 * followed by two LE u32s, plus a third+fourth u32 for Daikatana
 * (compressed_size + is_compressed). Written field-by-field instead of
 * struct-as-bytes so the in-memory filename[] buffer can be a different
 * size than the on-disk name_field_len without breaking the format.
 *
 * The name_field scratch is sized for the widest row (SiN's 120 bytes
 * fit inside PAKKA_ENTRY_NAME_SIZE) so this remains a fixed-size stack
 * buffer — C99 VLAs are not supported on MSVC. */
static pakka_status_t write_pak_entry(Pak_t *pak, Pakfileentry_t *entry,
                                      pakka_error_t *err) {
    char name_field[PAKKA_ENTRY_NAME_SIZE];
    const pakka_pak_geometry_t *geom = pakka_pak_geometry(pak->format);
    size_t cap;
    size_t len;
    int saved_errno;

    if (geom == NULL) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "commit",
                        "Internal: write_pak_entry without geometry");
    }
    cap = geom->name_field_len;
    len = strlen(entry->filename);
    if (len > cap) {
        len = cap;
    }
    memcpy(name_field, entry->filename, len);
    if (len < cap) {
        memset(name_field + len, 0, cap - len);
    }

    if (fwrite(name_field, cap, 1, pak->fp) != 1
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
    if (geom->has_compression) {
        if (pakka_write_u32_le(pak->fp, entry->dk_compressed_size) != 0
            || pakka_write_u32_le(pak->fp,
                                  (uint32_t)entry->dk_is_compressed) != 0) {
            saved_errno = errno;
            err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                     (uint32_t)saved_errno, "commit",
                     "Cannot write DK directory entry fields");
            err_set_entry(err, entry->filename, (size_t)-1, 0,
                          (uint64_t)entry->length);
            return PAKKA_ERR_IO;
        }
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
    const pakka_pak_geometry_t *geom = pakka_pak_geometry(pak->format);
    uint32_t entry_size;

    if (geom == NULL) {
        return err_fill(err, PAKKA_ERR_FORMAT, PAKKA_ERR_DOMAIN_NONE, 0,
                        "commit",
                        "Internal: write_pak_directory without geometry");
    }
    entry_size = (uint32_t)geom->dir_entry_size;

    /* Empty pak still needs a valid header (PACK or SPAK) so the file
     * isn't malformed. This happens after a delete that removes every
     * entry. */
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
        total_dirlength += entry_size;
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
        pak->dirlength += entry_size;
    } while ((current = current->next) != NULL);

    if (pakka_compat_fseek(pak->fp, 0, SEEK_SET) != 0) {
        saved_errno = errno;
        return err_fill(err, PAKKA_ERR_IO, PAKKA_ERR_DOMAIN_ERRNO,
                        (uint32_t)saved_errno, "commit",
                        "Cannot seek to pak header");
    }

    return write_pak_header(pak, err);
}

