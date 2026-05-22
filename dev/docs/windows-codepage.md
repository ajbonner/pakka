# Windows codepage / non-ASCII path handling

**Status:** known bug, fix deferred. This doc captures the audit, the
intended remediation, and the locked-in policy decisions so the work
is ready to pick up later. If you mirror this as a GitHub issue, point
the issue body at this file rather than duplicating content.

Pakka's Win32 code path uses ANSI narrow-char file APIs end-to-end and
takes argv straight from `main()`. On a non-Western Windows installation
(Russian CP1251, Japanese CP932, Chinese CP936, etc.), non-ASCII paths
either silently misbehave or fail outright. The bug is invisible on
US-English Windows (CP1252) because the ASCII subset of CP1252 round-
trips through every other codepage; everything below 0x80 is identical
across the OEM/ANSI codepage map.

## 1. Concrete failure modes

**Open an archive with a Cyrillic name on a Russian PC:**

    pakka -l "Архив.pak"

argv arrives in CP1251 bytes (`\xC0\xF0\xF5\xE8\xE2`). `fopen()` calls
the CRT, which looks up the file under the active codepage. It succeeds
only if the on-disk UTF-16 filename happens to round-trip through
CP1251. A filename that doesn't (CJK chars on a Russian PC, accented
Latin on a Japanese PC) produces ERROR_FILE_NOT_FOUND with no useful
diagnostic.

**Add a file with a non-ASCII name to a pak:**

    pakka -a foo.pak тест.txt

Pakka stores the raw CP1251 bytes (`\xF2\xE5\xF1\xF2.txt`) into the PAK
header's 56-byte name field. The archive is now non-portable: on a
Japanese PC the same bytes display as different characters (`蛻ｹ繧ｹ繝.txt`
or similar), and the bytes don't survive a copy to UTF-8-aware tools
without manual codepage tagging.

**Recurse into a directory with non-ASCII entries:**

`cli_add_folder_r` calls `opendir(path)` at `src/cli.c:770`. The vendor
shim at `src/vendor/dirent/dirent.h:682` converts the narrow path to
wide via `mbstowcs_s` and dispatches to `_wopendir` — but the input
bytes are already ANSI-codepage-encoded, so any name unmappable in the
active codepage round-trips to `?` placeholders or fails.

## 2. Audit findings

### 2.1 Entry point

- `src/cli.c:136` — `int main(int argc, char *argv[])`. ANSI argv.
  No `wmain` variant.

### 2.2 Narrow Win32 file APIs

All Win32 syscalls in pakka use the `A` (ANSI-codepage) variants:

| Call site | File:line | Note |
| --------- | --------- | ---- |
| `fopen(path, "rb"/"r+b")` | `src/pakfile.c:287, 1639, 2352, 2378` | open + rebuild paths |
| `fopen(...)` | `src/pk3file.c:921, 1506, 1562, 1996, 2292, 2331, 2365` | open + per-entry source reads + rebuild paths |
| `fopen(path, "wb")` | `src/platform.c:321` | extract write path (`open_extract_target`) |
| `opendir(path)` | `src/cli.c:770` | wraps vendored dirent's narrow form |
| `_mkdir(path)` | `src/platform.c:183, 305` | inside extraction tree builder |
| `GetFileAttributesA(path)` | `src/platform.c:237, 295, 313` | explicit `A` suffix |
| `GetTempPathA(...)` | `src/platform.c:133` | temp file location |
| `CreateFileA(path, ...)` | `src/platform.c:91` | atomic temp-file create |
| `MoveFileExA(src, dst, ...)` | `src/platform.c:192, 206` | atomic rename |
| `DeleteFileA(path)` | `src/platform.c:95` | rollback path |

Total 11 `fopen` call sites across `pakfile.c` + `pk3file.c`, plus the
extract-write `fopen` inside `platform.c`. The re-audit also flagged
`_fullpath`, `_getcwd`, `stat`, and `remove(tmp_path)` call sites
elsewhere in `src/`; those need wrappers in the implementation pass
even though they aren't strictly `*A`-suffixed Win32 APIs (the CRT
narrow forms have the same active-codepage problem).

No `_wfopen`, no `CreateFileW`, no `MoveFileExW`, no `_wmkdir`, no
`GetFileAttributesW`. The codebase never imports `<wchar.h>` for path
purposes.

### 2.3 Conversion logic

Zero `MultiByteToWideChar` / `WideCharToMultiByte` calls anywhere in
`src/`. No `setlocale(LC_ALL, ".UTF8")`, no `SetConsoleOutputCP(CP_UTF8)`,
no manifest `<activeCodePage>UTF-8</activeCodePage>`. No `UNICODE` /
`_UNICODE` defines in `CMakeLists.txt`.

The vendored dirent (`src/vendor/dirent/dirent.h:682-714`) internally
calls `mbstowcs_s` → `_wopendir`, but that only round-trips through the
active codepage — it doesn't add Unicode coverage.

### 2.4 On-disk entry name encoding

**PAK / SiN / Daikatana / WAD** (`src/pakfile.c:3163` `write_pak_entry`,
memcpy at `:3181`): the 8-, 56-, or 120-byte name field (8 for WAD,
56 for PAK/Daikatana, 120 for SiN) is `memcpy`'d directly from
`entry->filename`. Those bytes came from argv (ANSI codepage),
public-API callers passing `entry_name` directly, or — on copy/rebuild
— the source archive. The formats themselves have no encoding metadata
of any kind.

**ZIP / PK3 / PK4** (`src/pk3file.c:600-601`): a comment acknowledges
the question — `"< 0x20 or == 0x7F is control; >= 0x80 is allowed for
UTF-8 / CP437"` — but the writer does **not** set GPBF bit 11 (the
ZIP-spec UTF-8 flag), and the reader does not check it. Whatever bytes
the user typed go into the local-file-header and central-directory
name fields as-is.

### 2.5 Why the manifest opt-in doesn't apply

`<activeCodePage>UTF-8</activeCodePage>` in the application manifest
makes the `A`-suffixed Win32 APIs interpret narrow strings as UTF-8
without any code changes — but it requires Windows 10 1903 (May 2019)
or later. Pakka's supported Windows floor is **Windows XP SP3**, built
with the VS 2017 `v141_xp` toolset. (Neither the README nor
`CMakeLists.txt` currently pin the floor explicitly — the CI matrix
runs on Windows Server 2025 / VS 2026. The XP floor is a project
policy that the build system happens to satisfy because nothing in
`src/` reaches past the XP API surface.) The manifest is a no-op on
XP/7/8/10<1903, so the explicit wide-char path is required regardless.
The manifest can be added later as a redundant fast-path for modern
Windows, but the wide-char work isn't optional.

## 3. Intended fix (deferred)

UTF-8 as the canonical internal path encoding on all platforms.
Convert UTF-8 → UTF-16 only at Win32 syscall boundaries. This matches
the convention used by modern Rust, Go, and Python on Windows, and
keeps the Unix code path unchanged (UTF-8 is already the universal
narrow encoding under glibc/musl/BSD libc).

### 3.1 Entry point

Rename the current `int main(int argc, char *argv[])` body in
`src/cli.c` to a static `cli_run(int argc, char **argv)`. The POSIX
`main` becomes a thin wrapper that just calls `cli_run`. On Windows,
add an `#ifdef _WIN32` `wmain(int argc, wchar_t **wargv)` that
converts each `wargv[i]` to UTF-8 via
`WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)` (size probe
then convert), populates a heap-allocated `char **argv_utf8`, and
calls `cli_run`. Calling `main` by name from `wmain` is a bad seam —
`cli_run` makes the layering explicit and testable.

The `argv_utf8` vector must be **mutable**: `strip_long_options`
(`src/cli.c:873`, invoked from `parseopts` at `:978`) rewrites argv
in place during option parsing. Don't share storage with a `const
char *const *` view.

Set `argv_utf8[argc] = NULL` (POSIX convention; some call paths rely
on the sentinel).

Use `WC_ERR_INVALID_CHARS` so malformed surrogates fail loudly instead
of silently turning into replacement characters. On any conversion
failure, write a clear stderr message naming the bad argument index
and exit with a non-zero status; do not fall through with a partial
argv.

### 3.2 Platform helpers

Add the following to `src/platform.{h,c}`. Unix backend is the existing
POSIX call; Windows backend converts UTF-8 → UTF-16 and dispatches to
the `W` variant.

    FILE *pakka_platform_fopen(const char *utf8_path, const char *mode)
    int   pakka_platform_remove(const char *utf8_path)
    int   pakka_platform_stat(const char *utf8_path, pakka_stat_t *out)
    int   pakka_platform_get_file_attributes(const char *utf8_path)
    int   pakka_platform_fullpath(const char *in_utf8, char *out_utf8, size_t cap)
    int   pakka_platform_getcwd(char *out_utf8, size_t cap)
    pakka_dir_t *pakka_platform_opendir(const char *utf8_path)
    int   pakka_platform_readdir(pakka_dir_t *, char *utf8_name_out, size_t cap)
    void  pakka_platform_closedir(pakka_dir_t *)

`pakka_platform_mkdir(const char *path, int mode)` and
`pakka_platform_rename_replace` / `pakka_platform_rename_noreplace`
already exist (`src/platform.h:91`, `:148`, `:158`). Keep their
signatures verbatim — just swap their Win32 backends from `_mkdir` /
`MoveFileExA` to UTF-8 → wide → `_wmkdir` / `MoveFileExW`. Don't
collapse the rename pair into a single `movefile(flags)`: the
no-replace semantic and Win32 error capture differ and the callers
depend on the split.

**Iteration is not just `_wopendir`.** The vendored dirent's
`_wopendir` is the easy half — but the narrow `readdir()` at
`src/vendor/dirent/dirent.h:751` converts each result back to narrow
via `wcstombs_s`, which goes through the active codepage and undoes
the UTF-16 win on the return path. So `cli_add_folder_r` at
`src/cli.c:770` still emits ANSI-codepage bytes today. The Windows
backend must call `_wreaddir` directly and convert with
`WideCharToMultiByte(CP_UTF8, ...)`, never the narrow `readdir`.
That's why §3.2 exposes the full `opendir`/`readdir`/`closedir` triple
as platform helpers rather than just `opendir`.

Internal helpers (Win32-only, static):

    static int u8_to_w(const char *utf8, wchar_t *wbuf, size_t wcap)
    static int w_to_u8(const wchar_t *wstr, char *u8buf, size_t cap)

`u8_to_w` uses `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`;
`w_to_u8` uses `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)`.

**Buffer policy:** stack-allocated `wchar_t[MAX_PATH]` (260) on Win32.
Long-path support (`\\?\`-prefixed > MAX_PATH paths) is out of scope
per §5; treating it as a separate effort keeps this work bounded and
avoids touching fixed-size path buffers elsewhere in pakka (`Pak_t`
path fields, `cli.c` temp-path buffers, vendored dirent's `PATH_MAX`).

### 3.3 Call-site replacements

Every Win32 narrow-API call site listed in §2.2 changes to its
`pakka_platform_*` helper. The bulk lives in `src/platform.c` itself
(already platform-conditional), with the rest in `src/pakfile.c` and
`src/pk3file.c` (`fopen` call sites) and `src/cli.c:770` (`opendir`).

The `pakka_platform_*` Unix backends are one-line wrappers, so the
abstraction adds essentially zero cost on POSIX hosts.

### 3.4 Wire encoding

- Write UTF-8 in all archive entry-name fields. On the input side, the
  argv conversion in §3.1 ensures `entry->filename` is already UTF-8 by
  the time it reaches the writers.
- ZIP/PK3/PK4: set GPBF bit 11 on entries whose name contains any byte
  > 0x7F. Set it in **both** the local file header (LFH GP flags at
  byte offset 6, ~`pk3file.c:1352`) and the central directory record
  (CDR GP flags at byte offset 8, ~`pk3file.c:1372`); both records
  carry independent GP flag fields per the ZIP spec and most readers
  consult both. Lower-ASCII-only names leave bit 11 clear (the spec
  permits either, and clearing it on pure-ASCII names keeps archives
  byte-identical to existing pakka output for the common case).
- PAK / SiN / Daikatana / WAD: store UTF-8 bytes in the name field as-is
  (these formats have no encoding metadata; the convention is just
  documented here and in the per-format docs under `dev/docs/`).

### 3.5 Public API

`include/pakka.h`'s `pakka_open`, `pakka_open_ex`, `pakka_create` keep
their `const char *path` signature. Document in the header: on Windows,
`path` is interpreted as UTF-8. Callers holding wide strings convert
before calling. No `pakka_open_w(const wchar_t *, ...)` variant planned
for v1 — the API surface stays single-signature.

## 4. Locked policy decisions

These are settled so future-pakka doesn't relitigate them.

**Read-side legacy archives: best-effort fallback.** On read, try UTF-8
first (validate the name bytes against UTF-8 syntax). If the name isn't
valid UTF-8:

- **ZIP / PK3 / PK4**: interpret as CP437 (the ZIP-spec default for
  pre-bit-11 archives), convert CP437 → UTF-8 for the entry name pakka
  exposes to callers.
- **PAK / SiN / Daikatana / WAD**: pass the raw bytes through unchanged
  in the in-memory `entry->filename`. Those formats have no encoding
  metadata at all and were authored on PCs of every locale across 30
  years; pakka has no basis to guess, so it preserves them verbatim
  and lets the caller decide.

**False-positive caveat (read side).** The UTF-8 validation-then-CP437
heuristic for ZIP is not a perfect inverse. Some CP1251 (and other
8-bit Cyrillic) byte sequences are accidentally legal UTF-8 — e.g.
the two bytes `0xD0 0xBF` form the legal UTF-8 sequence U+043F (`п`)
but in CP1251 spell `Рї`. Without GPBF bit 11 to disambiguate, pakka
will silently misclassify them as UTF-8 and pass through bytes that
the user actually meant as CP1251. This is inherent to any byte-only
heuristic — pakka explicitly does not try to second-guess it.

**Extract behavior for legacy invalid-UTF-8 entry names (PAK family).**
The raw-bytes-in-memory policy above conflicts with the "UTF-8 on
Windows" invariant at the syscall boundary: invalid UTF-8 cannot be
converted to UTF-16 for `CreateFileW`. Policy:

- **list (`-l`)**: pass the bytes through the existing
  `pakka_fprint_sanitized` (`src/cli.c:32`), which currently
  `?`-substitutes control bytes. Extend it to also `?`-substitute
  byte runs that don't form valid UTF-8, so a UTF-8 console renders
  printable output without lying about what's stored.
- **extract (`-x`)**: log a `[warn] entry <N>: name not valid UTF-8,
  substituting invalid bytes` warning naming the entry by index plus
  the sanitized display name. Replace each invalid byte run with `_`
  and create the file under the substituted name. The data is not
  silently lost. A future `--strict` flag can switch this to "skip
  entry" without changing the default.
- **memory representation (`entry->filename`)**: stays the raw bytes
  from the archive. Conversion / substitution happens at the OS
  syscall boundary, not at archive-parse time. Callers that want the
  display name use the same sanitization helper as `-l`.

**Write-side: UTF-8 always.** The active console codepage of the user's
shell is irrelevant; conversion happens at the boundary. A user typing
non-ASCII at a CP1251 cmd.exe prompt gets UTF-16 from
`CommandLineToArgvW`-equivalent OS plumbing (already wide before
`wmain` sees it), and pakka converts to UTF-8 in the wmain wrapper.

**Public API: narrow `const char *`, UTF-8 on Windows.** No wide-char
overload. Callers with wide strings convert.

## 5. Out of scope (for the eventual implementation)

- Normalization (NFC vs NFD). Windows filesystems tolerate either;
  pakka should pass bytes through without normalizing.
- Console output codepage. Pakka writes diagnostics to stderr/stdout
  through `fprintf`. On legacy Windows consoles set to CP1251/CP932,
  UTF-8 output displays as mojibake. Fix is `SetConsoleOutputCP(CP_UTF8)`
  in the wmain wrapper — easy add but a behavior change worth its own
  decision.
- Long-path support (`\\?\`-prefixed > MAX_PATH paths). Independent
  concern; out of scope for the Unicode fix.
- Environment-variable encoding (e.g. `PAKKA_INJECT_FAULT_AT`). These
  flow through `getenv()` and would need the same UTF-8 ↔ codepage
  treatment if we ever start passing path-like env vars. Out of scope
  for v1; revisit if any env var grows a path argument.

## 6. Tests to add with the implementation pass

- Round-trip a file named with Cyrillic characters through `-a`, `-l`,
  `-x` on Windows. Verify the entry name in the archive header is the
  expected UTF-8 byte sequence (`\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82.txt`
  for `Тест.txt`).
- Round-trip a file named with CJK characters (`日本語.txt`) on a
  Windows VM with a non-CJK active codepage (e.g. en-US).
- Read a legacy ZIP/PK3 written by another tool with bit 11 clear and
  non-UTF-8 name bytes; verify the CP437 fallback produces sensible
  output.
- A bats file `tests/unicode_paths.bats` skipped on non-Windows hosts
  (the bug is Windows-only; Unix is already correct because POSIX is
  UTF-8 throughout).
- Extract a legacy PAK whose stored name bytes are not valid UTF-8
  (e.g. CP1251). Verify pakka emits the documented `[warn] entry <N>:
  name not valid UTF-8` line on stderr, that the file gets created
  with `_`-substituted bytes, and that exit status is still success
  (`--strict` is future).

**MSYS2 vs cmd.exe invocation.** The CI test harness (`bats tests/`
from MSYS2 bash, see `CLAUDE.md`) invokes the MSVC-built `pakka.exe`
under MSYS2. MSYS2 path-translates its own argv (e.g.
`/c/Users/...` → `C:\Users\...`) before the binary sees it, and that
translation happens at the C runtime / MSYS2 boundary before `wmain`
ever runs. The Unicode fix has to work under both invocation paths:
native `cmd.exe` (where argv arrives untranslated from
`CommandLineToArgvW`) and MSYS2 (where it may have already been
rewritten). The bats tests above already exercise the MSYS2 path;
add at least one manual cmd.exe smoke case to the verification
checklist.
