# Testing

The full C test suite runs from the repo root with:

    $ make test

That builds and runs `symbol-audit`, `c_api_test`, `dk_codec_test`,
and a per-format binary (`pakka_test`, `pk3_test`, `pk4_test`,
`dk_test`, `sin_test`, `wad_test`, `unicode_paths_test`,
`large_file_test`). No bats, no shell harness — every case is in
`test/*_test.c`.

`make test` covers round-trip, extract-specific, add, delete
(including the head, the tail, and every entry), `--verify`, `--as`
aliasing, and rejection of malformed paks. The Quake fixture (id's
shareware `pak0.pak`) is downloaded once with a pinned SHA-256.

Requires a C99 compiler, `make`, `curl`, `openssl`, and `tar`
(`unzip` only for the optional GoldSrc realpak suites).

## Notable test binaries

* **`symbol-audit`** — runs `nm -g build/lib-test/libpakka.a` and
  fails if any defined global lacks the `pakka_` prefix. Keeps the
  library namespace-clean.
* **`c_api_test`** — a C-API exerciser linked only against the
  public header. Covers NULL tolerance, round-trips, structured-
  error population, and the `pakka_open_entry` /
  `pakka_reader_read` streaming API that black-box CLI tests
  can't reach.

## Running tests on Windows

The Windows test path is CMake + Ninja + CTest. No bash, no MSYS2.
From a stock PowerShell or `cmd.exe`:

    cmake -B build/cmake -G Ninja -DPAKKA_TEST_BUILD=ON .
    cmake --build build/cmake
    pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite quake
    ctest --test-dir build/cmake --output-on-failure

`PAKKA_TEST_BUILD=ON` mirrors the Makefile's `make test` flag so the
fault-injection hooks in `src/platform.c` compile in (NOP at runtime
when `PAKKA_INJECT_FAULT_AT` is unset).

`dev/win/fixtures.ps1` is the PowerShell-native equivalent of `make
fixture` / `make verify-*`. It and the Makefile share fixture URLs
plus SHA-256 pins via `dev/fixtures.mk` — edit that file when a
fixture source moves.

Other fixture suites:

    pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite q3              # Q3 demo pak0.pk3
    pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite goldsrc-uplink  # Half-Life Uplink valve/pak0.pak
    pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite goldsrc-dayone  # Half-Life Day One valve/pak0.pak

## Realpak suites

Optional, opt-in suites that exercise pakka against real
game-engine archives. Not part of `make test` because of the
download sizes and archive.org's intermittent availability.

    $ make realpak-test-q3       # Q3 demo wrapper (~93 MiB)
    $ make realpak-test-goldsrc  # Half-Life Uplink + Day One (~138 MiB)
    $ make realpak-test          # umbrella that runs both

`realpak-test-q3` downloads id's Q3 demo wrapper from archive.org
(SHA-256-pinned), uses pakka itself to extract the inner 47 MiB
`pak0.pk3` (1,274 real id-built entries), and runs the full read /
list / extract / structural-verify / deep-verify sequence against
it.

On Windows the same coverage is reached through CTest. Set the
fixture environment variable to the path `fixtures.ps1` produces,
then filter the CTest run:

    $env:Q3DEMO_PAK0_PK3 = "$pwd\build\test\q3demo\pak0.pk3"
    ctest --test-dir build/cmake -R pk3_q3demo_test --output-on-failure

    $env:GOLDSRC_UPLINK_PAK0 = "$pwd\build\test\hl-uplink\valve\pak0.pak"
    $env:GOLDSRC_DAYONE_PAK0 = "$pwd\build\test\hl-dayone\valve\pak0.pak"
    ctest --test-dir build/cmake -R pak_goldsrc_test --output-on-failure

## Lint

    $ make lint

Runs [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with the
curated check set in `.clang-tidy` at the repo root.

On macOS, install via `brew install llvm` and either add
`$(brew --prefix llvm)/bin` to `PATH` or invoke as:

    $ make CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy lint

On Linux: `apt install clang-tidy`.
