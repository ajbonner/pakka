# Quake-Era Legacy Target Plan

Pakka exists to inspect and rewrite Quake-family `.pak` archives, so legacy
platform coverage should prefer machines where Quake, Quake II, or close
Quake-engine games plausibly used those archives. This plan extends the current
legacy Linux i386 work with BSD and non-x86 targets that are historically
relevant and likely to expose real portability bugs.

## Selection Criteria

Targets are useful when they satisfy most of these:

* Late-1990s or early-2000s operating system and toolchain.
* Direct or near-direct Quake/Quake II `.pak` relevance.
* Meaningful portability coverage beyond current Linux/MSVC CI.
* Reproducible path through QEMU, preserved install media, source packages, or
  real hardware.
* Small enough scope for a C99 CLI smoke test; graphics/audio support is not a
  requirement.

The current RH9 and legacy CI work already covers old Linux/i386, glibc 2.3.x,
gcc 3.x, GNU make 3.79.1, and Linux 2.4-era assumptions. The targets below
should add BSD libc/userland behavior, big-endian systems, 64-bit non-x86, and
old workstation Unix behavior.

## Recommended Matrix

| Priority | Target | Why it fits | What it proves | Expected route |
| --- | --- | --- | --- | --- |
| P0 | FreeBSD 4.11/i386 | Best legacy BSD baseline. FreeBSD 4.x was common in the era and Quake users could run ports or Linux Quake/Quake II binaries through compatibility layers. | BSD libc/userland, old BSD filesystem behavior, non-GNU userland assumptions, little-endian 32-bit. | QEMU i386 install from archived FreeBSD 4.11 media; build `pakka`; run smoke tests against Quake shareware and synthetic hostile paks. |
| P0 | Solaris 2.6 or 7/SPARC | Strongest non-x86 Quake-era Unix target. Quake had a SPARC Solaris port, and Quake II dedicated server builds existed for Solaris SPARC. | Big-endian SPARC, old SysV/Solaris libc, 32-bit non-x86, stricter POSIX gaps. | QEMU SPARC if practical, otherwise documented hardware/manual probe. Prefer gcc from period packages unless native Sun compiler support is explicitly desired. |
| P1 | IRIX 6.5/MIPS | SGI Quake used Quake `.pak` files directly and SGI workstations were a real Quake-era niche. | Big-endian MIPS, IRIX libc/toolchain quirks, workstation Unix path and archive behavior. | Exploratory manual probe first. QEMU/MAME feasibility is uncertain; real hardware or preserved IRIX environment may be required. |
| P1 | Linux/Alpha | Quake II dedicated server builds existed for Alpha Linux/glibc. Less client-relevant than Solaris or IRIX, but historically tied to Quake II server use. | 64-bit non-x86 LP64, little-endian Alpha, old glibc on a non-i386 ABI. | QEMU Alpha with a period Linux distribution, or Debian/Red Hat Alpha media if obtainable. Build-only plus archive smoke tests is sufficient. |
| P2 | Classic Mac OS or early Darwin/PowerPC | Commercial Mac Quake and Quake II ports had real `.pak` use. Classic Mac OS is not a POSIX target; early Darwin is more buildable but less faithful to the original game ports. | Big-endian PowerPC and, if Classic Mac OS is attempted, case-insensitive HFS path behavior. | Park until the Unix targets above are stable. Consider Darwin/PPC as a pragmatic proxy only if the goal is C CLI portability rather than historical runtime fidelity. |

## Execution Order

1. Add a FreeBSD 4.11/i386 probe.
   This is the most practical way to close the legacy BSD gap. Keep it similar
   to the RH9 probe: build from a source snapshot, run `./pakka -h`, then run a
   small smoke suite for list, extract, create, delete, and symlink/path
   traversal refusal.

2. Add a Solaris/SPARC probe.
   This is the highest-value non-x86 target because it combines Quake-era
   relevance with big-endian coverage. Start with build-only plus the same smoke
   tests. Avoid adding Solaris-specific code unless the failure is clearly a
   portability boundary and not a missing local package.

3. Investigate IRIX/MIPS feasibility.
   Treat this as a research branch until install media, compiler availability,
   and emulator or hardware access are clear. If it is too expensive to automate,
   keep a manual reproduction log under `dev/legacy/irix/`.

4. Add Linux/Alpha if 64-bit non-x86 coverage still looks valuable.
   This should come after SPARC or IRIX, because Alpha is little-endian and will
   not catch byte-order bugs. It does catch `long`, pointer-width, and old glibc
   behavior on a non-i386 ABI.

5. Revisit PowerPC only after the Unix matrix is stable.
   PowerPC is historically valid, but classic Mac portability is a different
   project from maintaining a portable POSIX CLI. Use it later if there is a
   clear need for big-endian PPC or case-insensitive filesystem coverage.

## Smoke Test Shape

Each target should start with the same minimal checks:

* Build `pakka` with the target's native make/compiler setup.
* Confirm `./pakka -h` prints the banner.
* List the Quake shareware `pak0.pak` fixture when available.
* Create a small pak, list it, extract it, delete all entries, and verify the
  empty-pak header behavior.
* Extract a synthetic path-traversal pak and confirm absolute paths, drive-like
  prefixes, backslash traversal, and `..` components are rejected.
* Extract through a pre-planted symlink where the target filesystem supports
  symlinks, and confirm writes do not escape the destination.

Keep tests shell-portable. Bats is not assumed to run on these systems.

## Deprioritized Targets

NetBSD/macppc, NetBSD/sparc, OpenBSD/sparc, OpenBSD/macppc, HPPA, m68k, MIPS
Linux, and ARM/OABI are historically interesting, but they are not first-line
targets for this project. They either have weaker direct Quake/Quake II `.pak`
lineage, duplicate the coverage from Solaris/IRIX/PowerPC, or are likely to
consume more setup time than they return in portability signal.

If Solaris/SPARC or IRIX/MIPS proves impractical, NetBSD 1.6 on SPARC or macppc
is the best fallback for big-endian non-x86 coverage. It should be documented
as a portability proxy, not as an id-era Quake runtime target.

## Evidence Notes

Primary evidence gathered for this target choice:

* Quake's GPL source release notes shared source ancestry across DOS, Linux,
  and Windows, and keeps Quake data files separate from engine source:
  <https://github.com/id-Software/Quake>
* Quake II's GPL source tree includes Unix platform directories such as
  `linux`, `solaris`, `irix`, `rhapsody`, and `unix`:
  <https://github.com/id-Software/Quake-2>
* Linux Quake II setup references the original `baseq2/*.pak` files and id's
  Unix/Linux binaries:
  <https://tldp.org/HOWTO/archived/Game-Server-HOWTO/quake2.html>
* SGI Quake documentation says the `pak[01].pak` files belong under `id1/`
  beside the Quake executable:
  <https://nekofiles.irixnet.org/Games/Quake/quake/doc/quake1.html>
* FreeBSD 4.11 was the final FreeBSD 4.x legacy release and supported i386 and
  Alpha:
  <https://www.freebsd.org/releases/4.11R/announce/>
