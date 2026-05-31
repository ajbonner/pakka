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

/* Version string of the linked libpakka, e.g. "1.5.0". Static storage,
 * never NULL. Useful for runtime feature gating in language bindings or
 * for embedding the linked library version in a host application's
 * diagnostics. */
const char *pakka_version(void);

typedef enum {
    PAKKA_FORMAT_AUTO = 0,
    PAKKA_FORMAT_PAK,        /* id Quake 1 / 2 and Half-Life / GoldSrc — bit-identical PACK layout */
    PAKKA_FORMAT_PK3,
    PAKKA_FORMAT_PK4,
    PAKKA_FORMAT_SIN,        /* Ritual 1998, SPAK + 120-byte names */
    PAKKA_FORMAT_DAIKATANA,  /* Ion Storm 2000, PACK + 72-byte directory entries */
    PAKKA_FORMAT_IWAD,       /* id Doom 1/2 base archive — IWAD magic, 16-byte directory entries */
    PAKKA_FORMAT_PWAD        /* id Doom 1/2 patch archive — PWAD magic, otherwise identical to IWAD */
} pakka_format_t;

typedef enum {
    PAKKA_OPEN_READ       = 0,
    PAKKA_OPEN_READ_WRITE = 1,
    /* Like PAKKA_OPEN_READ_WRITE, but for PAK-class archives (PAK / SiN /
     * Daikatana / IWAD / PWAD) every mutating commit is published
     * atomically: adds are staged in memory rather than streamed into the
     * live file, and commit writes a complete fresh archive to a temp file
     * then renames it over the original, so an interrupted add or commit
     * cannot corrupt the published archive. (Plain PAKKA_OPEN_READ_WRITE
     * adds stream in place — fast, bounded memory, but a mid-write failure
     * can tear the original; pakka_delete already rebuilds either way.)
     * For PK3/PK4 this is identical to PAKKA_OPEN_READ_WRITE — ZIP commits
     * are always atomic. pakka_create is already atomic-on-publish, so it
     * takes no mode. Use pakka_commit_is_atomic to query the effective
     * guarantee without hardcoding format knowledge. */
    PAKKA_OPEN_READ_WRITE_ATOMIC = 2
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
/* Same as pakka_open but lets the caller pin the format. format_hint ==
 * PAKKA_FORMAT_AUTO means "probe and decide from the on-disk magic + a
 * layout probe for the PACK/Daikatana ambiguity" (today's pakka_open
 * behavior). Other values skip the probe and assert that the on-disk
 * magic is compatible with the hint:
 *   - PAKKA_FORMAT_SIN: requires "SPAK" magic
 *   - PAKKA_FORMAT_PAK / PAKKA_FORMAT_DAIKATANA: requires "PACK" magic
 *   - PAKKA_FORMAT_PK3 / PAKKA_FORMAT_PK4: requires "PK\3\4" / "PK\5\6"
 *   - PAKKA_FORMAT_IWAD: requires "IWAD" magic
 *   - PAKKA_FORMAT_PWAD: requires "PWAD" magic
 * Mismatch returns PAKKA_ERR_INVALID_ARGUMENT. */
pakka_status_t pakka_open_ex(const char *path, pakka_open_mode_t mode,
                             pakka_format_t format_hint,
                             pakka_archive_t **out, pakka_error_t *err);
pakka_status_t pakka_create(const char *path, pakka_format_t format,
                            unsigned flags,
                            pakka_archive_t **out, pakka_error_t *err);
/* pakka_close implicitly commits any pending pakka_add_file /
 * pakka_add_memory / pakka_delete / pakka_rename / pakka_copy changes
 * before releasing the archive — equivalent to
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

/* Forward iteration in archive directory order. O(1) per step, the
 * idiomatic way to walk every entry:
 *
 *     const pakka_entry_t *e;
 *     for (e = pakka_entry_first(pak); e != NULL; e = pakka_entry_next(e)) {
 *         ...consume e...
 *     }
 *
 * Returned pointers remain valid until the owning archive is closed or
 * the entry list is mutated by pakka_add_file / pakka_add_memory /
 * pakka_delete / pakka_rename / pakka_copy / pakka_commit (including the
 * implicit commit performed by pakka_close). Any iterator mid-walk MUST
 * be discarded across a mutation. pakka_entry_first returns NULL on an empty archive or a
 * NULL argument; pakka_entry_next returns NULL at end-of-list or on a
 * NULL argument. The archive must be live — pakka_close frees the
 * handle, so calling either function after close (or with a stale
 * pointer) is undefined behavior, same as every other accessor here.
 *
 * Concurrency rules match pakka_entry_at / pakka_find_entry: multiple
 * threads may iterate the same archive concurrently as long as no other
 * thread is mutating it. */
const pakka_entry_t *pakka_entry_first(const pakka_archive_t *archive);
const pakka_entry_t *pakka_entry_next(const pakka_entry_t *entry);

/* Accessors for the opaque entry handle. Names are NUL-terminated and
 * remain valid until the owning pakka_archive_t is closed. Offsets and
 * sizes are uint64_t so the API can grow into ZIP64-sized archives in
 * the future without an ABI change; today every supported format
 * stays inside the 32-bit ceiling and pakka explicitly refuses ZIP64
 * sentinels. */
const char *pakka_entry_name(const pakka_entry_t *entry);
uint64_t    pakka_entry_size(const pakka_entry_t *entry);
uint64_t    pakka_entry_offset(const pakka_entry_t *entry);

/* On-disk (compressed) payload size for the entry, in bytes. For
 * STORED entries (PAK, SiN, IWAD/PWAD, STORED PK3/PK4, STORED
 * Daikatana) this equals pakka_entry_size. For DEFLATE PK3/PK4
 * entries it's the compressed-stream length between the local file
 * header and the next entry / CDR. For compressed Daikatana entries
 * it's the on-disk codec-stream length. Useful for "Packed" columns
 * in file-manager-style UIs. */
uint64_t    pakka_entry_compressed_size(const pakka_entry_t *entry);

/* Streaming reader. A pakka_reader_t is a stateful cursor into one
 * entry of the owning pakka_archive_t. Multiple readers may be open
 * on the same archive simultaneously: pakka_reader_read re-seeks the
 * archive's file handle before every read so interleaved readers
 * stay coherent. A reader becomes invalid the moment its archive is
 * closed; callers must pakka_reader_close every reader before
 * pakka_close. The archive owns the underlying FILE*; pakka_reader_close
 * only frees the reader handle.
 *
 * A reader also becomes invalid across a mutating call or a commit on the
 * same archive (pakka_add_file / pakka_add_memory / pakka_delete /
 * pakka_rename / pakka_copy / pakka_commit, including the implicit commit
 * in pakka_close): a rebuild commit closes and reopens the underlying
 * file handle and relocates payloads, so a reader opened beforehand would
 * read stale offsets. Close and reopen the reader after any mutation. */
pakka_status_t pakka_open_entry(pakka_archive_t *archive,
                                const char *entry_name,
                                pakka_reader_t **out,
                                pakka_error_t *err);
pakka_status_t pakka_reader_read(pakka_reader_t *reader, void *buf,
                                 size_t len, size_t *nread,
                                 pakka_error_t *err);
void pakka_reader_close(pakka_reader_t *reader);

/* Same as pakka_open_entry but skips the name lookup. Use when the
 * caller already holds the pakka_entry_t* (e.g. from pakka_entry_at
 * in an iteration loop) to avoid the O(n) name scan per open. The
 * entry must come from the same archive; passing a stale pointer
 * after pakka_delete/pakka_commit is undefined behavior. */
pakka_status_t pakka_open_entry_handle(pakka_archive_t *archive,
                                       const pakka_entry_t *entry,
                                       pakka_reader_t **out,
                                       pakka_error_t *err);

/* Add a file from disk under entry_name. Validation: entry-name length
 * + unsafe-name + duplicate + source symlink/regular-file checks +
 * u32 size cap.
 *
 * Duplicate-name policy: every format except IWAD/PWAD rejects a
 * second add of the same entry_name with PAKKA_ERR_DUPLICATE. WAD
 * archives accept the duplicate (Doom IWADs/PWADs deliberately use
 * repeated lump names — e.g. THINGS / LINEDEFS / SECTORS appear once
 * per map — and engines walk the directory positionally, so pakka
 * preserves the duplicates rather than rejecting authorings that mirror
 * the on-disk pattern).
 *
 * Format-specific timing of the source read:
 *   - PAK / SiN (plain PAKKA_OPEN_READ_WRITE): source bytes are read and
 *     written into the archive immediately. After return, source_path can
 *     be freely modified or deleted.
 *   - PAK / SiN / Daikatana / IWAD / PWAD opened
 *     PAKKA_OPEN_READ_WRITE_ATOMIC: a STORED add defers to commit exactly
 *     like the PK3/PK4 STORED path below — pakka_add_file records the
 *     CRC32 + size and stashes the source path, and the bytes are read
 *     again (and revalidated) at commit, so source_path MUST stay
 *     readable and unchanged until pakka_commit / pakka_close returns.
 *     Daikatana entries that get compressed are captured at add time
 *     (see the Daikatana bullet); pakka_add_memory pins bytes at call
 *     time on every format.
 *   - PK3 / PK4 with STORED (default): pakka_add_file records the
 *     CRC32 + size, stashes the source path, and reads the bytes
 *     again at commit time (or close time, which implicitly commits).
 *     The caller MUST keep source_path readable and unchanged until
 *     pakka_commit / pakka_close returns; a modified-since-add source
 *     causes commit to refuse with PAKKA_ERR_FORMAT (CRC or size
 *     mismatch), and a source replaced with a symlink causes
 *     PAKKA_ERR_UNSAFE_NAME. This is what makes ZIP adds atomic
 *     against partial-write failures — the archive on disk isn't
 *     touched until the commit-time temp-file rename succeeds.
 *   - PK3 / PK4 with DEFLATE (pakka_set_compression): the source is
 *     read fully into memory at add time, compressed (or kept as
 *     STORED if DEFLATE didn't win), and the resulting bytes are
 *     stashed in an in-memory buffer. The CRC32 is recomputed over
 *     the captured bytes — the LFH/CDR always describes the bytes
 *     actually committed. After return, source_path can be freely
 *     modified or deleted; the commit-time revalidation does NOT
 *     apply on this path (the source is captured at add time).
 *   - Daikatana: source bytes are read fully into memory, encoded
 *     through pakka_dk_deflate when the entry name ends in
 *     .tga / .bmp / .wal / .pcx / .bsp (the DK format documentation's
 *     compress-on-pack policy; everything else is STORED). After
 *     return, source_path can be freely modified or deleted. Per-
 *     entry auto-fallback to STORED when the encoded form is no
 *     smaller than the source.
 *
 * Use pakka_add_memory if you want the payload bytes pinned at call
 * time on every format. */
pakka_status_t pakka_add_file(pakka_archive_t *archive,
                              const char *source_path,
                              const char *entry_name,
                              pakka_error_t *err);
/* Add an entry whose payload comes from an in-memory buffer rather
 * than a source file. Same validation as pakka_add_file (entry-name
 * length, unsafe-name, duplicate, u32 size cap, append overflow),
 * including the IWAD/PWAD exception that allows duplicate entry names
 * — see pakka_add_file for the policy rationale.
 * len == 0 produces a zero-byte entry. The buffer is copied
 * synchronously (PAK / SiN write it to the archive, PK3 / PK4 store
 * an owned copy until commit), so the caller can free `data` as soon
 * as this function returns. */
pakka_status_t pakka_add_memory(pakka_archive_t *archive,
                                const char *entry_name,
                                const void *data, size_t len,
                                pakka_error_t *err);
pakka_status_t pakka_delete(pakka_archive_t *archive,
                            const char *entry_name,
                            pakka_error_t *err);

/* Rename (move) an entry within the archive. Only the directory name
 * field changes — the payload bytes never move, so a rename is an
 * index-only update. For a PAK-class archive opened PAKKA_OPEN_READ_WRITE
 * the commit rewrites just the trailing directory in place (the payload
 * is left untouched), so renaming one entry in a multi-gigabyte archive
 * costs O(directory), not O(payload).
 *
 * new_name is validated exactly like an added entry name: length cap for
 * the format, unsafe-name rejection, and a duplicate check. If new_name
 * already names an entry the call fails with PAKKA_ERR_DUPLICATE and the
 * archive is unchanged — except IWAD/PWAD, which allow duplicate lump
 * names (the rename proceeds). old_name == new_name is a no-op returning
 * PAKKA_OK. old_name must name an existing entry (PAKKA_ERR_NOT_FOUND
 * otherwise); when it appears more than once (only possible on IWAD/PWAD)
 * the first match in directory order is renamed.
 *
 * Atomicity follows the open mode, exactly like pakka_add_file: a
 * PAK-class archive opened PAKKA_OPEN_READ_WRITE rewrites the directory
 * in place (fast, not crash-atomic — only the directory region is
 * touched, never the payload), while PAKKA_OPEN_READ_WRITE_ATOMIC and
 * every PK3/PK4 archive publish via a full temp-file rebuild + rename.
 * Use pakka_commit_is_atomic to query the effective guarantee. The change
 * is staged in memory and applied by pakka_commit (or the implicit commit
 * in pakka_close). Requires a writable archive. */
pakka_status_t pakka_rename(pakka_archive_t *archive,
                            const char *old_name, const char *new_name,
                            pakka_error_t *err);

/* Duplicate an entry under a second name. Semantics differ by format:
 *   - PAK-class (PAK / SiN / Daikatana / IWAD / PWAD): the new entry
 *     shares the source's payload bytes — one payload, two directory
 *     entries pointing at the same offset. No payload is copied, so the
 *     archive grows by just one directory record, and the sharing is
 *     preserved across later rebuilds (a subsequent pakka_delete, or an
 *     atomic-mode commit) rather than silently duplicating the bytes.
 *   - PK3 / PK4 (ZIP): the payload is duplicated at commit. ZIP requires
 *     each entry to carry its own local file header with a matching name,
 *     so byte aliasing is not possible; the duplicate is an independent,
 *     fully valid copy.
 *
 * dst_name is validated like an added entry name (length cap, unsafe
 * name, duplicate). A pre-existing dst_name fails with PAKKA_ERR_DUPLICATE
 * (except IWAD/PWAD, which allow duplicate lump names). src_name must
 * exist (PAKKA_ERR_NOT_FOUND otherwise); the first match in directory
 * order is copied when the name is duplicated (IWAD/PWAD only).
 *
 * Copying a source that was added in this session but not yet committed
 * duplicates the staged payload — after commit the two PAK-class entries
 * have distinct offsets (no sharing), because there are no on-disk bytes
 * to alias yet. Copy a committed source to obtain shared bytes.
 *
 * Atomicity follows the open mode (see pakka_rename /
 * pakka_commit_is_atomic). Staged in memory and applied by pakka_commit /
 * pakka_close. Requires a writable archive. */
pakka_status_t pakka_copy(pakka_archive_t *archive,
                          const char *src_name, const char *dst_name,
                          pakka_error_t *err);

pakka_status_t pakka_commit(pakka_archive_t *archive, pakka_error_t *err);

/* Reports whether a pakka_commit on this archive is atomic — i.e.
 * guaranteed to either fully succeed or leave the on-disk file
 * unchanged, never a torn intermediate. Lets a consumer decide whether
 * it needs its own backup-and-replace dance without hardcoding which
 * formats stream in place. Returns nonzero (atomic) for:
 *   - any PK3/PK4 archive (ZIP commits always rebuild + rename),
 *   - any archive from pakka_create (published via temp + rename on close),
 *   - any archive opened PAKKA_OPEN_READ_WRITE_ATOMIC,
 *   - any read-only archive (commit is a no-op).
 * Returns zero only for a PAK-class archive opened plain
 * PAKKA_OPEN_READ_WRITE, whose adds stream into the live file. Returns
 * zero on a NULL argument. */
int pakka_commit_is_atomic(const pakka_archive_t *archive);

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

/* Bit flags for pakka_verify's `flags` argument. PAKKA_VERIFY_DEEP
 * adds per-entry decompression + CRC32 checks on ZIP-class archives
 * (PK3/PK4); no effect on PAK, which has no per-entry checksum. Other
 * bits are reserved and must be zero. */
#define PAKKA_VERIFY_DEEP 0x1u

/* Verify the archive's structural integrity. Walks every entry:
 *   - rejects names that pakka itself would refuse to extract (path
 *     traversal, drive prefix, Windows reserved name, control bytes)
 *   - streams each payload to confirm the bytes referenced by
 *     entry->offset/length are actually readable
 *   - flags collisions after portable-union normalization (case fold,
 *     slash/backslash, trailing dot/space) — extracting such a pak
 *     would silently overwrite files on Windows/HFS+
 *   - duplicate entry names are reported as PAKKA_REPORT_WARNING on
 *     IWAD/PWAD (Doom IWADs deliberately reuse lump names per map);
 *     on every other format they remain PAKKA_REPORT_ERROR
 *   - with PAKKA_VERIFY_DEEP: also decodes every DEFLATE entry and
 *     confirms uncompressed size + CRC32 match the central directory
 *
 * report (optional, may be NULL) is called once per finding with a
 * severity, the per-finding status, the entry name (NULL if the
 * finding is archive-wide), and a human-readable message. Returns
 * PAKKA_OK if every finding was INFO/WARNING; otherwise returns the
 * first ERROR-level status. */
pakka_status_t pakka_verify(pakka_archive_t *archive, unsigned flags,
                            pakka_report_fn report, void *userdata,
                            pakka_error_t *err);

/* Cap the maximum payload size that pakka_open_entry and
 * pakka_read_entry_alloc will accept. Set to 0 to disable the cap.
 * Default is 64 MiB.
 *
 * Coverage by format:
 *   - PK3 / PK4 (every entry, STORED or DEFLATE): caps both the
 *     declared uncompressed size and the on-disk compressed size,
 *     so peak resident bytes during inflate are bounded by 2x the
 *     cap per open reader.
 *   - Daikatana compressed entries: same as ZIP — caps both sides.
 *   - Quake PAK / SiN / Daikatana STORED: no effect (no decompression
 *     step; the on-disk size IS the payload size).
 *
 * Also gates the open-time ZIP CDR allocation against a separate,
 * non-tunable PK3_MAX_CDR_SIZE so a hostile EOCD-declared cdr_size
 * can't force a multi-GiB malloc before per-entry validation runs. */
pakka_status_t pakka_set_max_decompressed_size(pakka_archive_t *archive,
                                               uint64_t max_bytes,
                                               pakka_error_t *err);

/* Compression methods accepted by pakka_set_compression. Values match
 * the on-disk ZIP method numbers (RFC PKWARE APPNOTE 4.4.5): 0 STORED,
 * 8 DEFLATE. Other method values are reserved for future use and
 * rejected by the setter. */
typedef enum {
    PAKKA_COMPRESSION_STORE   = 0,
    PAKKA_COMPRESSION_DEFLATE = 8
} pakka_compression_t;

/* Set the compression method applied to subsequent pakka_add_file /
 * pakka_add_memory calls on the archive. Sticky on the archive handle
 * until changed; default at pakka_open / pakka_create time is
 * PAKKA_COMPRESSION_STORE.
 *
 * Validation order — error paths leave archive state unchanged:
 *   1. archive == NULL                     -> PAKKA_ERR_INVALID_ARGUMENT
 *   2. method not in {STORE, DEFLATE}      -> PAKKA_ERR_INVALID_ARGUMENT
 *   3. !archive->writable                  -> PAKKA_ERR_INVALID_ARGUMENT
 *   4. DEFLATE on non-PK3/PK4 format       -> PAKKA_ERR_INVALID_ARGUMENT
 *   5. STORE on any writable archive       -> PAKKA_OK
 *   6. DEFLATE on writable PK3/PK4         -> PAKKA_OK
 *
 * The setter is idempotent and may be called any number of times; the
 * most recent successful call wins for the next pakka_add_*. The
 * setting lives on the in-memory handle, NOT on the on-disk archive
 * — re-opening an existing archive resets to STORE.
 *
 * When DEFLATE is set, each add still falls back to STORED for that
 * entry if the compressed payload is not smaller than the source
 * (matches info-zip behaviour). Entries whose source exceeds the
 * 2 GiB INT_MAX limit imposed by the bundled (sdefl/sinfl) backend
 * also fall back to STORED without erroring; the zlib backend (opt-
 * in at build time) accepts the full 4 GiB ZIP u32 range. */
pakka_status_t pakka_set_compression(pakka_archive_t *archive,
                                     pakka_compression_t method,
                                     pakka_error_t *err);

#ifdef __cplusplus
}
#endif

#endif
