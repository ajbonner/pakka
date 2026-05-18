#ifndef PAKKA_H
#define PAKKA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer sizes, in bytes, including NUL terminator. Usable string payload
 * is one byte less than the *_SIZE value. Mirrors the internal
 * PAKFILE_PATH_MAX / PAKFILE_PATH_BUF split in src/common.h. */
#define PAKKA_ENTRY_NAME_SIZE 256u
#define PAKKA_OPERATION_SIZE   48u
#define PAKKA_MESSAGE_SIZE   1024u

typedef struct pakka_archive pakka_archive_t;
typedef struct pakka_entry pakka_entry_t;
typedef struct pakka_reader pakka_reader_t;
typedef struct pakka_error pakka_error_t;

typedef enum {
    PAKKA_FORMAT_AUTO = 0,
    PAKKA_FORMAT_PAK,
    PAKKA_FORMAT_PK3
} pakka_format_t;

typedef enum {
    PAKKA_OPEN_READ       = 0,
    PAKKA_OPEN_READ_WRITE = 1
} pakka_open_mode_t;

/* Reserved for future bits (exclusive-create, atomic-replace, etc.).
 * V1 accepts only PAKKA_CREATE_DEFAULT; unknown bits return
 * PAKKA_ERR_INVALID_ARGUMENT. Likewise for pakka_open_mode_t values
 * outside the defined enumerators. */
#define PAKKA_CREATE_DEFAULT 0u

typedef enum {
    PAKKA_OK = 0,
    PAKKA_ERR_IO,
    PAKKA_ERR_FORMAT,
    PAKKA_ERR_UNSUPPORTED,
    PAKKA_ERR_UNSAFE_NAME,
    PAKKA_ERR_DUPLICATE,
    PAKKA_ERR_NOT_FOUND,
    PAKKA_ERR_EXISTS,
    PAKKA_ERR_NOMEM,
    PAKKA_ERR_LIMIT,            /* exceeded a documented cap (entry count, file size) */
    PAKKA_ERR_INVALID_ARGUMENT  /* caller passed an unknown mode/flag or NULL where required */
} pakka_status_t;

typedef enum {
    PAKKA_ERR_DOMAIN_NONE  = 0,
    PAKKA_ERR_DOMAIN_ERRNO = 1,
    PAKKA_ERR_DOMAIN_WIN32 = 2
} pakka_error_domain_t;

struct pakka_error {
    pakka_status_t       status;
    pakka_error_domain_t domain;
    uint32_t             system_code;
    char                 operation[PAKKA_OPERATION_SIZE];
    char                 entry_name[PAKKA_ENTRY_NAME_SIZE];
    int                  entry_name_truncated;
    size_t               entry_index;
    uint64_t             offset;
    uint64_t             length;
    int                  message_truncated;
    char                 message[PAKKA_MESSAGE_SIZE];
};

typedef enum {
    PAKKA_REPORT_INFO    = 0,
    PAKKA_REPORT_WARNING = 1,
    PAKKA_REPORT_ERROR   = 2
} pakka_report_severity_t;

typedef void (*pakka_report_fn)(void *userdata,
                                pakka_report_severity_t severity,
                                pakka_status_t status,
                                const char *entry_name,
                                const char *message);

pakka_status_t pakka_open(const char *path, pakka_open_mode_t mode,
                          pakka_archive_t **out, pakka_error_t *err);
pakka_status_t pakka_create(const char *path, pakka_format_t format,
                            unsigned flags,
                            pakka_archive_t **out, pakka_error_t *err);
/* pakka_close implicitly commits any pending pakka_add_file /
 * pakka_delete changes before releasing the archive — equivalent to
 * calling pakka_commit() explicitly first. A failed implicit commit
 * is sticky: pakka_close still releases the handle and frees the
 * archive, but returns the commit status (and for a pakka_create-
 * style archive it skips the final rename to new_pakpath so a broken
 * create isn't published). Always pakka_close every archive returned
 * by pakka_open or pakka_create — even on error paths. */
pakka_status_t pakka_close(pakka_archive_t *archive, pakka_error_t *err);

pakka_format_t pakka_format(const pakka_archive_t *archive);
size_t pakka_entry_count(const pakka_archive_t *archive);
pakka_status_t pakka_entry_at(const pakka_archive_t *archive, size_t index,
                              const pakka_entry_t **out);
pakka_status_t pakka_find_entry(const pakka_archive_t *archive,
                                const char *entry_name,
                                const pakka_entry_t **out);

/* Accessors for the opaque entry handle. Names are NUL-terminated and
 * remain valid until the owning pakka_archive_t is closed. Offsets and
 * sizes are uint64_t for forward compatibility with PK3 (which can
 * exceed the PAK 4 GiB ceiling); PAK entries always fit in 32 bits. */
const char *pakka_entry_name(const pakka_entry_t *entry);
uint64_t    pakka_entry_size(const pakka_entry_t *entry);
uint64_t    pakka_entry_offset(const pakka_entry_t *entry);

/* Streaming reader. A pakka_reader_t is a stateful cursor into one
 * entry of the owning pakka_archive_t. Multiple readers may be open
 * on the same archive simultaneously: pakka_reader_read re-seeks the
 * archive's file handle before every read so interleaved readers
 * stay coherent. A reader becomes invalid the moment its archive is
 * closed; callers must pakka_reader_close every reader before
 * pakka_close. The archive owns the underlying FILE*; pakka_reader_close
 * only frees the reader handle. */
pakka_status_t pakka_open_entry(pakka_archive_t *archive,
                                const char *entry_name,
                                pakka_reader_t **out,
                                pakka_error_t *err);
pakka_status_t pakka_reader_read(pakka_reader_t *reader, void *buf,
                                 size_t len, size_t *nread,
                                 pakka_error_t *err);
void pakka_reader_close(pakka_reader_t *reader);

pakka_status_t pakka_add_file(pakka_archive_t *archive,
                              const char *source_path,
                              const char *entry_name,
                              pakka_error_t *err);
/* Add an entry whose payload comes from an in-memory buffer rather
 * than a source file. Same validation as pakka_add_file (entry-name
 * length, unsafe-name, duplicate, u32 size cap, append overflow).
 * len == 0 produces a zero-byte entry. */
pakka_status_t pakka_add_memory(pakka_archive_t *archive,
                                const char *entry_name,
                                const void *data, size_t len,
                                pakka_error_t *err);
pakka_status_t pakka_delete(pakka_archive_t *archive,
                            const char *entry_name,
                            pakka_error_t *err);
pakka_status_t pakka_commit(pakka_archive_t *archive, pakka_error_t *err);

/* Read an entry's full payload into a freshly malloc'd buffer. *data
 * and *len are populated on success; caller MUST release the buffer
 * with pakka_free (not free()) — on Windows the library may be linked
 * to a different C runtime than the caller, so free() across the
 * boundary is undefined. Zero-byte entries return *data == NULL,
 * *len == 0, status PAKKA_OK. */
pakka_status_t pakka_read_entry_alloc(pakka_archive_t *archive,
                                      const char *entry_name,
                                      void **data, size_t *len,
                                      pakka_error_t *err);
void pakka_free(void *ptr);

/* Verify the archive's structural integrity. Walks every entry:
 *   - rejects names that pakka itself would refuse to extract (path
 *     traversal, drive prefix, Windows reserved name, control bytes)
 *   - streams each payload to confirm the bytes referenced by
 *     entry->offset/length are actually readable
 *   - flags collisions after portable-union normalization (case fold,
 *     slash/backslash, trailing dot/space) — extracting such a pak
 *     would silently overwrite files on Windows/HFS+
 *
 * report (optional, may be NULL) is called once per finding with a
 * severity, the per-finding status, the entry name (NULL if the
 * finding is archive-wide), and a human-readable message. Returns
 * PAKKA_OK if every finding was INFO/WARNING; otherwise returns the
 * first ERROR-level status. The flags parameter is reserved for future
 * use; pass 0. */
pakka_status_t pakka_verify(pakka_archive_t *archive, unsigned flags,
                            pakka_report_fn report, void *userdata,
                            pakka_error_t *err);

#ifdef __cplusplus
}
#endif

#endif
