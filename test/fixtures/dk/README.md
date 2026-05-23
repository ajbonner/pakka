# Daikatana fixture

Cross-validates pakka's DK decoder against an external encoder's
output, complementing the synthetic in-process round-trip tests
(`test/dk.bats`, `test/dk_codec_test.c`) that only exercise pakka
against itself.

## Layout

- `inputs/` — four small files of synthetic bytes, committed to git
  so the bats consumer can `cmp` extracted payloads against the
  source. Regenerate with `python3 inputs/generate.py` — the script
  is deterministic across machines, Python versions, and endianness.
- `inputs/dkpak_output.txt` — captured stdout from the `dkpak`
  invocation that built `user.pak`. Kept as a transcript of the
  per-entry compression decisions and totals; informational only.
- `user.pak` — DK pak produced by packing `inputs/*` through
  Daikatana's `dkpak` packer in `-userpak` mode. Tracked in git,
  consumed by the `dk real fixture` bats case. **The packed bytes are
  exclusively from `inputs/`; no game assets are included.**

## What each input exercises

| Input file    | Bytes | Entry name in pak        | Encoder outcome     | Why |
| ------------- | ----- | ------------------------ | ------------------- | --- |
| `zeros.bmp`   |  1024 | `textures/zeros.bmp`     | compressed, 17 B    | `.bmp` listed; all zeros — zero-run tokens. |
| `striped.tga` |   512 | `textures/striped.tga`   | compressed, 30 B    | `.tga` listed; repeating pattern — back-refs. |
| `random.bsp`  |   256 | `maps/random.bsp`        | STORED (256 B)      | `.bsp` listed, but encoder produced 261 B (>256); STORED fallback. |
| `notes.txt`   |    33 | `notes.txt`              | STORED (33 B)       | `.txt` not listed. |

The `maps/` and `textures/` prefixes come from how the inputs were
staged before invoking dkpak, not from any rule the tool enforces.
In `-userpak` mode dkpak walks whatever directory tree the user
provides and preserves the relative path of each file in the entry
name; files at the staging root (like `notes.txt`) have no prefix.

Without `-userpak`, dkpak runs in its default 3-pak mode — auto-
allocating files by type into separate `base`, `models`, and `maps`
output paks (per Section 4 of `dktools_readme.htm`, John Romero / Ion
Storm 2000). `-userpak` collapses that into a single file and uses
the user's own layout instead. With no filename argument, the output
is named `user.pak`; with one (e.g. `dkpak -userpak PAK9.PAK`), the
argument wins.

The committed fixture uses the `maps/`+`textures/`+root layout that
matches dktools_readme.htm's user-pak authoring convention. The bats
consumer pins these names when comparing extracted payloads to
`inputs/`; a different staging layout would need matching updates in
`test/dk.bats`.

## Regenerating

If you need to rebuild `user.pak` (e.g. you changed the inputs):

```sh
cd test/fixtures/dk
python3 inputs/generate.py             # idempotent; overwrites the four files

# 1. Stage the inputs into whatever directory tree you want preserved
#    in the pak. The currently-committed fixture uses:
#       <staging>/maps/random.bsp
#       <staging>/textures/zeros.bmp
#       <staging>/textures/striped.tga
#       <staging>/notes.txt
#    Any other layout works — `-userpak` ships the user's tree as-is.
#    If you change the layout, also update the entry-name patterns in
#    test/dk.bats's "dk real fixture" case.
#
# 2. Run `dkpak.exe -userpak` from <staging> (under Wine on macOS /
#    Linux). With no filename arg, dkpak writes user.pak in the
#    current directory.
#
# 3. Move the result back here:
#       mv <staging>/user.pak test/fixtures/dk/user.pak
#
# inputs/dkpak_output.txt is the captured stdout from a successful
# regen run, kept as a transcript of the per-entry compression
# decisions and totals.
```

The bats consumer reads only `user.pak` and `inputs/*` — it makes no
assumption about how `user.pak` was produced. Any DK packer that emits
the same four routed names is a valid fixture.

## Fixture required

`test/dk.bats` hard-fails the `dk real fixture: ...` case when
`user.pak` is missing — the pak is committed and CI should not
silently lose this coverage to an accidental deletion.
