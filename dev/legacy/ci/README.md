# `dev/legacy/ci/` — Docker assets for the Linux legacy-floor CI jobs

This directory holds the build inputs for pakka's two Linux
legacy-floor lanes: `legacy-floor` and `legacy-rh9`. Both jobs run on
every push (`.github/workflows/test.yml`) and catch regressions
against pakka's documented legacy minimums:

* gcc 3.0+
* glibc 2.2.5+
* GNU make 3.79.1+
* Linux kernel 2.4+

Files:

| File | Purpose |
|---|---|
| `Dockerfile` | Debian 3.1 Sarge userland (apt-fetched from `archive.debian.org`) with GNU make 3.79.1 compiled from a SHA-pinned tarball on top of the apt-shipped 3.80. Gives CI a single image with the full RH9-era toolchain floor — glibc 2.3.2, gcc 3.3, make 3.79.1 — without needing a literal Red Hat rootfs. |
| `Dockerfile.rh9` | Literal Red Hat Linux 9 (Shrike, March 2003) userland assembled `FROM scratch` by `rpm2cpio`-ing SHA-pinned vault RPMs into a rootfs. glibc 2.3.2, gcc 3.2.2, make 3.79.1 are the *actual* RH9 binaries, not a Debian proxy. The Sarge image catches portability issues earlier; the RH9 image confirms pakka builds on the real distro it claims to support. |
| `rh9-rpms.sha256` | Manifest of every RPM `Dockerfile.rh9` fetches, with SHA-256 hashes pinned against the `legacy.redhat.com` vault. Must be a complete closure (`rpm2cpio` skips RPM dependency resolution), so any new system library reference needs a matching line here. |
| `build.sh` | Build runner shared by both images. Copies `/src` (bind-mounted read-only from the host checkout) into a writable scratch dir, runs `make`, prints the toolchain banner, and exercises the `PAKKA_LEGACY_EXTRACT` symlink-rejection path through a small fixture pak. Smoke only — the full C test suite isn't run here because the minimal userland on these guests can't host every transitive build dependency. |

## When to update

* **GNU make version bump**: edit `MAKE_URL` and `MAKE_SHA256` in
  `Dockerfile`. The compile-from-source step exists specifically
  because Sarge ships make 3.80; if the floor moves above 3.80, the
  whole stage-1 fetch becomes unnecessary.
* **New RH9 library reference**: add the corresponding RPM to
  `rh9-rpms.sha256` with its SHA-256. `rpm2cpio` won't pull
  transitive deps automatically. The Stage 1 chroot check
  (`bash -c 'gcc --version && make --version'`) catches missing
  shared-lib references before the real build.
* **`build.sh`**: edit when changing what the check exercises.
  Keep it shell-portable — RH9's `mktemp` requires an explicit
  template, busybox-era `grep` may not support `-E`, etc.

## Related

* `dev/legacy/rh9/README.md` — the manual QEMU-RH9 probe used to
  validate this CI lane against a full RH9 install (booted under
  QEMU on x86_64) before it landed in `.github/workflows/test.yml`.
* `dev/legacy/netbsd-sparc/README.md` — a separate legacy probe
  (BSD libc + big-endian SPARC + the pre-openat
  `PAKKA_LEGACY_EXTRACT` BSD branch).
