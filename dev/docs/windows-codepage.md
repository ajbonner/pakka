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
| `fopen(path, "rb"/"r+b")` | `src/pakfile.c` (open, rebuild paths) | 10 call sites across pakfile + pk3file |
| `fopen` | `src/pk3file.c` | same |
| `opendir(path)` | `src/cli.c:770` | wraps vendored dirent's narrow form |
| `_mkdir(path)` | `src/platform.c:183, 305` | inside extraction tree builder |
| `GetFileAttributesA(path)` | `src/platform.c:237, 295, 313` | explicit `A` suffix |
| `GetTempPathA(...)` | `src/platform.c:133` | temp file location |
| `CreateFileA(path, ...)` | `src/platform.c:91` | atomic temp-file create |
| `MoveFileExA(src, dst, ...)` | `src/platform.c:192, 206` | atomic rename |
| `DeleteFileA(path)` | `src/platform.c:95` | rollback path |

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

**PAK / SiN / Daikatana / WAD** (`src/pakfile.c:3163` `write_pak_entry`):
the 56- or 120-byte name field is `memcpy`'d directly from
`entry->filename`. Those bytes came from argv (ANSI codepage). The
formats themselves have no encoding metadata of any kind.

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
or later. Pakka's documented Windows floor is **Windows XP SP3** (built
with VS 2017 `v141_xp` toolset; see Installation in the README). The
manifest is a no-op on XP/7/8/10<1903, so the explicit wide-char path
is required regardless. The manifest can be added later as a redundant
fast-path for modern Windows, but the wide-char work isn't optional.

## 3. Intended fix (deferred)

UTF-8 as the canonical internal path encoding on all platforms.
Convert UTF-8 → UTF-16 only at Win32 syscall boundaries. This matches
the convention used by modern Rust, Go, and Python on Windows, and
keeps the Unix code path unchanged (UTF-8 is already the universal
narrow encoding under glibc/musl/BSD libc).

### 3.1 Entry point

Add a `#ifdef _WIN32` `wmain(int argc, wchar_t **argv)` wrapper in
`src/cli.c`. Convert each `argv[i]` to UTF-8 via
`WideCharToMultiByte(CP_UTF8, 0, ...)`, build a `char **argv_utf8`,
and call straight into the existing `main()` body (rename the current
body to a static `cli_run()` if the wmain wrapper makes that cleaner).

### 3.2 Platform helpers

Add the following to `src/platform.{h,c}`. Unix backend is the existing
POSIX call; Windows backend converts UTF-8 → UTF-16 and dispatches to
the `W` variant.

    pakka_platform_fopen(const char *utf8_path, const char *mode)
    pakka_platform_remove(const char *utf8_path)
    pakka_platform_rename(const char *old_utf8, const char *new_utf8)
    pakka_platform_mkdir(const char *utf8_path)
    pakka_platform_getfileattributes(const char *utf8_path)
    pakka_platform_opendir(const char *utf8_path)
    pakka_platform_movefile(const char *old_utf8, const char *new_utf8, unsigned flags)

The Windows backend of `pakka_platform_opendir` should call
`_wopendir` from the vendored dirent directly (the vendor exposes both
forms — `src/vendor/dirent/dirent.h:601` already uses `FindFirstFileExW`
under the hood).

A shared UTF-8 → wide helper (`pakka_platform_utf8_to_wide(const char *,
wchar_t *, size_t)`) handles the conversion. Stack-allocated `wchar_t`
buffer sized for `MAX_PATH * 2` covers the common case; fall back to
heap allocation for `\\?\`-extended paths if needed.

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
  > 0x7F. Lower-ASCII-only names leave bit 11 clear (the spec permits
  either, and clearing it on pure-ASCII names keeps archives byte-
  identical to existing pakka output for the common case).
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
- **PAK / SiN / Daikatana / WAD**: pass the raw bytes through unchanged.
  Those formats have no encoding metadata at all and were authored on
  PCs of every locale across 30 years; pakka has no basis to guess, so
  it preserves them verbatim and lets the caller decide.

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
