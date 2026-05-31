# Windows path encoding (UTF-8)

A reference for how pakka handles non-ASCII paths and entry names on
Windows.

Pakka treats UTF-8 as the canonical internal narrow encoding on every
platform. POSIX filesystem APIs treat path strings as opaque bytes —
they do not interpret them through any codepage — so the helpers in
`src/platform.{h,c}` are one-line passthroughs that hand UTF-8 bytes
straight to the kernel, where modern tools and locales render them
correctly as UTF-8. On Windows the
C runtime's narrow-char file APIs (`fopen`, `_mkdir`, `opendir`, the
`A`-suffixed Win32 entry points) interpret bytes through the active
codepage — which silently misbehaves on any non-Western install — so
pakka converts UTF-8 to UTF-16 once at the syscall boundary and
dispatches to the `W`-suffixed CRT and Win32 APIs. The same UTF-8
convention applies on the wire for PAK / SiN / Daikatana / WAD /
PK3 / PK4: archives produced through the `pakka` CLI store UTF-8
because `wmain` converts argv at the boundary; archives produced
through the C API store whatever bytes the caller passed (see §4 and
§7).

## 1. Sources

### 1.1 Primary references

- Microsoft Win32 / MSVC CRT documentation for the `W`-suffixed APIs
  pakka calls on Windows: `_wfopen`, `_wremove`, `_wstat64i32`,
  `_wfullpath`, `_wgetcwd`, `_wmkdir`, `_wopendir` / `_wreaddir`,
  `MoveFileExW`, `CreateFileW`, `GetFileAttributesW`,
  `GetTempPathW`, plus
  [`MultiByteToWideChar`](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar) /
  [`WideCharToMultiByte`](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte)
  for the conversion at the boundary. The exact contracts for
  `MB_ERR_INVALID_CHARS` / `WC_ERR_INVALID_CHARS` are taken from
  those two pages.
- PKWARE APPNOTE.TXT §4.4.4 (current:
  <https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT>) —
  defines general-purpose flag bit 11 (the "language encoding" / EFS
  flag) as the signal that ZIP entry names are UTF-8. The legacy
  default before bit 11 is CP437.
- IBM CP437 codepage table (the "OEM US" / DOS code page) — used as
  the ZIP-spec fallback decoding for entry names whose GP bit 11 is
  clear. Authoritative tables:
  [IBM CP437](https://www.ibm.com/docs/en/db2/12.1.x?topic=tables-code-page-437-generic-system-437)
  and the Unicode vendor mapping at
  <https://www.unicode.org/Public/MAPPINGS/VENDORS/MISC/IBMGRAPH.TXT>.
- RFC 3629 — UTF-8 syntax,
  <https://www.rfc-editor.org/rfc/rfc3629>. pakka's validator rejects
  overlong encodings, surrogate halves (U+D800..U+DFFF), and
  codepoints above U+10FFFF, all per §4 of that RFC.
- pakka's own implementation: `src/cli.c` (`wmain` / `cli_run` +
  the sanitize / substitute call sites), `src/platform.{h,c}`
  (UTF-8 ↔ UTF-16 conversion and W-suffixed dispatch),
  `src/pk3file.c` (GP bit 11 writer, CP437 fallback decoder,
  name-decode policy), `src/common.{h,c}` (`pakka_is_valid_utf8`,
  `pakka_utf8_substitute_invalid`).

### 1.2 Additional references

- Microsoft Learn, "Use UTF-8 code pages in Windows apps" —
  <https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page>.
  Documents the Windows 10 version 1903+ `activeCodePage` manifest
  element discussed in §8.
- Microsoft Learn, "Application Manifests" —
  <https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests>.
  Full manifest schema reference.
- Microsoft Learn, "Code Page Identifiers" —
  <https://learn.microsoft.com/en-us/windows/win32/intl/code-page-identifiers>.
  Numeric CP IDs (CP437 = 437, CP1251 = 1251, CP1252 = 1252,
  Shift-JIS = 932, GBK = 936, UTF-8 = 65001) — useful when reading
  ZIP archives written by non-pakka tools that record the codepage
  in a vendor extra field.
- Microsoft Learn, "Naming Files, Paths, and Namespaces" —
  <https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file>.
  Reserved device names, path syntax, the `\\?\` long-path prefix,
  and `MAX_PATH` semantics — context for pakka's name-safety check.
- Unicode Standard Annex #15, "Unicode Normalization Forms" —
  <https://unicode.org/reports/tr15/>. Defines NFC / NFD / NFKC /
  NFKD; relevant for the normalization caveat in §10.
- Unicode Technical Report #36, "Unicode Security Considerations" —
  <https://unicode.org/reports/tr36/>. Bidi controls, confusables,
  invisible codepoints — the broader security context behind pakka's
  control-byte rejection and listing sanitizer.

### 1.3 Preservation

Every live external reference in §1.1 and §1.2 was submitted to the
[Wayback Machine](https://web.archive.org/) on 2026-05-24. To fetch
a snapshot of any link above, prepend `https://web.archive.org/web/`
to its URL. Sources that carry their own archival guarantee (RFCs,
Microsoft Learn, ibm.com, unicode.org, GitHub source files) are
excluded from the submission set.

## 2. Background

ANSI-codepage narrow Win32 APIs — `fopen`, `CreateFileA`,
`GetFileAttributesA`, `_mkdir`, `MoveFileExA`, `opendir` (via the
vendored dirent's narrow path) — interpret narrow byte strings
through the process's active ANSI codepage. On a Russian install
that codepage is CP1251; on Japanese it's CP932; on Simplified
Chinese it's CP936. A filename whose on-disk UTF-16 representation
doesn't round-trip cleanly through the active codepage produces
`ERROR_FILE_NOT_FOUND` (or some other quiet failure) with no useful
diagnostic.

The bug is invisible on US-English Windows (CP1252) for one reason:
every codepage in the OEM/ANSI family shares the ASCII subset, so
anything below 0x80 round-trips identically. A test suite that only
exercises ASCII names — including pakka's pre-fix CI — would never
notice. Non-ASCII paths from argv, from `opendir` results, or from
archive entry names hit the failure modes the moment the active
codepage diverges from the on-disk UTF-16.

The fix is structural: the application takes argv as UTF-16, treats
UTF-8 as the canonical internal narrow encoding, and converts to
UTF-16 once at every Win32 syscall. Same convention used by modern
Rust, Go, and Python on Windows.

## 3. Architecture

Three layers, each with a single responsibility — the entry point
that converts argv at the command-line boundary, the in-process
UTF-8 narrow-char convention, and the platform layer that re-encodes
at the syscall boundary.

The two conversion points and the UTF-8 zone between them are what
the diagram below traces:

    ┌──────────────────────────────────────────────────────────────────────┐
    │  External boundary  (Win32 wide-char / NTFS UTF-16)                  │
    └────────────────┬─────────────────────────────────▲───────────────────┘
                     │                                 │
                     │  argv from OS                   │  _wfopen, _wmkdir,
                     │  (wchar_t **)                   │  MoveFileExW,
                     │                                 │  CreateFileW,
                     │                                 │  _wopendir / _wreaddir,
                     │                                 │  GetFileAttributesW,
                     │                                 │  GetTempPathW
                     │                                 │
        WideCharToMultiByte                  MultiByteToWideChar
        (CP_UTF8, WC_ERR_INVALID_CHARS)      (CP_UTF8, MB_ERR_INVALID_CHARS)
        src/cli.c::wmain                     src/platform.c::u8_to_w
                     │                                 ▲
                     ▼                                 │
    ┌──────────────────────────────────────────────────────────────────────┐
    │  Internal  (UTF-8 everywhere)                                        │
    │                                                                      │
    │  argv_utf8 → cli_run → pakka_open / create / add / extract / verify  │
    │  → pakka_platform_*  (src/platform.c)                                │
    │                                                                      │
    │  Every `const char *` in include/pakka.h is UTF-8 on Windows by      │
    │  contract; entry->filename, internal path buffers, and the C API     │
    │  arguments callers pass all live in this zone.                       │
    └──────────────────────────────────────────────────────────────────────┘

A malformed byte sequence at either conversion gate raises `EILSEQ`
(`*_ERR_INVALID_CHARS`) rather than silently substituting U+FFFD;
this is what makes the conversion a true boundary rather than a
lossy approximation. POSIX builds collapse both gates into identity:
the OS already treats narrow paths as opaque UTF-8 bytes, so
`pakka_platform_*` is one-line wrappers around the POSIX call.

### 3.1 Entry point — `wmain`

The C runtime hands `wmain` an argv array of `wchar_t *` pointers
already decoded by the OS from the original command line.
`src/cli.c`'s `wmain` walks each `wargv[i]` through
`WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)` — once with
a NULL output buffer to size the UTF-8 result, once into a fresh
malloc — and hands the resulting UTF-8 vector to `cli_run`, which is
the platform-neutral CLI body. `WC_ERR_INVALID_CHARS` makes malformed
surrogates fail loudly instead of silently substituting U+FFFD; the
program exits with a non-zero status and a stderr message naming the
bad argv index. POSIX builds use a thin `main` that forwards `argv`
to `cli_run` directly.

### 3.2 `owned_argv` vs `argv_utf8`

`wmain` keeps two parallel arrays:

- **`argv_utf8`** is what `cli_run` (and therefore `parseopts` /
  `strip_long_options`) sees. It is mutable: `strip_long_options` in
  `src/cli.c` rewrites argv in place by copying kept argument
  pointers down to lower indices when stripping long options. The
  higher slots are left untouched, so after the shift the same
  malloc'd pointer can appear at two indices.
- **`owned_argv`** is a parallel array of the original malloc'd
  pointers in their pre-shift positions. It is never mutated.
  `wmain_free_argv` walks it at exit and releases each block once.

Freeing through `argv_utf8` instead would double-free any pointer
that `strip_long_options` shifted down (the original copy is left
intact past the new `dst`) and leak the pointers that were
overwritten (no longer in `argv_utf8` but still on the heap). POSIX
builds never encountered the problem because they never malloc'd
their argv.

### 3.3 Platform layer

`src/platform.h` declares a `pakka_platform_*` family — `fopen`,
`remove`, `stat`, `lstat`, `realpath`, `getcwd`, `opendir`,
`readdir`, `closedir`, `mkdir`, `rename_replace`,
`rename_noreplace`, `mkstemp_open`, `open_extract_target`,
`is_reparse_or_symlink` — that the rest of the codebase calls in
place of the raw POSIX or CRT names. Every `const char *` path
argument is UTF-8 on Windows by contract; the comment block above
the declarations states this.

The Windows backends in `src/platform.c` use two file-scope statics,
`u8_to_w` and `w_to_u8`, that wrap `MultiByteToWideChar(CP_UTF8,
MB_ERR_INVALID_CHARS, ...)` and `WideCharToMultiByte(CP_UTF8,
WC_ERR_INVALID_CHARS, ...)`. The `*_ERR_INVALID_CHARS` flags surface
malformed input as `EILSEQ`; oversized expansion surfaces as
`ENAMETOOLONG`. After conversion the helpers dispatch to `_wfopen`,
`_wremove`, `_wstat64i32`, `_wfullpath`, `_wgetcwd`, `_wopendir` +
`_wreaddir`, `_wmkdir`, `MoveFileExW`, `CreateFileW`,
`GetFileAttributesW`, `GetTempPathW`. POSIX backends are one-line
wrappers around the matching POSIX call.

The `_wreaddir` direct call matters. The vendored dirent shim
(`src/vendor/dirent/dirent.h`) implements narrow `opendir` /
`readdir` by going wide internally with `mbstowcs_s` /
`_wopendir`, but it then converts each result back to narrow via
`wcstombs_s` on the readdir return path — which routes through the
active codepage and undoes the Unicode win. `pakka_platform_readdir`
bypasses the narrow shim entirely: it calls `_wreaddir` and converts
the wide name to UTF-8 with `WideCharToMultiByte(CP_UTF8, ...)`.

## 4. On-disk entry-name encoding (write)

UTF-8 is the convention pakka follows, not an invariant the writer
enforces. The C API does not validate caller-supplied `entry_name`
bytes; PAK-class writers `memcpy` the caller's bytes directly into
the fixed-size name field. The `pakka` CLI satisfies the convention
because `wmain` converts argv at the boundary, so every byte that
reaches the writer from a CLI invocation is already UTF-8.

### 4.1 PAK / SiN / Daikatana / WAD

`write_pak_entry` in `src/pakfile.c` handles all four formats off
the same geometry table; it copies the entry-name bytes verbatim
into the fixed-size name field — 8 bytes for WAD, 56 bytes for
PAK / Daikatana, 120 bytes for SiN. See the per-format docs in this
directory for the full directory-entry layouts. None of these
formats carry encoding metadata. The convention pakka writes — and
reads on a round-trip — is UTF-8.

### 4.2 ZIP / PK3 / PK4

Same UTF-8 convention, plus pakka sets general-purpose flag bit 11
(`0x0800`, the APPNOTE 6.3 §4.4.4 language-encoding flag) in
**both** the local file header (LFH offset 6) and the central
directory record (CDR offset 8) when:

- the name contains at least one byte > 0x7F, **and**
- the bytes form a valid UTF-8 sequence per RFC 3629.

Pure-ASCII names leave bit 11 clear, which keeps byte-identical
output for archives that don't actually need the flag. Non-ASCII
names whose bytes are not valid UTF-8 (a library caller passing
CP1251 directly through `pakka_add_file`) also leave bit 11 clear —
asserting UTF-8 over non-UTF-8 bytes would produce an archive that
pakka's own read path rejects. Both the LFH and CDR get the same GP
flag word because the ZIP spec carries the flag field in both
records and most readers consult both. See `pk3_gp_flags_for_name`
in `src/pk3file.c`.

## 5. On-disk entry-name decoding (read)

pakka tries UTF-8 first regardless of any encoding flag, on the
principle that "actually valid UTF-8" is the strongest signal
available. The decode boundary collapses three on-disk encodings
into the single UTF-8 in-memory representation:

    On-disk archive bytes                  Decode boundary             entry->filename
    ─────────────────────                  ───────────────             ───────────────

    PK3/PK4, GP bit 11 set        ──► validate UTF-8 ──►              ┐
       (claims UTF-8)                  reject if invalid                │
                                       (PAKKA_ERR_FORMAT)               │
                                                                        │
    PK3/PK4, GP bit 11 clear      ──► decode CP437 ────►                │   UTF-8
       (defaults to CP437)            high-half table  ──►              ├──► (always)
                                      via src/pk3file.c                 │
                                                                        │
    PAK / SiN / Daikatana / WAD   ──► passthrough ─────►                │
       (opaque bytes; no              (raw bytes; the                   │
        encoding metadata)             archive does not                 │
                                       declare a charset)              ─┘

The CP437 decoder is `pk3_decode_entry_name` in `src/pk3file.c`; it
returns `PK3_DECODE_TOOLONG` when the high-half expansion exceeds
the in-memory name buffer (CP437 bytes that map to U+00BF..U+00FF
each become 2 UTF-8 bytes, so a maximally-long high-half name can
double in size). PAK-class formats have no decoder — the raw bytes
*are* the filename, valid UTF-8 or not, and §6 covers what happens
on extract / list when those bytes aren't UTF-8.

### 5.1 ZIP / PK3 / PK4

`pk3_decode_entry_name` in `src/pk3file.c`:

- Bytes are valid UTF-8 → accept as-is.
- Bytes are not valid UTF-8 **and** GP bit 11 is set → reject as
  `PAKKA_ERR_FORMAT`. The archive asserts UTF-8 and isn't; silently
  downgrading to CP437 would mask a malformed or crafted archive.
- Bytes are not valid UTF-8 **and** GP bit 11 is clear → decode
  through the CP437 high-half table (`pk3_cp437_high` in
  `src/pk3file.c`). The low half is identical to ASCII and passes
  through. Output is UTF-8. If the CP437 → UTF-8 expansion exceeds
  the entry-name buffer the decoder returns `PK3_DECODE_TOOLONG`,
  which the caller maps to `PAKKA_ERR_LIMIT` — the archive is
  rejected rather than the name silently truncated.

### 5.2 PAK / SiN / Daikatana / WAD

These formats carry no encoding metadata, so pakka cannot
distinguish a UTF-8 name from a CP1251 or Shift-JIS name written by
a tool on a non-Western PC twenty years ago. The in-memory
`entry->filename` is the raw bytes from the archive, byte-for-byte.
Conversion or substitution (if any) happens when the name is
**rendered or materialised** — at extract time when handing the
name to the filesystem syscall, or at list time when writing it to
stdout — not at archive-parse time. See §6 for the policy; the
list operation (`pakka_fprint_sanitized` in `src/cli.c`) and the
extract operation each apply their own UTF-8 sanitization.

### 5.3 False-positive caveat

The UTF-8-first-then-CP437 heuristic for ZIP is not a perfect
inverse. Some CP1251 (and other 8-bit Cyrillic) byte sequences are
accidentally legal UTF-8 — for example the two bytes `0xD0 0xBF`
form the legal UTF-8 sequence U+043F `п` but in CP1251 spell `Рї`.
Without GP bit 11 to disambiguate, pakka silently picks UTF-8 and
the user gets a different character than they meant. This is
inherent to any byte-only heuristic; pakka explicitly does not try
to second-guess it.

## 6. Extract-time handling of non-UTF-8 legacy names

Legacy PAK / SiN / Daikatana / WAD archives can carry entry-name
bytes that aren't UTF-8 (a Russian-localised PC writing CP1251, a
Japanese PC writing Shift-JIS). On Windows those bytes cannot be
handed to the W-suffixed Win32 / CRT APIs the platform layer
dispatches to — `MultiByteToWideChar(CP_UTF8,
MB_ERR_INVALID_CHARS, ...)` returns `EILSEQ`. pakka materialises
the file under a sanitized name rather than silently dropping the
entry. WAD names are traditionally 8-byte uppercase ASCII so this
path is rarely exercised for WAD, but the sanitization wraps every
PAK-class read path uniformly.

### 6.1 Substitution

`pakka_utf8_substitute_invalid` in `src/common.c` walks the source
byte by byte. Valid UTF-8 sequences (1–4 bytes per RFC 3629) are
copied verbatim; **each** invalid byte — one at a time, not one
marker per contiguous run — is replaced with the fill character.
Returns 1 if anything was substituted, 0 if the source was already
valid UTF-8. A 4-byte CP1251 prefix becomes four fill characters.

### 6.2 Extract

The extract loop in `src/cli.c` runs the substitution with `_` as
the fill char. If anything was substituted, pakka emits a `[warn]`
line to stderr naming the entry index and the sanitized form, then
creates the file under the sanitized name:

    [warn] entry 0: name not valid UTF-8, substituting invalid bytes (writing as '____.txt')

Exit status is still success — preserving the data under a
sanitized name is the documented default.

### 6.3 Collision check

The pre-pass that rejects entries that would collide after extract
normalization runs the **same** substitution before the sort. Two
legacy CP1251 names whose first four bytes differ but are both
4 bytes of invalid UTF-8 (e.g. CP1251 `Тест.txt` and `АБВГ.txt`)
both substitute to `____.txt`. Without the pre-pass substitution
they would compare distinct, both pass preflight, and one would
silently overwrite the other on extract. The collision check in
`src/cli.c` (in the normalized-name preflight loop preceding the
extract pass) catches this and refuses to extract with a "collide
after normalization" error.

### 6.4 List

`pakka_fprint_sanitized` in `src/cli.c` runs the same UTF-8
substitution (using `?` as the fill char) plus the original
control-byte substitution. Listing a legacy CP1251 PAK on a UTF-8
console therefore produces printable ASCII without raw 8-bit bytes
corrupting the TTY state, and without claiming the name is
something it isn't.

### 6.5 Invalid-UTF-8 decision matrix

The full per-operation behaviour for an entry whose on-disk name
bytes are not valid UTF-8 per RFC 3629, broken down by archive
class and code path:

| Class                | Read / list                                                                  | Extract                                                                                                   | C API (`pakka_find_entry`, `pakka_open_entry`, ...) |
| -------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| PAK / SiN / DK / WAD | Listing prints sanitized form (invalid bytes → `?`); raw bytes never reach TTY. | `_` substitution per byte; `[warn]` to stderr; file materialized under sanitized name; exit 0.            | Returns raw on-disk bytes (no sanitization). Caller may pass them straight to a write path that calls `pakka_platform_*`; on Windows that returns `EILSEQ` from the UTF-8-to-UTF-16 conversion at the boundary. |
| PK3 / PK4 (bit 11 set) | `pakka_open` rejects the archive with `PAKKA_ERR_FORMAT` ("entry N: name claims UTF-8 but is not"). | Same — open never succeeds.                                                                               | Same — open never succeeds.                         |
| PK3 / PK4 (bit 11 clear) | Bytes decoded through the CP437 high-half table (§5.1); result is UTF-8; listing prints as-is. | The CP437→UTF-8 result is written to disk as-is.                                                          | Returns the CP437→UTF-8 result.                     |

The collision check (§6.3) runs on the **sanitized** name for PAK-class
formats, so two distinct on-disk names that map to the same sanitized
form refuse to extract rather than silently overwrite.

## 7. Public API

Every `const char *` path or entry-name argument across
`include/pakka.h` — `pakka_open`, `pakka_open_ex`, `pakka_create`,
`pakka_add_file`, `pakka_add_memory`, `pakka_find_entry`,
`pakka_open_entry`, `pakka_read_entry_alloc`, `pakka_delete` — is
interpreted as UTF-8 on Windows. Library callers holding wide
strings convert before calling. There is no
`pakka_open_w(const wchar_t *, ...)` overload planned — the API
stays single-signature.

The UTF-8-on-Windows contract is documented in the comment block at
the top of the `pakka_platform_*` declarations in `src/platform.h`.

The C API does **not** validate caller-supplied entry-name bytes
against UTF-8 syntax. That responsibility is on the caller, the
same way it's on the caller to pass a path the filesystem will
accept. The ZIP writer's GP bit 11 logic uses UTF-8 validity as
the precondition for setting the flag, so passing non-UTF-8 bytes
to a PK3 writer produces an archive with bit 11 clear and the
caller's bytes verbatim in the name field. APPNOTE-compliant
readers (including pakka itself) decode such names through the
CP437 fallback — which is almost certainly **not** what a caller
who passed CP1251 / Shift-JIS / GB18030 bytes meant the names to
read as. The C API contract is: callers who want the names to read
correctly on every reader must convert to UTF-8 themselves.

## 8. Windows floor and the manifest non-opt-in

The supported Windows floor is **Windows XP SP3** (the README
documents the VS 2017 `v141_xp` toolset for XP/Vista/7 release
builds; CI runs on Windows Server 2025 with VS 2026). Pakka avoids
API calls that require later Windows versions so the same source
tree builds against either toolset.

Modern Windows offers an `<activeCodePage>UTF-8</activeCodePage>`
element in the application manifest that makes the `A`-suffixed
Win32 APIs interpret narrow strings as UTF-8 with no source change.
It requires Windows 10 version 1903 (May 2019) or later. On every
supported floor below that — XP, Vista, 7, 8, early 10 — the
manifest is a no-op, so the explicit `wmain` + UTF-16 syscall path
described in §3 is what handles the work. The manifest could be
added later as a redundant fast path on modern Windows without
changing the structural design.

## 9. MSYS2 vs cmd.exe invocation

A Windows binary built with `wmain` receives UTF-16 argv directly
from the C runtime's command-line parsing. Under `cmd.exe` and
PowerShell that argv reflects what the user typed (already wide
before pakka sees it). Under MSYS2 bash the argv may have already
been path-translated by the MSYS2 runtime (`/c/Users/...` →
`C:\Users\...`) before the native `pakka.exe` is invoked. From
pakka's perspective both look identical at the `wmain` boundary —
each argument is whatever wide string the parent handed to
`CreateProcess`, and pakka converts it to UTF-8 once.

The Windows CI builds `pakka.exe` via CMake/MSVC and exercises it
via CTest from native PowerShell/cmd.exe; the same binary works
under interactive `cmd.exe` invocation with no path-handling
differences pakka cares about.

## 10. Out of scope

These are intentionally not implemented today and are independent
of the Unicode work above.

- **Unicode normalization (NFC vs NFD).** Windows filesystems
  tolerate either; pakka passes bytes through without normalizing.
  An NFD name (decomposed) and the NFC equivalent (composed) appear
  as distinct entries.
- **Console output codepage.** pakka does not call
  `SetConsoleOutputCP(CP_UTF8)`. On legacy CP1251 / CP932 consoles
  pakka's UTF-8 stderr/stdout will display as mojibake. The fix is
  trivial (one line in `wmain`) but is a behavior change with its
  own trade-off; a separate concern.
- **Long-path support (`\\?\`-prefixed paths > MAX_PATH).** Path
  buffers throughout pakka are `MAX_PATH`-sized (260 wide
  characters on Windows). Lifting that ceiling touches `Pak_t`
  path fields, CLI temp-path buffers, the vendored dirent's
  `PATH_MAX`, and a number of stack arrays — independent of the
  Unicode work.
- **Environment-variable path encoding.** `getenv()` is not routed
  through the UTF-8 conversion layer. The only runtime env var
  pakka reads is `PAKKA_INJECT_FAULT_AT` (operation tag + counter,
  no path bytes); it does not need UTF-8 treatment.
  `PAKKA_LEGACY_EXTRACT` is a compile-time `#ifdef` gate in
  `src/platform.c`, not a runtime env var.

## 11. Test coverage

`test/unicode_paths_test.c` covers the behavior in this document:

- PAK round-trip with a Cyrillic UTF-8 filename through `-c`,
  `-l`, and `-x`.
- PAK header byte-level assertion that the 56-byte name field
  stores the raw UTF-8 bytes for a Cyrillic name
  (`\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82.txt` for `Тест.txt`).
- PK3 with a Cyrillic filename: byte-level assertion that GP bit
  11 is set in **both** the LFH (offset +6) and the CDR (offset
  +8).
- PK3 with a pure-ASCII filename: byte-level assertion that the
  GP-flag high byte at LFH offset +6 is `0x00` (bit 11 clear).
- Legacy PAK with CP1251 bytes in the name field (built by an
  inline byte writer that skips pakka): extract substitutes the
  invalid bytes with `_`, emits the `[warn]` line to stderr, and
  exits success with the file present.
- Legacy PAK with two CP1251 names that both sanitize to the same
  path: extract refuses with a collide-after-normalization error.
- Listing a legacy CP1251 PAK: output is sanitized to printable
  ASCII (no raw 8-bit bytes reach the TTY).
- PK3 fixture with GP bit 11 set but non-UTF-8 bytes in the name:
  open rejects the archive as a format error.
- PK3 fixture with GP bit 11 clear and CP437 bytes in the name:
  the name decodes through the CP437 high-half table (`0xE1` →
  U+00DF `ß` → UTF-8 `\xC3\x9F` in the listing output).

The suite runs on every host. POSIX exercises the same byte-level
handling opaquely (POSIX treats the test names as bytes; the CI
hosts' locales render them as UTF-8); the Windows CI job is what
exercises the real `wmain` + `_wfopen` syscall path.

## 12. Related docs

- [`quake-pak-format.md`](quake-pak-format.md) — Quake / Q2 /
  GoldSrc PAK; the 56-byte name field this doc refers to.
- [`sin-pak-format.md`](sin-pak-format.md) — SiN's 120-byte name
  field.
- [`daikatana-pak-format.md`](daikatana-pak-format.md) —
  Daikatana's PAK row (custom codec, same name-encoding policy).
- [`wad-format.md`](wad-format.md) — Doom WAD; 8-byte lump names,
  conventionally uppercase ASCII. WAD reads/writes go through the
  same `pakka_platform_*` syscall layer and the same sanitizer
  paths as the other PAK-class formats, but the policy in §4–§6
  rarely triggers because the names are ASCII in practice.
- [`pk3-pk4-format.md`](pk3-pk4-format.md) — ZIP / PK3 / PK4; GP
  flag word layout and the LFH / CDR offsets referenced from §4.
