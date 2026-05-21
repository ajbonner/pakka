#!/bin/sh
# Build + smoke-test pakka inside one of the legacy CI images. Source
# is bind-mounted at /src as read-only; we copy into a writable temp
# directory so the host checkout doesn't end up with ELF i386 build
# artifacts after a local `act` run.
set -eu

# Legacy mktemp (RH9's mktemp-1.5) requires an explicit template.
WORK=$(mktemp -d /tmp/pakka.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
cp -a /src/. "$WORK/"
cd "$WORK"

echo "== system =="
uname -a || true
if [ -r /etc/redhat-release ]; then
  cat /etc/redhat-release
elif [ -r /etc/os-release ]; then
  . /etc/os-release && echo "${PRETTY_NAME:-unknown}"
elif [ -r /etc/debian_version ]; then
  echo "Debian $(cat /etc/debian_version)"
fi

echo "== toolchain =="
gcc --version | head -1
make --version | head -1

echo "== build =="
make

echo "== smoke: banner =="
# `./pakka -h` prints help and exits 1 by design (no mode selected),
# so check the banner reached stdout/stderr instead of relying on $?.
if ! ./pakka -h 2>&1 | head -1 | grep -q '^Pakka '; then
    echo "smoke test failed: pakka banner not printed" >&2
    exit 1
fi

# The whole point of the PAKKA_LEGACY_EXTRACT branch is the fchdir +
# O_NOFOLLOW descent at runtime. Mirror the symlink fixture from
# tests/pakka.bats so legacy CI fails loudly if that path stops
# refusing pre-planted symlinks. Without this, a regression in
# platform_open_extract_target would slip past CI because the build
# itself doesn't exercise extract.
echo "== smoke: symlink-safe extract =="
mkdir -p "$WORK/scratch/out" "$WORK/scratch/outside"
{
    printf 'PACK\014\000\000\000\100\000\000\000'
    printf 'models/x'
    dd if=/dev/zero bs=1 count=48 2>/dev/null
    printf '\114\000\000\000\000\000\000\000'
} > "$WORK/scratch/ok.pak"
ln -s "$WORK/scratch/outside" "$WORK/scratch/out/models"
if ./pakka -x -C "$WORK/scratch/out" "$WORK/scratch/ok.pak" 2>/dev/null; then
    echo "smoke test failed: pakka extracted through a symlink" >&2
    exit 1
fi
if [ -e "$WORK/scratch/outside/x" ]; then
    echo "smoke test failed: write escaped to outside dir" >&2
    exit 1
fi

# And a happy-path extract proves the fchdir descent works for
# normal entries — without it the negative test above would pass
# trivially if extract was just broken.
rm -f "$WORK/scratch/out/models"
mkdir -p "$WORK/scratch/out2"
./pakka -x -C "$WORK/scratch/out2" "$WORK/scratch/ok.pak"
[ -f "$WORK/scratch/out2/models/x" ] \
    || (echo "smoke test failed: extract did not create models/x" >&2; exit 1)

echo "== all smoke checks passed =="
