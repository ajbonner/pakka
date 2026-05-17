# NetBSD 3.0 / sparc Legacy Build Probe

This directory captures the QEMU-based NetBSD/sparc probe for Pakka. It
mirrors `dev/legacy/rh9/` in shape: large install media live outside the
repo under `PAKKA_NBSD_WORKDIR` (default `/private/tmp/pakka-netbsd-sparc`),
and the scripts here automate fetch / boot / build.

NetBSD/sparc adds two coverage dimensions beyond the existing CI:

* **BSD libc on a big-endian host.** `linux-glibc-s390x-be` exercises
  big-endian byte order with glibc; NetBSD/sparc exercises it with BSD
  libc (different `mkstemp`, `dirname`, `O_NOFOLLOW` enablement history).
* **SPARC strict alignment.** sun4m enforces stricter alignment than
  x86 or s390x. Low marginal value given pakka's byte-stream I/O, but a
  useful canary.

NetBSD 3.0 is chosen for the toolchain version: it ships gcc 3.3.3 by
default, which sits above the documented `gcc 3.0+` floor. NetBSD 1.6
(the period-accurate Quake-era release) ships gcc 2.95 and is below
floor; bumping to 2.0 (Dec 2004) or 3.0 (Dec 2005) is the closest we
can get to "Quake-era BSD" while keeping the C99 floor intact.

## Contents

* `assets/SHA512SUMS` — pinned SHA-512 sums for the install miniroot
  and the binary sets the install pulls. Sourced from NetBSD's own
  per-directory `SHA512` files in the archive.
* `scripts/fetch-iso.sh` — downloads miniroot.fs.gz and the binary
  sets we need (`base`, `comp`, `etc`, `text`, `kern-GENERIC`),
  verifies SHAs, and pre-gunzips the miniroot.
* `scripts/install-vm.sh` — boots the miniroot under `qemu-system-sparc`
  for an interactive `sysinst` install onto a fresh qcow2 target disk.
  Hosts the binary sets on `http://10.0.2.2:8000/` for sysinst's
  "Install from HTTP" path.
* `scripts/run-vm.sh` — boots the installed disk for subsequent runs.
* `scripts/prepare-source-tarball.sh` — packs the working tree into
  `pakka-src.tar.gz` for the guest to fetch.
* `scripts/run-build.sh` — SSHs into the running guest, fetches the
  source tarball over HTTP, builds with `gmake` (pkgsrc), and runs the
  banner smoke. Mirrors `dev/legacy/rh9/scripts/run-build.sh`.
* `results/` — will hold logs once an install + build cycle is captured.

## Reproduction

Host prerequisites:

* `qemu-system-sparc` (Homebrew `brew install qemu` ships it; Linux:
  `qemu-system-sparc` package)
* `qemu-img`
* `curl`, `gunzip`, `shasum -a 512`
* `python3` (HTTP serve for the install + build)
* `sshpass` (for `run-build.sh`)

Steps:

```sh
# 1. Fetch + verify install media (~30 MB total).
dev/legacy/netbsd-sparc/scripts/fetch-iso.sh

# 2. Interactive install. install-vm.sh boots the miniroot on sd1
#    while leaving the empty target disk at sd0, so sysinst writes
#    /etc/fstab against the device the post-install kernel will
#    actually see. Hints are printed at start; key ones:
#      - install target: sd0 (the 2 GB empty disk)
#      - install from http://10.0.2.2:8000/binary/sets
#      - choose base, comp, etc, kern-GENERIC, text; skip x* sets
#      - in the post-install shell, append `sshd=YES` to
#        /targetroot/etc/rc.conf (sysinst mounts the install target
#        at /targetroot, not /mnt)
#      - after reboot, `PKG_PATH=...3.0/All pkg_add gmake` so pakka's
#        GNU-make-only Makefile builds (base.tgz ships BSD make only)
dev/legacy/netbsd-sparc/scripts/install-vm.sh

# 3. Boot the installed disk in one shell. (No miniroot mounted now;
#    target is the only disk, at sd0, matching what sysinst wrote.)
dev/legacy/netbsd-sparc/scripts/run-vm.sh

# 4. In another shell, build pakka + smoke-test:
dev/legacy/netbsd-sparc/scripts/run-build.sh
```

## Environment overrides

* `PAKKA_NBSD_WORKDIR` — cache + disk image dir, default
  `/private/tmp/pakka-netbsd-sparc`.
* `PAKKA_NBSD_SSH_PORT` — host port forwarded to guest SSH, default
  `10022`.
* `PAKKA_NBSD_RAM` — guest RAM in MiB, default `256`.
* `PAKKA_NBSD_DISK_SIZE` — install disk size, default `2G`.
* `PAKKA_NBSD_ROOT_PASSWORD` — what `run-build.sh` ssh's with, default
  `pakka`. Must match what you set during sysinst.
* `PAKKA_NBSD_MIRROR` — base URL of the NetBSD archive. Defaults to
  `https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-3.0/sparc`.
* `PAKKA_NBSD_KEY` — value of the archive's `?key=` query parameter
  ("trivial botcatcher"). Defaults to `NetBSD`. Drop or change if the
  archive's bot-detection scheme changes.

## Bot-catcher note

`archive.netbsd.org` returns HTTP 402 ("Payment Required") with a
small HTML form for binary downloads unless `?key=NetBSD` is appended
to the URL. The fetch script does this automatically. If the archive
changes the scheme, update `PAKKA_NBSD_KEY` or the URL builder in
`scripts/fetch-iso.sh`.

## Status

Boot is verified — `qemu-system-sparc -M SS-5` running NetBSD 3.0
miniroot reaches kernel device-probe and the install kernel banner
prints over the serial console. The interactive sysinst step has not
been captured into automation yet (NetBSD 3.0's sysinst pre-dates the
`sysinst -j` scripted-install flag from later releases). An actual
installed disk image + build log will land under `results/` once the
manual install completes.

## Why not a fully automated install?

NetBSD 3.0's `sysinst` is curses-driven, with no record/replay or
preseed equivalent on the level of Anaconda kickstart. The realistic
options are:

1. **Manual install once, snapshot disk.** Save the resulting qcow2
   as a host-side artifact and reuse it. Disk image is a few hundred
   MB once gzipped — too big for git; could live in a GitHub release.
2. **Expect-script the serial console.** Possible but brittle: every
   sysinst version offsets prompts differently.
3. **Bypass sysinst, build the disk by extracting `base.tgz` + a
   custom `/etc/fstab` directly.** Doable, but reimplements sysinst.

Option 1 is the path most NetBSD-on-QEMU tutorials take. Option 3 is
the path the legacy CI (`dev/legacy/ci/Dockerfile.rh9`) takes for the
RH9 image — same idea, different OS. Either is a follow-up.
