# Red Hat Linux 9 Legacy Build Probe

This directory captures the one-off Red Hat Linux 9 build probe for Pakka and
keeps enough tooling in-tree to reproduce it. Large binary artifacts are
intentionally not checked in: the Red Hat ISO images, kickstart floppy image,
source tarball, and QEMU disk live under `PAKKA_RH9_WORKDIR`, defaulting to
`/private/tmp/pakka-rh9`.

## Contents

* `assets/ks.cfg` - kickstart used for the install. It installs RH9 from CD,
  enables SSH, installs the legacy C toolchain, and writes
  `/root/run-pakka-build.sh` into the guest.
* `assets/MD5SUM` - Red Hat's signed MD5 listing from the public legacy mirror,
  included as pinned provenance for the three Shrike i386 binary CDs.
* `scripts/fetch-isos.sh` - downloads and verifies the RH9 i386 CD images.
* `scripts/make-kickstart-floppy.sh` - creates the FAT floppy image consumed by
  Anaconda as `ks=floppy`.
* `scripts/install-vm.sh` - boots the RH9 installer under QEMU with CD #1.
* `scripts/change-cd.sh` - switches the QEMU CD-ROM to CD #2 or #3 during
  install via the monitor socket.
* `scripts/run-vm.sh` - boots the installed guest with SSH forwarded to the
  host.
* `scripts/run-build.sh` - rebuilds a source tarball from this checkout, serves
  it to the guest, and runs the Pakka build via `/root/run-pakka-build.sh`
  (installed by the kickstart).
* `results/` - logs from the 2026-05-17 baseline run, captured before the
  legacy-floor compatibility work landed.

## Reproduction

Host prerequisites:

* `qemu-system-i386` and `qemu-img`
* `curl`
* `python3`
* `sshpass` and OpenSSH
* `bsdtar`
* macOS `hdiutil`, or Linux `mtools` (`mformat` and `mcopy`)
* `ncat` or `nc` with Unix-domain socket support, for scripted CD swaps

The full RH9 install is not fully unattended because Anaconda asks for CD #2
when it reaches `gcc-3.2.2-5`.

```sh
dev/legacy/rh9/scripts/install-vm.sh
```

When the installer prompts `Please insert disc 2 to continue`, run this from a
second shell:

```sh
dev/legacy/rh9/scripts/change-cd.sh 2
```

Then press Enter in the QEMU console. `install-vm.sh` uses `-no-reboot`, so it
exits after the installer's first reboot. Boot the installed guest:

```sh
dev/legacy/rh9/scripts/run-vm.sh
```

In another shell, run the build probe:

```sh
dev/legacy/rh9/scripts/run-build.sh
```

Useful environment overrides:

* `PAKKA_RH9_WORKDIR` - cache and VM workspace, default
  `/private/tmp/pakka-rh9`
* `PAKKA_RH9_SSH_PORT` - host SSH forward, default `10022`
* `PAKKA_RH9_RAM` - QEMU RAM in MiB, default `512`

## Approaches Tried

OrbStack was considered but not used for the primary test. It can provide an
old userland/container experiment, but it still shares a modern Linux kernel,
so it does not answer whether Pakka builds and runs on a 2003-vintage Linux OS.

QEMU with the official Red Hat Linux 9 i386 install media was used for the
authoritative run. The three public Shrike binary CDs were downloaded from:

`https://legacy.redhat.com/pub/redhat/linux/9/en/iso/i386/`

Their MD5s matched Red Hat's `MD5SUM`:

* `shrike-i386-disc1.iso` - `34048ce4cd069b624f6e021ba63ecde5`
* `shrike-i386-disc2.iso` - `6b8ba42f56b397d536826c78c9679c0a`
* `shrike-i386-disc3.iso` - `af38ac4316ba20df2dec5f990913396d`

Two automation paths were rejected during setup:

* HTTP install tree: a merged `RedHat/` tree from all three CDs was served over
  local HTTP. The RH9 loader accepted the kickstart but failed before the host
  HTTP server saw a `netstg2.img` request.
* Home-made combined ISO: a merged ISO containing all RPMs was built, but RH9
  Anaconda's CD probe rejected it as not being a Red Hat Linux CD.

The working install path was the conservative one: official CD #1, kickstart on
a floppy image, and a QEMU monitor CD swap to official CD #2 when Anaconda
requested it.

## Guest environment

```text
Red Hat Linux release 9 (Shrike)
Linux pakka-rh9 2.4.20-8 #1 Thu Mar 13 17:54:28 EST 2003 i686 i686 i386 GNU/Linux
gcc (GCC) 3.2.2 20030222 (Red Hat Linux 3.2.2-5)
GNU Make version 3.79.1
```

## Baseline failures (2026-05-17, captured in `results/`)

The unmodified build at that point failed twice. First on GNU make:

```text
make: *** No rule to make target `|', needed by `build/obj/common.o'.  Stop.
```

— order-only prerequisite syntax arrived after GNU make 3.79.1. Then on
the symlink-safe extraction path:

```text
src/compat.c:427: `O_DIRECTORY' undeclared
src/compat.c:427: `O_CLOEXEC' undeclared
src/compat.c:449: warning: implicit declaration of function `openat'
src/compat.c:451: `O_NOFOLLOW' undeclared
src/compat.c:466: warning: implicit declaration of function `mkdirat'
```

— `openat`/`mkdirat` are glibc 2.4 (2006), and glibc 2.3.2 doesn't expose
`O_NOFOLLOW`/`O_DIRECTORY` under `_XOPEN_SOURCE=700`.

## Current status

Both issues are fixed in-tree:

* The Makefile uses inline `mkdir -p $(@D)` instead of order-only
  prerequisites, so GNU make 3.79.1 accepts it.
* `src/platform.c` swaps the openat-based descent for an `fchdir` +
  `open(O_NOFOLLOW)` emulation when `!__GLIBC_PREREQ(2,4)`. Symlink
  refusal stays atomic per descent step; the mkdir→reopen window on
  missing intermediates is the same one the modern path has between
  mkdirat and the next openat.

The QEMU probe in this directory remains the authoritative real-hardware
verifier. Faster CI coverage (Sarge-derived and RH9-from-vault-RPMs
Docker images) lives under `dev/legacy/ci/`.

## Out of scope

Running the bats integration suite inside RH9 isn't covered yet — bash
2.05b is too old for bats-core 1.x. A host-driven smoke fixture or a
vendored mini-bats would be a separate piece of work.
