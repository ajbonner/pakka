#!/bin/sh
# Fetch the NetBSD 3.0 sparc install miniroot + binary sets the
# install needs to bootstrap a usable build environment for pakka.
# Verifies every download against ../assets/SHA512SUMS.
#
# Mirror note: archive.netbsd.org gates binary downloads behind a
# `?key=NetBSD` query parameter ("trivial botcatcher"). We pass it on
# every fetch so curl doesn't bounce off the 402 response.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
MIRROR=${PAKKA_NBSD_MIRROR:-https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-3.0/sparc}
KEY=${PAKKA_NBSD_KEY:-NetBSD}

mkdir -p "$WORKDIR/installation/miniroot" "$WORKDIR/binary/sets"

# Files to fetch: relative paths under the sparc/ tree.
files="
installation/miniroot/miniroot.fs.gz
binary/sets/base.tgz
binary/sets/comp.tgz
binary/sets/etc.tgz
binary/sets/text.tgz
binary/sets/kern-GENERIC.tgz
"

# Empty PAKKA_NBSD_KEY omits the query parameter entirely so the
# script keeps working if archive.netbsd.org drops the bot-catcher.
if [ -n "$KEY" ]; then
    Q="?key=$KEY"
else
    Q=""
fi

for path in $files; do
    out="$WORKDIR/$path"
    if [ -f "$out" ]; then
        echo "==> Already have $path"
        continue
    fi
    echo "==> Fetching $path"
    curl -fsSL -o "$out" "$MIRROR/$path$Q"
done

echo "==> Verifying SHA512s against assets/SHA512SUMS"
(
    cd "$WORKDIR"
    shasum -a 512 -c "$SCRIPT_DIR/../assets/SHA512SUMS"
)

# Pre-extract the miniroot for the install boot. NetBSD's installer
# treats this as the boot disk: qemu-system-sparc reads it as raw and
# OpenBIOS hands control to NetBSD's secondary boot inside it.
if [ ! -f "$WORKDIR/installation/miniroot/miniroot.fs" ]; then
    gunzip -k "$WORKDIR/installation/miniroot/miniroot.fs.gz"
fi

echo "==> Ready in $WORKDIR"
