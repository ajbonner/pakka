# Using libpakka from C

`include/pakka.h` exposes an opaque-handle C99 API for opening,
inspecting, extracting from, and mutating pak archives. Every
exported symbol is prefixed `pakka_`; `make symbol-audit` fails the
build if anything else leaks out, so the library is namespace-clean
against any host symbol table.

`man 3 pakka` (`man/pakka.3` in this tree, or
`${MANDIR}/man3/pakka.3` after `make install`) is the full reference.
This page is the inventory and call-pattern overview.

## Error model

Library functions never call `exit` and never write to `stdout` or
`stderr`. Every failure returns a `pakka_status_t` and optionally
populates a caller-provided `pakka_error_t` with structured detail:
errno or Win32 `GetLastError`, operation name, entry index, file
offset, message. Callers that don't need structured errors can pass
`NULL` for the `pakka_error_t *` argument.

## Inventory

### Version

* `pakka_version` — returns the linked libpakka version string (e.g.
  `"1.9.0"`). Useful for bindings that want to feature-gate on the
  loaded library.

### Archive lifecycle

* `pakka_open` — open an existing archive; format is probed from the
  on-disk header. `mode` is `PAKKA_OPEN_READ`, `PAKKA_OPEN_READ_WRITE`,
  or `PAKKA_OPEN_READ_WRITE_ATOMIC` (see
  [Transactional commit](#transactional-commit)).
* `pakka_open_ex` — open with the format pinned explicitly. Matters
  for Daikatana (shares Quake's `"PACK"` magic) and for callers that
  want to refuse formats they don't expect.
* `pakka_create` — create a new archive.
* `pakka_close` — close the handle; implicitly commits if dirty. A
  failed implicit commit is reported as a close-time error.

### Read introspection

* `pakka_format` — the format pakka resolved the archive to.
* `pakka_entry_count` — number of entries.
* `pakka_entry_at` — fetch entry by index.
* `pakka_find_entry` — fetch entry by name.

### Entry accessors

These read fields from the opaque `pakka_entry_t`:

* `pakka_entry_name`
* `pakka_entry_size` — uncompressed size.
* `pakka_entry_compressed_size` — on-disk size (equals `size` for
  uncompressed entries).
* `pakka_entry_offset` — byte offset into the archive.

### Streaming reads

* `pakka_open_entry` — opens a streaming reader over an entry's
  payload. Decompresses transparently for PK3 / PK4 DEFLATE and
  Daikatana custom-codec entries.
* `pakka_reader_read` — pull N bytes from the reader.
* `pakka_reader_close` — release the reader.
* `pakka_open_entry_handle` — variant that returns a low-level
  handle, for callers that need to plumb the read into their own
  I/O abstraction.

### Mutation

* `pakka_add_file` — add a file from disk; entry name can differ
  from source path.
* `pakka_add_memory` — add bytes from a caller-owned buffer.
* `pakka_delete` — remove an entry by name.
* `pakka_rename` — rename (move) an entry. Index-only: the payload
  never moves, so on a plain `PAKKA_OPEN_READ_WRITE` PAK-class handle
  the commit rewrites just the directory in place. Refuses an existing
  destination with `PAKKA_ERR_DUPLICATE` (IWAD / PWAD allow duplicate
  names); `old == new` is a no-op.
* `pakka_copy` — duplicate an entry under a new name. PAK-class formats
  share the source's bytes (one payload, two directory entries pointing
  at the same offset — the file grows by one directory record, and the
  sharing survives later rebuilds). PK3 / PK4 duplicate the payload at
  commit (ZIP needs a per-entry local header). Copying a still-staged
  source duplicates the staged bytes (no sharing until it is committed).
* `pakka_commit` — explicit flush; `pakka_close` calls this for you
  on a dirty archive.
* `pakka_commit_is_atomic` — query whether a commit on this handle is
  all-or-nothing (see [Transactional commit](#transactional-commit)).

### Integrity

* `pakka_verify` — walks every entry, runs the name-safety check and
  the normalized-collision preflight, confirms directory offsets
  point at readable bytes. Drives a caller-supplied
  `pakka_report_fn` callback for per-finding reporting.
  `PAKKA_VERIFY_DEEP` adds per-entry CRC32 / decompression checks
  (CRC for PK3 / PK4; byte-count check for Daikatana, whose custom
  codec has no CRC).

### Tuning

* `pakka_set_max_decompressed_size` — caps the bytes any single
  `pakka_open_entry` or `pakka_read_entry_alloc` will inflate.
  Default 64 MiB; pass 0 to disable. Refuses zip-bomb-style
  high-ratio entries before they hit RAM.
* `pakka_set_compression` — PK3 / PK4 only. Switches new adds
  between uncompressed and DEFLATE. Sticky on the open handle;
  rejected on non-ZIP archives. See
  [`formats.md`](formats.md) for the per-entry STORED fallback.

### Memory convenience

* `pakka_read_entry_alloc` — slurp an entire entry into a malloc'd
  buffer. Pair with `pakka_free`.
* `pakka_free` — release a buffer returned by the library. Must be
  used in place of `free()` because, on Windows, the library may be
  linked against a different CRT than the caller.

## Transactional commit

Whether a `pakka_commit` is **atomic** — fully applied or the on-disk
file left untouched, never a torn intermediate — depends on the format
and how the archive was opened:

| Operation | PAK / SiN / Daikatana / IWAD / PWAD | PK3 / PK4 |
| --- | --- | --- |
| `pakka_create` (new file) | atomic (temp → rename on close) | atomic |
| `pakka_delete` then commit | atomic (rebuild → rename) | atomic |
| `pakka_add_*` then commit, `PAKKA_OPEN_READ_WRITE` | **in place — a mid-write failure can corrupt the original** | atomic |
| `pakka_add_*` then commit, `PAKKA_OPEN_READ_WRITE_ATOMIC` | atomic (staged → rebuild → rename) | atomic |
| `pakka_rename` / `pakka_copy` then commit, `PAKKA_OPEN_READ_WRITE` | **in place directory rewrite — index-only, payload untouched, not crash-atomic** | atomic (rebuild → rename) |
| `pakka_rename` / `pakka_copy` then commit, `PAKKA_OPEN_READ_WRITE_ATOMIC` | atomic (rebuild → rename) | atomic |

The PAK family defaults to streaming adds straight into the live file:
fast and bounded-memory, but an interrupted add or commit can leave the
archive torn. Open with `PAKKA_OPEN_READ_WRITE_ATOMIC` to stage adds and
publish every commit through a temp-file rebuild and rename instead, at
the cost of rewriting the archive on each commit. For ZIP the flag is a
no-op — PK3 / PK4 commits already rebuild and rename.

A consumer that wants uniform behavior shouldn't hardcode this table;
call `pakka_commit_is_atomic(handle)` instead. It returns nonzero for any
PK3 / PK4 handle, any `pakka_create` handle, any handle opened
`PAKKA_OPEN_READ_WRITE_ATOMIC`, and any read-only handle; zero only for a
PAK-class handle opened plain `PAKKA_OPEN_READ_WRITE`. The recommended
pattern is: open atomic when you need durability, or query and fall back
to your own backup-and-replace when it returns zero.

Note the atomic STORED contract matches PK3 STORED: `pakka_add_file`
records the source's size + CRC32 and re-reads the file at commit, so the
source must stay readable and unchanged until `pakka_commit` /
`pakka_close` returns (a changed source makes commit refuse with
`PAKKA_ERR_FORMAT`). Use `pakka_add_memory` to pin bytes at call time.

## Call-pattern example

[`test/c_api_test.c`](https://github.com/ajbonner/pakka/blob/master/test/c_api_test.c)
exercises the entire public surface against the installed
`libpakka.a` only — no internal headers. It's the canonical example
of call patterns and structured-error handling.

## Threading and ownership

A `pakka_archive_t *` is owned by exactly one thread at a time.
Concurrent calls on the same handle are undefined; concurrent calls
on different handles to the same on-disk archive are unsupported
(file locking is not pakka's responsibility). Entry pointers
returned by `pakka_entry_at` / `pakka_find_entry` are invalidated by
any mutation (`pakka_add_file`, `pakka_add_memory`, `pakka_delete`,
`pakka_rename`, `pakka_copy`, `pakka_commit`). Open `pakka_reader_t`
handles are likewise invalidated by a mutation or commit — a rebuild
commit reopens the underlying file — so close and reopen readers across
one.
