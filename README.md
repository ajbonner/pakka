# Pakka
A command line utility for working with quake 1 and 2 .pak files.

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden](http://www.inthenightgarden.co.uk/) character.

## Disclaimer
I am a novice C programmer. This code is the result of a learning exercise.
Given that C programs can be very brittle, and behave in unexpected ways,
I would strongly suggest you do not use this software :) If you are feeling
very brave and do choose to use this software and find a problem, please let
me know!

## Installation
    $ git clone https://github.com/ajbonner/pakka.git
    $ make

Requires GNU make. On BSD systems, install `gmake` and use `gmake` in place of
`make`.

## Usage
Pakka has 5 major modes:

* List pak contents: `./pakka -lf <pakfile.pak>`
* Extract: `./pakka -xf <pakfile.pak> [-C <destination>] [path...]`
  * With no path arguments, every entry is extracted.
  * With one or more path arguments, only those entries are extracted; pakka
    errors if any requested path isn't in the pak.
  * `-C` selects a destination directory; defaults to the current working
    directory.
* Create: `./pakka -cf <pakfile.pak> [file/dir...]`
* Add to pak: `./pakka -af <pakfile.pak> [file/dir...]`
* Delete from pak: `./pakka -df <pakfile.pak> [path...]`

`./pakka -h` prints a usage summary.

## Testing
The repo includes an integration test suite that downloads id's shareware Quake
`pak0.pak` (with a pinned SHA256) and exercises pakka against it: round-trip,
extract-specific, add, delete (including the head, the tail, and every entry),
and rejection of malformed paks.

    $ make test

Requires [bats-core](https://github.com/bats-core/bats-core)
(`brew install bats-core`, `apt install bats`, `pkg install bats-core`, etc.),
plus `curl`, `openssl`, and `tar` for fetching and verifying the test fixture.

`make lint` runs [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with
a curated check set (`.clang-tidy` at the repo root). On macOS, install via
`brew install llvm` and either add `$(brew --prefix llvm)/bin` to `PATH` or
invoke as `make CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy lint`. On Linux:
`apt install clang-tidy`.

## Supported Platforms
The code aims to be POSIX-portable across Linux, BSD, and macOS — little-endian,
32 or 64 bit. CI verifies builds and the full bats suite on:

| OS | Architecture | libc / toolchain |
|---|---|---|
| macOS 26 | Intel x86_64 | Apple clang |
| macOS 26 | Apple Silicon arm64 | Apple clang |
| Ubuntu latest | x86_64 | glibc / gcc |
| Ubuntu 24.04 | arm64 | glibc / gcc |
| Alpine 3.23 | x86_64 | musl / gcc |
| Ubuntu latest | x86_64 (`-m32`) | glibc / gcc (32-bit ABI) |
| FreeBSD 15.0 | amd64 | clang |
| FreeBSD 15.0 | amd64 (`-m32`) | clang (32-bit ABI) |
| OpenBSD 7.8 | amd64 | clang |

The `-m32` jobs build 32-bit binaries on a 64-bit kernel/libc — they exercise
the 32-bit code path but aren't a true i386 OS.

## Contributing
1. Fork it!
2. Create your feature branch: `git checkout -b my-new-feature`
3. Commit your changes: `git commit -am 'Add some feature'`
4. Push to the branch: `git push origin my-new-feature`
5. Submit a pull request :D

## History
Created as an excuse to re-learn some C circa December '15–January '16.
Picked up again in 2026 for a substantial cleanup pass — warning-free under
`-Wall --std=c99 --pedantic`, several real bugs fixed, integration test suite
added, and multi-platform CI added.

## Credits
John Carmack for not only creating quake, but then open sourcing it and its
successor games. He is the reason I am a programmer today.

I am unsure who the correct author is, and the original link is now long since
dead, but I followed
[Quake PAK File Format](https://web.archive.org/web/20160711041711/https://debian.fmi.uni-sofia.bg/~sergei/cgsr/docs/pak.txt)
in building this utility.

## License
MIT
