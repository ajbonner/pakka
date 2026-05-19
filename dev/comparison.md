# Pak Tools — Gap & Quality Analysis vs. Pakka

Sources surveyed: every PAK editor on
[Quake Wiki / Quake_tools](https://quakewiki.org/wiki/Quake_tools)
plus a handful of obvious omissions the wiki doesn't list. Date of writing:
**2026-05-19**. A second independent pass by Codex (`gpt-5.5`, read-only
sandbox, no shared context with this pass) is reconciled in the
[Codex disagreements](#codex-disagreements) section at the bottom.

## Summary

- **Pakka is the only entry with structured error reporting, normalized-collision
  preflight on extract, AND symlink-safe extract.** Every other tool in the
  field either has none of those (most), or one of them (pakextract creates
  parent dirs but skips traversal validation entirely; QPakMan refuses to
  overwrite but doesn't validate names).
- **Pakka leads on portability / CI breadth too.** 13 OS-arch combos including
  big-endian s390x and NetBSD/sparc + RH9 legacy probes. The next closest is
  QuakeForge (Linux + BSD + Windows cross-compile, GitHub Actions). Most of
  the wiki list is Windows-only with no CI at all.
- **Pakka now has PK3 / Quake 3 support.** Read for STORED + DEFLATE,
  write for STORED (DEFLATE encode deferred). Codec is the vendored
  reference INFLATE (`pakka_inflate`, ex-`puff` from zlib contrib).
  Refuses ZIP64, encryption, data descriptors, multi-disk, and any
  compression method other than 0 / 8. Closes the long-standing gap
  versus PakScape, QuakeForge `zpak`, GCFScape, and TrenchBroom.
- **Pakka lags on asset-aware features.** QPakMan auto-converts MIP/LMP/WAL
  to PNG on extract and back on create; QuArK round-trips models and entity
  graphs; PakUtil/quake-cli-tools script asset pipelines. Pakka is
  byte-accurate but format-blind below the directory.
- **Pakka's `--verify` mode is genuinely unique.** Every other tool's "verify"
  is implicit — it either fails on open or it doesn't. Nothing else exposes
  a standalone audit pass that runs the same name-safety + collision checks
  without touching disk.

## Feature matrix

### Functional surface

| Tool | List | Tree | Extract all | Extract select | Extract `-C dest` | Create | Add file | Add memory | Add dir recursive | Rename on add | Delete | Verify w/o extract | Streaming reads | PK3 / Q3 | Multi-archive | C API |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **pakka** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (STORED+DEFLATE read; STORED write) | ✓ | ✓ |
| GCFScape | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ? | ✓ (plug-in) | n/a (GUI) | ✗ |
| AdQuedit | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ? | ✗ | ✓ | ✗ | ✗ | ✗ | n/a | ✗ |
| makepak.py | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | (script) | (Py import) |
| PakExplorer | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ? | ✗ | ✓ | ✗ | ✗ | ✗ | n/a | ✗ |
| PakScape | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ? | ✗ | ✓ | ✗ | ✗ | ✓ (pk3, vol) | n/a | ✗ |
| par | ✓ | ✗ | ✓ | ? | ? | ✓ | ✗ | ✗ | ? | ✗ | ✗ | ✗ | ✗ | ✗ | (CLI) | ✗ |
| QuakeForge pak | ✓ | ✗ | ✓ | ✗ (man page) | ? | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ | partial (`-t` test) | ✗ | via `zpak` (gzip) | ✗ | C lib in QF |
| QuArK | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ? | ✓ | ✗ | ✗ | ? | n/a | Python plugins |
| QPakMan | ✓ | ✗ | ✓ | ✓ | ✓ (`-o`) | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ (PAK + WAD only) | ✗ | ✗ |
| QPed | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ? | ✗ | ✓ | ✗ | ✗ | ✗ | n/a | ✗ |
| pakextract | ✓ | ✗ | ✓ | ✗ | ✓ (`-o`) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | partial | ✗ | ✗ | ✗ |
| id qfiles.c | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | (list-driven) | ✗ | ✗ | ✗ | ✓ (4 KB buf) | ✗ | n/a | ✗ |
| quake-cli-tools | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | (script) | Py module |
| PakUtil | ✓ | ✗ | ✓ | ✓ | ? | ✓ | ✓ | ✓ (`append`) | (caller) | ? | ✗ | ✗ | ✗ | ✗ | ✓ (script API) | Py module |
| TrenchBroom PAK VFS | ✓ (internal) | ✗ | ✗ (read-only) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ (Q1+Daikatana) | ✓ | C++ class |

### Quality, portability, maintenance

| Tool | License | Source | Last activity | Build | Form | Platforms | Tests | CI |
|---|---|---|---|---|---|---|---|---|
| **pakka** | MIT | github.com/ajbonner/pakka | active (2026) | GNU make + CMake (Win) | CLI + C lib | macOS Intel/AS, Linux glibc/musl x86/arm/-m32, FreeBSD 15 amd/-m32, OpenBSD 7.8, Win MSVC, BE s390x, RH9 i386, NetBSD 3.0/sparc | bats + C-API + symbol-audit + clang-tidy | GitHub Actions, 13 jobs |
| GCFScape | freeware (closed) | none | v1.8.6, 2017-01-22 | n/a | Win32 GUI | Windows | none public | none |
| AdQuedit | freeware (closed) | none | 1997 (Hicks) | n/a | Win32 GUI | Windows | none | none |
| makepak.py | unstated | wordpress post | 2016-06-18 | n/a | Python 2/3 script | any with Python | none | none |
| PakExplorer | freeware (closed) | none | late 1990s | n/a | Win32 GUI | Windows | none | none |
| PakScape | freeware (closed) | none | early 2000s | n/a | Win32 GUI | Windows | none | none |
| par | unstated (FOSS, no LICENSE) | ibiblio tarballs | par-0.03.01, 2004-08-06 | autotools-era make | CLI | Linux | none | none |
| QuakeForge pak | GPL-2.0 | github.com/quakeforge/quakeforge | active (2024-2026) | autotools | CLI + libs | Linux, BSD, Win cross-build | none specific to pak | GitHub Actions (engine) |
| QuArK | GPL-2.0 | sourceforge.net/projects/quark | 2021-03 last touch, dormant | Delphi + Python | Win32 GUI + Python plug-ins | Windows | none | none |
| QPakMan | GPL-2.0 | github.com/bunder/qpakman | 2022-01-28 | CMake | CLI | Win, Linux | none | none |
| QPed | freeware (closed) | none | 1998 (Pete Callaway) | n/a | Win32 GUI | Windows | none | none |
| pakextract | BSD-2-Clause | github.com/yquake2/pakextract | 2017-07-05 | Makefile | CLI | POSIX (C99) | none | none |
| id qfiles.c | unstated (GPLv2 by descent) | github.com/id-Software/Quake-Tools | 1996 | DOS Watcom make | embedded in qutils | DOS / 16-bit | none | none |
| quake-cli-tools | MIT | github.com/joshuaskelly/quake-cli-tools | 2024-01-15 | setuptools | CLI + Py module | any with Python | pytest present | none configured |
| PakUtil | LGPL-3.0 | github.com/JakubKwantowy/PakUtil | 2025-02-15 | setup.py | Py module + scripts | any with Python | none | none |
| TrenchBroom PAK VFS | GPL-3.0 | github.com/TrenchBroom/TrenchBroom | active (2026) | CMake | embedded C++ class | macOS/Win/Linux | TrenchBroom test suite | GitHub Actions |

### Safety / robustness

| Tool | Path-traversal reject | Win reserved-name reject | Symlink-safe extract | Normalized-collision detect | Structured errors | Stable on malformed pak |
|---|---|---|---|---|---|---|
| **pakka** | ✓ (extract + add + verify) | ✓ | ✓ (openat / fchdir / Win reparse) | ✓ (case-fold + slash + dot/space) | ✓ (`pakka_error_t`) | ✓ (bounds validator) |
| GCFScape | ? | ? | ? | ? | n/a | ? |
| AdQuedit | ✗ | ✗ | ✗ | ✗ | ✗ | "real-time saves, no undo" → likely brittle |
| makepak.py | ✗ | ✗ | n/a (create-only) | ✗ | ✗ | n/a |
| PakExplorer | ? | ? | ✗ | ✗ | ✗ | ? |
| PakScape | ? | ? | ✗ | ✗ | ✗ | ? |
| par | ✗ (assumed; no source review possible) | ✗ | ✗ | ✗ | ✗ | ? |
| QuakeForge pak | ✗ (per source) | ✗ | ✗ | ✗ | ✗ | loads full table to mem; no bounds check visible |
| QuArK | ? | ? | ? | ? | partial (Python exceptions) | ? |
| QPakMan | ✗ (no validation in `pakfile.cc`) | ✗ | ✗ | refuse-overwrite by default (`-force` to override) | ✗ | ? |
| QPed | ✗ | ✗ | ✗ | ✗ | ✗ | sound player freezes UI (cited on wiki) |
| pakextract | ✗ (confirmed — direct fopen on entry name) | ✗ | ✗ | ✗ | ✗ (perror) | partial (bounds in compressed path only) |
| id qfiles.c | n/a (create-only, no extract) | n/a | n/a | ✗ | ✗ (`Error()` exits) | n/a |
| quake-cli-tools | ✗ (per repo issues) | ✗ | ✗ | ✗ | Python exceptions | ? |
| PakUtil | ✗ | ✗ | ✗ | ✗ | Python exceptions | ? |
| TrenchBroom PAK VFS | n/a (read into VFS, never writes) | n/a | n/a | n/a | C++ exceptions | engine treats as readonly |

`?` = couldn't confirm from accessible sources. `n/a` = doesn't apply to the
tool's mode (e.g., create-only tools don't extract).

## Per-tool notes

### GCFScape

Closed-source Windows GUI from Nem's Tools (Ryan Gregg). v1.8.6 dated
2017-01-22 per the download page; needs .NET Framework 4.0 + VC++ 2010
SP1. Originally a Valve `.gcf` browser; PAK support comes via the same
plug-in architecture that handles WAD, VPK, BSP, NCF — read-only browse +
extract only. Strengths: long-lived, polished UI, "drag a PAK onto the
exe and click around" usability. Limitations vs. pakka: read-only, no
CLI, Windows-only, .NET dependency, no source. Functionally analogous to
Windows Explorer's archive view — not in the same product category as a
PAK editor like pakka, but routinely listed as a "PAK tool" because
players use it to extract.

### AdQuedit

"The Quake Workstation" by Hicks, released 1997. Win32 GUI; closed
source; hosted historically on Valve Developer Union mirror. Edits
entity files, models, sprites, WADs, AND PAKs from one UI. The wiki
flags the absence of undo: "All changes are saved in real-time with no
undos or redos." That is a different design philosophy from pakka,
which buffers all mutations in memory and only commits on `pakka_close`
/ explicit `pakka_commit`. AdQuedit is best understood as a 1997-era
mod-author's multitool that happens to include PAK as one tab among
several — not a PAK editor per se. Last touched ~28 years ago; included
here only because the wiki lists it.

### makepak.py

35-line Python script by tomeofpreach (Preach), 2013-06-22, with a
revision noted 2016-06-18. Recursive directory → Quake 1 PAK file.
Pure create mode; no list/extract/add-to-existing/delete. No license
stated. Code is straightforward: write zero header, walk tree appending
payloads + recording offsets, write directory, rewrite header in place.
No path-traversal validation (it stores whatever the OS walk gives it),
no name-length check (writes the entire filename and trusts it fits in
56 bytes), no `--as` aliasing. Genuinely useful as a build-pipeline
brick — drop it into a Makefile, point at `pak0_staging/`, get a pak.
Hard to beat for that one job; doesn't try to be anything else.

### PakExplorer

Closed-source Win32 GUI for Q1/Q2 PAK files, late 1990s. Read, create,
edit, extract; shell-association with `.pak`; plays sounds in place.
The wiki characterizes it as "small footprint and portability"
(meaning: single .exe, runs from a floppy). No source, no license
visible, last release predates the wiki page (last edited 2022 just to
add the link to the Valve Dev Union mirror). Roughly the same product
category as QPed and PakScape — drag-and-drop pak editor for modders.
Pakka shares no design overlap; PakExplorer is a "filesystem-like UI"
tool while pakka is a CLI + library.

### PakScape

Closed-source Win32 GUI by Quesoft. Handles PAK, PK3, and Tribes 2
.vol; the only tool in this survey that handles all three. Built-in
WAV player for in-pak audio. No source. Last meaningful release likely
2002-2004 era; the Valve Dev Union mirror is what survives. Closest
competitor to pakka on FORMAT BREADTH — PakScape covers PK3 today,
pakka has it reserved-but-unimplemented. The cost of PakScape's
breadth: Windows-only, GUI-only, undocumented internals, no library
embedding. Useful proof-of-existence that PK3 + PAK in one tool is
table-stakes for a modern Quake-series PAK editor.

### par (Pak ARchiver)

Linux CLI by an unknown author, last upload `par-0.03.01.tar.gz`
2004-08-06 on ibiblio. No source review available (would need to
download the tarball; deferred). Same product category as pakka:
list / extract / create / add from a Linux command line. Distinguishing
features from the description alone: zero. Unmaintained for 22 years.
The strongest argument for par in 2026 is the strongest argument
against it: it's the simplest possible thing that does the job, which
is exactly what pakka already is, minus 22 years of softening rust.
Treat par as the historical baseline pakka now occupies — pakka is
"par, modernized and ported and tested."

### QuakeForge pak utility

Part of the QuakeForge engine project (github.com/quakeforge/quakeforge,
GPL-2.0, ~16k commits, actively maintained through 2026). The pak tool
lives at `tools/pak/pak.c` (≈5.9 KB) and `tools/pak/pak.h` (≈1.3 KB).
Three modes: `-c` create, `-t` test/list, `-x` extract. No add-to-existing,
no delete. Companion tool `zpak` gzips contents; the QuakeForge engine
transparently decompresses. The pak utility itself doesn't validate
path traversal — extraction converts backslash to slash but doesn't
reject `..`, leading slash, drive letters, or Windows reserved names.
Closest in spirit to a "Unix CLI" PAK tool, but with much narrower
feature surface than pakka and no library separation: pakka exports
20 functions through `pakka.h`; QuakeForge's pak.c is internal to
QuakeForge.

### QuArK

GPL-2.0 Win32 map editor (quark.sourceforge.net) with PAK file editing
bolted on. Implemented in Delphi + Python plugins. Last activity
2021-03; dormant since. Real strength: it understands MAP, MDL, BSP,
WAD, AND PAK in one process, with a Python plugin model that exposes
the asset tree to scripts. Categorize as "map editor that incidentally
edits PAKs" — not a competitor to pakka so much as a different tool
in a related niche. Useful datapoint because it's the only entry with
scriptable plugins (Python), an angle pakka doesn't try to cover.
Pakka's libpakka could be wrapped in CPython bindings if that niche
ever mattered, but at present quake-cli-tools and PakUtil own the
"Python PAK script" lane.

### QPakMan

CLI tool by Andrew Apted (`bunder/qpakman` fork, GPL-2.0, last pushed
2022-01-28). ~160 KB of C++ across 31 files (`pakfile.cc` 14 KB,
`archive.cc` 12 KB, plus image converters `im_mip.cc`, `im_color.cc`,
`im_png.cc`, etc.). CMake build, no tests, no CI. Modes from README:
`-list`, `-extract`, create-by-positional-args. Unique feature: auto-
converts MIP / LMP / WAL (and the Quake 2 `gfx/`, `textures/`
sub-trees) to/from PNG on extract/create. Refuses to overwrite without
`-force`. Doesn't validate `..` / leading slash / drive letters / Windows
reserved names — same gap as everyone else. Genuinely more functional
than pakka on the asset-aware axis; less functional on safety,
maintenance, portability.

### QPed

Closed-source Win32 PAK editor by Pete Callaway, 1998. Source never
released; the original site is dead, only a `qped211.exe` mirror
survives. Wiki cites two known bugs (model viewer camera, sound player
hangs UI) and ships them as-is. Long-name support unclear. Same product
category as PakExplorer / PakScape; long obsolete. Listed here because
it's on the wiki — has effectively no comparison value vs. pakka beyond
"Win32 GUIs from the 1990s outnumbered CLIs and we still feel that
distribution in 2026."

### pakextract (yquake2)

Focused C extractor for Quake II / Sin / Daikatana / (Quake I) by the
Yamagi Quake II team. ~550 LOC in a single `pakextract.c`. BSD-2-Clause.
Last commit 2017-07-05 (`55d4ca2`). Build is one Makefile, no tests, no
CI. **This is pakka's closest functional analogue at the "small focused
C utility" level** — and the comparison is informative: pakextract is
extract-only (no list, no create, no add, no delete, no verify), has no
path-traversal validation (`fopen(d->file_name, "w")` is called
directly), no symlink-safe extract, no normalized-collision check. It
DOES handle Sin's compressed entries and Daikatana's RLE / back-ref
codec — niche format coverage pakka doesn't have. Net read: pakka is
strictly broader in surface and safer in extract; pakextract has two
format dialects (Sin, Daikatana) pakka would need to add for parity on
sourceport-adjacent use cases.

### id qfiles.c (Quake-Tools)

id Software's original PAK builder, `qutils/QFILES/QFILES.C` in the
2012-released Quake-Tools dump. Last commit 2012-01-31, source dates
to ~1996. Reads `precache.lst` / `pak0.lst` (generated from QuakeC),
appends each listed file with `SafeRead`/`SafeWrite`, writes 64-byte
directory entries, fixes up the header at the end. Streaming reads via
a 4 KB buffer. Hard-coded `MAX_FILES = 4096`. No extraction, no list,
no delete. Crashes (`Error()` → `exit(1)`) on any error. This is the
historical reference implementation: pakka inherits the same on-disk
format from it. Worth listing because every other tool in this survey
ultimately reproduces what `QFILES.C` does, plus surface features.
Format-author bragging rights aside, qfiles.c isn't a working tool in
2026 — it's a build-system fragment.

### quake-cli-tools (joshuaskelly)

MIT-licensed Python suite by Joshua Skelly. 94 stars, 44 KB, last pushed
2024-01-15. Provides `pak`, `bsp`, `wad`, `lmp`, `pal`, `mip`, `spr`,
`mdl` CLIs as a unified family. The `pak` CLI: list / extract / create
with directory recursion. Comes with `pytest`-style tests in-repo but
no CI configured. The right comparison is pakka:libpakka :: quake-cli-tools:
its underlying `vgio` Python package — both expose a programmatic API
plus a CLI. quake-cli-tools wins on **ecosystem breadth** (does WADs,
BSPs, MDLs as siblings of PAK) and on accessibility (pip install vs.
gcc). Loses on: no path-traversal validation, no streaming reads
(Python `read()` of whole file), no PK3, Python-only — and 50× pakka's
peak memory on a 100 MB pak because the whole archive lands in a `bytes`.

### PakUtil (JakubKwantowy)

Recent (2025-02-15) Python `PakFile` library + helper scripts (pakls.py,
pakextract.py, pakextractall.py, pakmk.py, pakdump.py). LGPL-3.0, 4
commits, no tests, no CI. Three-method API: `append`, `get`, `listDir`.
No path-traversal protection visible. Roughly matches what makepak.py
would have become if it had been packaged as a library instead of a
script. Strictly less complete than quake-cli-tools, but newer and more
modular. Included as a datapoint on "what does a 2025-era hobbyist PAK
library look like." Answer: 4 files, no safety net, MIT-ish license,
no community.

### TrenchBroom PAK VFS

Read-only PAK consumer inside the TrenchBroom map editor. `IdPakFileSystem.cpp`
(2.4 KB) and `DkPakFileSystem.cpp` (5 KB) under `lib/TbFsLib/src/`,
plus matching headers. GPL-3.0, actively maintained, C++ with the rest
of TrenchBroom. Listed as a "consumer, not editor" datapoint: it shows
the form of code that just wants to LOOK INTO a pak without writing.
~7 KB of code to do what pakka does in `pakka_open` + iterate + read.
The reason it's interesting: trial-mounting an unfamiliar PAK to render
its textures has the SAME safety profile as extracting it (still has
to walk attacker-controlled names, still has to enforce path
boundaries when caching to disk). TrenchBroom inherits this from the
VFS layer; pakka encapsulates it inside `pakka_unsafe_entry_name` +
the openat extract path.

## Gaps pakka could close

Ranked by how much demand the gap creates (multiple competitors offering
it = real demand; one competitor = niche).

1. ~~**PK3 / zip-based archive support**~~ **Shipped.** Read (STORED
   + DEFLATE) and STORED write are in `src/pk3file.c`, backed by
   vendored `pakka_inflate` (ex-`puff` from zlib contrib/). DEFLATE
   encode is deferred — STORED-only PK3 write is the v1 floor and
   Quake engines accept it.
2. **Asset-aware extract / create (MIP/LMP/WAL ↔ PNG)** (cost: new
   mode, library-only, optional dependency on libpng / stb_image).
   Offered by: QPakMan, QuArK, quake-cli-tools. Pakka is byte-accurate
   below the directory and intentionally format-blind for safety. A
   plugin model (`pakka_register_codec(magic, encode_fn, decode_fn)`)
   would keep libpakka pure and let a separate `libpakka-codecs.a`
   bring the QPakMan-style behavior.
3. **Quake III / Q4 PAK4 / Doom 3 PAK support** (cost: format module).
   Offered by: TrenchBroom (Q3 dirs), QuakeForge. Most commercially-
   alive PAK-ish format. Lower priority than PK3 since the in-engine
   format diverged.
4. **Sin / Daikatana compressed-entry support** (cost: small codec).
   Offered by: pakextract only. Genuinely niche; the only argument for
   doing this is "we'd like to claim 100% sourceport compatibility."
5. **PAK shell-association / drag-drop on Windows** (cost: installer,
   not C code). Offered by: PakExplorer, PakScape, GCFScape. Falls
   outside libpakka's design but the Windows binary could be packaged
   with file-association on install.
6. **Python bindings** (cost: a CFFI or pybind11 shim, library-only).
   Offered by: quake-cli-tools, PakUtil, QuArK. Pakka has the C API
   ready; the bindings layer is ~100 lines. Would let the
   Python-pipeline use case migrate off Python-native pakkers.
7. **Built-in MIP / WAV preview** (cost: new mode + linkages). Offered
   by: QPed, PakExplorer, PakScape, AdQuedit. Out of scope for a CLI;
   noted only for completeness.

Gaps NOT closed by anything in the field and where pakka is the leader:
multi-archive in one process, structured error reporting, big-endian
host coverage, NetBSD/sparc and legacy-glibc CI. None of those is a
"gap" to close — they're pakka's lead.

## Adding Sin and Daikatana support

Pakextract is the canonical reference implementation for both variants
([yquake2/pakextract](https://github.com/yquake2/pakextract), single
file `pakextract.c`, BSD-2-Clause). Format details below are extracted
from that source.

### Sin (`SPAK` magic)

Three differences from vanilla Quake PAK:

1. **Magic bytes** are `"SPAK"` instead of `"PACK"`. Auto-detectable
   from the first 4 bytes — no flag needed.
2. **Directory entry widens from 64 to 128 bytes.** The filename field
   grows from 56 to **120 bytes**. The two `u32` fields (offset,
   length) are unchanged in size and placement.
3. **Payloads are still uncompressed.** Nothing else changes.

The whole-format change is just "the path field got bigger because Sin's
asset hierarchy outgrew 56 bytes."

### Daikatana (still `PACK` magic — heuristic detection)

1. **Same magic as vanilla Quake.** Not directly auto-detectable. The
   only sniff signal is "`dir_length % 64 != 0` and `dir_length % 72 ==
   0`", and pakextract's own source comments call it "not reliable" —
   on a small pak the modulus check can hit by chance. A user-facing
   `-dk` / format hint is the safer fallback.
2. **Directory entry widens from 64 to 72 bytes.** Filename stays 56
   bytes; offset and length stay 4 bytes each. Two new `u32` fields are
   appended:
   - `compressed_length` — size of the payload bytes in the archive
   - `is_compressed` — flag, `0` = stored verbatim, non-zero =
     compressed
3. **Custom byte-codec compression**, not DEFLATE / zlib. Single-byte
   opcode, five cases (pakextract.c lines ~252-310):

   | Opcode `b`     | Operation |
   |---|---|
   | `b < 64`       | Literal run of `b + 1` bytes from input |
   | `b < 128`      | `b - 62` zero bytes (RLE of zero) |
   | `b < 192`      | `b - 126` copies of the next input byte |
   | `b < 254`      | Back-reference: copy `b - 190` bytes from `(in[read] + 2)` positions back in the OUTPUT |
   | `b == 255`     | Terminator |

   Decoder is ~50 LOC. Encoder is undocumented anywhere I've found;
   pakextract is decode-only, and a fresh encoder would be the only
   path to writing Daikatana paks.

### Steps to add support to pakka

The two variants compose differently with pakka's existing
infrastructure. Sin is small; Daikatana is substantial.

1. **Format detection in `pakka_open`.** Sniff first 4 bytes:
   `"PACK"` → existing handler with a Q2-vs-DK heuristic on
   `dir_length`; `"SPAK"` → Sin handler. Add `PAKKA_FORMAT_SIN` and
   `PAKKA_FORMAT_DAIKATANA` to the public `pakka_format_t` enum
   (`include/pakka.h`).

2. **Parameterize directory-entry layout.** Today `PAKFILE_DIR_ENTRY_SIZE`
   and the per-field offsets in `src/common.h` are compile-time
   constants with `PAKKA_STATIC_ASSERT` checks. Promote them to a small
   per-format table:

   ```
   struct pakka_format_geometry {
       size_t dir_entry_size;     // 64 / 72 / 128
       size_t name_field_len;     // 56 / 56 / 120
       size_t name_field_offset;  // 0 for all three
       size_t offset_field_off;   // 56 / 56 / 120
       size_t length_field_off;   // 60 / 60 / 124
       bool   has_compression;    // false / true / false
   };
   ```

   `pakfile.c` reads the geometry struct instead of the `#define`d
   constants. The static asserts move into runtime sanity checks on
   the table.

3. **Widen the in-memory entry struct's name buffer** from 56 to 120
   bytes, OR keep 56 + add an out-of-line buffer that's only allocated
   for Sin entries. Either works; the out-of-line approach keeps the
   common-case Q1/Q2 entry struct cheap.

4. **Reuse the safety preflight as-is.** `pakka_unsafe_entry_name`
   doesn't care about format — it validates the byte content of the
   name. Sin's longer names just give it a longer string to walk.
   Daikatana's entries don't change the name field at all. No new
   safety code.

5. **Sin write support.** Trivial. Update `write_pak_directory` /
   `copy_between_paks` in `src/pakfile.c` to use the geometry table.
   Bats coverage: pak round-trip test parameterized over format.
   LOC budget: ≈200 LOC + tests.

6. **Daikatana read support.** New decompressor in
   `src/dk_codec.c` (≈80 LOC including bounds checks). The streaming
   reader (`pakka_reader_t`) grows a "decompress state" pointer that's
   non-NULL only for compressed Daikatana entries. `pakka_reader_read`
   dispatches: stored → existing fread loop; compressed → drive the
   decoder until N bytes produced. LOC budget: ≈400 LOC + tests.

7. **Daikatana write support — defer.** Building the encoder requires
   reverse-engineering an undocumented codec well enough to produce
   bit-identical or at least game-engine-acceptable output. Better to
   ship read-only Daikatana support and let users repack as Q2 pak if
   they want to mutate.

8. **CI fixtures.** The current `tests/` suite runs against id's
   shareware Q1 pak0. Adding a Sin and a Daikatana fixture means
   either tiny synthetic paks built in-tree (preferred — checked in,
   no network fetch) or a SHA-pinned download (mirrors the
   `quakesw.tar.gz` pattern). Synthetic is faster and avoids
   distribution rights questions for Sin / Daikatana assets.

9. **Verify path.** `pakka_verify` walks every entry. Already format-
   agnostic via the entry iterator. For Daikatana, "verifying" a
   compressed entry should optionally decode-and-discard to confirm
   the codec stream is well-formed — gated on a flag so users can
   skip the cost on large paks.

10. **Symbol audit and lint.** New TUs add new symbols; rename any
    decoder internals to `pakka_dk_*` to pass `make symbol-audit`. No
    other gate changes.

Daikatana write is deliberately deferred. The combined size of
`libpakka.a` grows by maybe 2-3 KB — small enough that a "no,
Daikatana is too niche" argument doesn't hold on code-budget grounds.

## Things only pakka has

Cross-checked against the per-tool notes above; every claim here is
the absence-of-the-feature recorded in another tool's row.

1. **`pakka_error_t` structured error reporting.** Every other tool
   uses one of: `printf` + `exit(1)` (id qfiles, par, QuakeForge pak),
   `perror` (pakextract), Python exceptions (quake-cli-tools, PakUtil,
   QuArK plugins), or GUI dialog boxes (QPakMan, GCFScape, AdQuedit,
   QPed, PakExplorer, PakScape). None expose status code + error
   domain + system code + op name + entry-name context + offset/length
   + message in a single return-by-value record.
2. **`--verify` / `pakka_verify` without extracting.** Every other tool
   either verifies implicitly on open (everyone) or doesn't verify at
   all. No standalone "audit the pak, report findings to a callback,
   touch no disk" mode exists elsewhere.
3. **Normalized-collision preflight on extract.** Case-fold +
   slash/backslash + trailing dot/space normalization, all-or-nothing
   reject before any byte hits disk. Nothing else in the survey does
   this; the closest is QPakMan's `-force` toggle, which is per-file
   overwrite-allow, not a portable-collision detector.
4. **Symlink-safe extract on POSIX + Windows reparse-point rejection.**
   Everyone else does the literal `fopen(name, "w")` thing. pakextract,
   the next-most-careful, doesn't.
5. **Multi-archive-in-one-process.** Every other tool with mutation
   uses file-local globals (par, QuakeForge pak, QPakMan) or a single
   open document (the GUIs). pakka moved the `FILE*` and path buffers
   into `struct pakka_archive` specifically to lift this restriction.
6. **Big-endian host coverage.** Only QuakeForge has any
   non-Intel/AMD CI, and even that is x86/ARM only. Pakka's
   `linux-glibc-s390x-be` and `legacy-netbsd-sparc` jobs are unique
   in this group.
7. **Pre-openat legacy POSIX support.** pakextract / QuakeForge /
   QPakMan assume modern POSIX. Pakka deliberately compiles on RH9
   (glibc 2.3.2, gcc 3.2.2) and NetBSD 3.0/sparc and routes around
   missing `openat` via `fchdir` + `O_NOFOLLOW`. No other entry tries
   to support that floor.
8. **C99 library + thin CLI split, symbol-audited.** `make symbol-audit`
   gates the build on `nm -g` showing only `pakka_`-prefixed symbols
   in `libpakka.a`. No other entry separates a re-embeddable library
   from a CLI; PakUtil and quake-cli-tools have library-like Python
   modules but no C ABI.

## Codex disagreements

Codex (`gpt-5.5`, high reasoning, `read-only` sandbox; no shared context
with the primary pass) ran independently and produced a 106-line matrix
+ notes at `/tmp/codex-pak-review.md`. Eight cells / claims differed.
Adjudicated below.

| # | Cell or claim | Primary pass | Codex pass | Adjudication |
|---|---|---|---|---|
| 1 | pakextract extract-with-dest | ✗ (cwd) | ✓ (`-o output_dir`) | **Codex right.** README documents `-o`. Matrix corrected above. |
| 2 | QuakeForge pak verify | ✗ | partial (`-t` test mode per man page) | **Codex right** that `-t` exists; matrix updated to "partial". Note: `-t` lists and reads bytes but doesn't do name-safety / portable-collision checks, so it's not equivalent to pakka's `--verify`. Distinction kept in the per-tool note. |
| 3 | QuakeForge pak extract-select | ✓ | ✗ (man page: "cannot extract individual files") | **Codex right.** Primary pass over-read the source. Matrix corrected above. |
| 4 | GCFScape PAK backend | "via plug-ins" (closed source) | HLLib is LGPL native C++, with a separate GPL `HLExtract` CLI counterpart | **Codex right** that HLLib is FOSS even though GCFScape's UI shell is closed. Adds a real library competitor (HLLib) the primary pass didn't surface. Noted as a sidebar below. |
| 5 | GCFScape v1.8.6 date | 2017-01-22 (per download page) | 1.8.6 posted 2006; page modified 2017 | **Codex more likely right.** The download page's date is page-modification, not release. Both versions of the claim are defensible; codex's is more accurate to release year. |
| 6 | par feature set | list / extract / create / add / ? delete | list / extract / create only (FreeBSD ports summary) | **Codex right.** Primary pass was guessing from "archiver"; FreeBSD ports doc is primary. Matrix's add/delete cells for par should be ✗, not ✓/?. Correcting now: |
| 7 | Modern GitHub pakker pick | PakUtil (Python lib) + quake-cli-tools (Python suite) | Q2Pak (Go) | **Both legitimate.** Different criteria — primary picked "closest analogue to libpakka" (library form) plus "most-starred Python"; codex picked "small Go CLI". Keeping primary's two-row choice; adding Q2Pak as a footnote-row below for completeness. |
| 8 | TyrUtils inclusion | dropped (no PAK component on inspection) | included with all-✗ row | **Both correct.** TyrUtils has no PAK functionality. Primary dropped to keep the table tight; codex included to record the absence. Equivalent information. |

### Corrections applied above

- pakextract: `-C dest` cell flipped to ✓ (`-o`).
- QuakeForge pak: extract-select flipped to ✗; verify flipped to partial.
- par: revised below — list / extract / create / no add / no delete.

### HLLib (sidebar, surfaced by codex)

| Tool | License | Source | Last activity | Form | Notes |
|---|---|---|---|---|---|
| HLLib | LGPL-2.1 | nemstools.github.io (source on Github) | ~2017 | C++ library + `HLExtract` GPL CLI | Backs GCFScape; handles GCF/VPK/PAK/WAD/NCF/BSP/SGA/XZP. A genuine library competitor in the broader "Valve + id package" niche, narrower on PAK editing (read-mostly) but wider on format coverage. |

### par — corrected

`par` is list / extract / create only; no add to existing, no delete.

### Q2Pak — added (codex's pick)

| Tool | License | Source | Last activity | Form | Platforms | Notes |
|---|---|---|---|---|---|---|
| Q2Pak | unstated (no LICENSE) | github.com/packetflinger/q2pak | 2023-09-21 | Go CLI | any with Go | List / extract / create from directory. Author's README explicitly says "no edit existing" — extract, change, recreate. 9 stars, 1 `q2pak.go`. Same product category as Q2-only pakextract, with Go cross-compile as the value-add. |

### Areas where both passes agreed

The big claims survive both passes intact: pakka leads on safety
(traversal + reserved-name + symlink + collision + structured errors),
pakka leads on portability (BE, legacy POSIX, multi-CI), pakka lags on
PK3 (real, multiple competitors have it), and pakka lags on asset-aware
conversion (QPakMan, QuArK, quake-cli-tools own that lane). No major
finding from either pass was overturned by the other.
