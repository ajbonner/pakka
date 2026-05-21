# NetBSD 3.0 / sparc Legacy Build Probe

QEMU-based NetBSD 3.0/sparc build probe for Pakka. Mirrors
`dev/legacy/rh9/` in shape: install media + disk image live outside
the repo under `PAKKA_NBSD_WORKDIR` (default
`/private/tmp/pakka-netbsd-sparc`), scripts here automate fetch /
boot / build.

NetBSD/sparc adds two coverage dimensions beyond the existing CI:

* **BSD libc on a big-endian host.** `linux-glibc-s390x-be` exercises
  big-endian byte order with glibc; NetBSD/sparc exercises it with BSD
  libc (different `mkstemp`, `dirname`, `O_NOFOLLOW` enablement history).
* **Pre-openat libc.** NetBSD 3.0 (2005) predates `openat`/`mkdirat`
  (NetBSD 6.0, 2012) and `O_DIRECTORY` (NetBSD 4.0). This probe is what
  surfaced the original `PAKKA_LEGACY_EXTRACT` BSD gap — the gate now
  fires for old BSDs as well as old glibc.

NetBSD 3.0 is chosen for the toolchain version: it ships gcc 3.3.3 by
default, which sits above the documented `gcc 3.0+` floor. NetBSD 1.6
(the period-accurate Quake-era release) ships gcc 2.95 and is below
floor.

## Status

* QEMU boot from `sparccd-3.0.iso`: verified
* `sysinst` install onto a fresh qcow2 target: verified
* Cross-host `scp` + `ssh` to the installed guest: verified (legacy
  KEX/cipher overrides documented in `run-build.sh`)
* `make` build + symlink-safe extract smoke: verified — see
  `results/2026-05-17-netbsd-sparc-build.log`
* Per-push CI coverage via the `legacy-netbsd-sparc` job in
  `.github/workflows/test.yml`: verified. The job downloads a
  pre-built disk image from the
  [`legacy-netbsd-sparc-disk-v1`](https://github.com/ajbonner/pakka/releases/tag/legacy-netbsd-sparc-disk-v1)
  GitHub release (144 MB compressed qcow2, SHA-256 pinned in the
  workflow), boots `qemu-system-sparc -M SS-5` on ubuntu-latest, and
  runs the same build + smoke. ~10 min wall-clock — sparc TCG is the
  slowest job in the matrix.

The build smoke catches a pre-openat libc regression class: pakka's
`platform.c` swaps to the `fchdir` + `O_NOFOLLOW` legacy extract path
when the libc lacks `openat`. Before that path was BSD-gated, NetBSD
3.0 failed compile on `O_CLOEXEC` and `openat`. The fix shipped in the
same commit that captured this probe (see git log for details).

## CI artifact regeneration

The pre-built disk image is intentionally pinned by SHA in the
workflow so a guest update is an explicit, reviewable event. To
regenerate:

1. Run `scripts/install-vm.sh` to produce a fresh installed qcow2 in
   `$PAKKA_NBSD_WORKDIR`. Follow the inline sysinst hints to enable
   sshd persistently and set the root password to `pakka`.
2. After the guest reboots into the installed system (`run-vm.sh`),
   SSH in and build GNU Make 3.81 from upstream source — NetBSD 3.0/
   sparc has no usable pkgsrc binary archive past 7.0:
   ```
   ftp -V -4 -o /tmp/make.tgz http://ftp.gnu.org/gnu/make/make-3.81.tar.gz
   cd /tmp && tar xzf make.tgz && cd make-3.81
   ./configure --prefix=/usr/local --without-guile && make && make install
   ```
3. Halt the guest cleanly: `ssh root@... '/sbin/halt -p'`.
4. Compress: `qemu-img convert -O qcow2 -c netbsd-sparc.qcow2 disk.qcow2`.
5. Re-upload with a bumped release tag (e.g.
   `legacy-netbsd-sparc-disk-v2`) and update both the `DISK_URL` and
   `DISK_SHA256` in `.github/workflows/test.yml`.

## Contents

* `assets/SHA512SUMS` — pinned SHA-512 for the install ISO. Sourced
  from NetBSD's published `SHA512` file in the archive.
* `scripts/fetch-iso.sh` — downloads `sparccd-3.0.iso`, verifies SHA-512.
* `scripts/install-vm.sh` — boots `qemu-system-sparc -M SS-5` with the
  CD attached and a fresh qcow2 target at sd0. `-boot d` boots the CD;
  sysinst runs interactively over the serial console.
* `scripts/run-vm.sh` — boots the installed target disk by itself.
* `scripts/prepare-source-tarball.sh` — packs the working tree.
* `scripts/run-build.sh` — `scp` the source tarball into the running
  guest, run `gmake` + the banner + symlink-safe extract smoke, and
  print the captured log.
* `results/2026-05-17-netbsd-sparc-build.log` — captured build +
  smoke output from the actual probe.

## Reproduction

Host prerequisites:

* `qemu-system-sparc` (Homebrew: `brew install qemu`)
* `qemu-img`
* `curl`, `gunzip`, `shasum -a 512`
* `sshpass`
* `tar`

```sh
# 1. Fetch + verify install media (~155 MB).
dev/legacy/netbsd-sparc/scripts/fetch-iso.sh

# 2. Interactive sysinst install (one-time, ~15 minutes).
#    install-vm.sh prints hints at start; key choices:
#      - install target: sd0 (the empty 2 GB disk; the CD is sd2)
#      - install source: CD-ROM (everything is on the local ISO)
#      - root password: pakka (must match PAKKA_NBSD_ROOT_PASSWORD)
#      - in the post-install Utility-menu shell, mount /dev/sd0a /mnt
#        and append `sshd=YES`, `dhclient=YES`,
#        `dhclient_flags=le0` to /mnt/etc/rc.conf, plus
#        `PermitRootLogin yes` to /mnt/etc/ssh/sshd_config.
#    QEMU exits at the sysinst reboot (-no-reboot).
dev/legacy/netbsd-sparc/scripts/install-vm.sh

# 3. Boot the installed disk in one shell. (Subsequent steps SSH in
#    from the host; the install-env shell of step 2 is gone.)
dev/legacy/netbsd-sparc/scripts/run-vm.sh

# 4. SSH into the running guest (root / pakka) and build GNU make
#    3.81 from upstream source — NetBSD 3.0/sparc has no usable
#    pkgsrc binary archive (vendor only keeps 7.0+). One-time bootstrap:
#      ftp -V -4 -o /tmp/make.tgz http://ftp.gnu.org/gnu/make/make-3.81.tar.gz
#      cd /tmp && tar xzf make.tgz && cd make-3.81
#      ./configure --prefix=/usr/local --without-guile && make && make install

# 5. In another shell on the host, build pakka + run smoke tests
#    against the running guest:
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
  `https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-3.0`.
* `PAKKA_NBSD_KEY` — value of the archive's `?key=` query parameter
  ("trivial botcatcher"). Defaults to `NetBSD`. Set empty to omit the
  query if the upstream scheme changes.

## Why not fully automated?

NetBSD 3.0's `sysinst` is curses-driven, with no record/replay or
preseed equivalent. The realistic options for full automation are:

1. **Manual install once, snapshot disk.** Save the resulting qcow2 as
   a host-side artifact and reuse it. Few hundred MB once gzipped —
   too big for git; could live in a GitHub release.
2. **Expect-script the serial console.** Possible but brittle: every
   sysinst version offsets prompts differently.
3. **Bypass sysinst, build the disk directly** by extracting `base.tgz`
   etc. into a labelled disk and installing boot blocks. Doable but
   reimplements sysinst.

Today the install is documented and verified; the actual automation
of options 1-3 is deferred follow-up.
