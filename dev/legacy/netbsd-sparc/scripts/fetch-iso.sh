#!/bin/sh
# Fetch the NetBSD 3.0 sparc install ISO from the archive mirror,
# verify SHA-512, and stage it for install-vm.sh.
#
# Mirror note: archive.netbsd.org gates binary downloads behind a
# `?key=NetBSD` query parameter ("trivial botcatcher"). We pass it on
# every fetch so curl doesn't bounce off the 402 response. Set
# PAKKA_NBSD_KEY="" to drop the parameter if the upstream scheme
# changes.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
MIRROR=${PAKKA_NBSD_MIRROR:-https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-3.0}
KEY=${PAKKA_NBSD_KEY:-NetBSD}

mkdir -p "$WORKDIR/iso"

if [ -n "$KEY" ]; then
    Q="?key=$KEY"
else
    Q=""
fi

ISO_PATH="iso/sparccd-3.0.iso"
if [ -f "$WORKDIR/$ISO_PATH" ]; then
    echo "==> Already have $ISO_PATH"
else
    echo "==> Fetching $ISO_PATH (~155 MB)"
    curl -fsSL -o "$WORKDIR/$ISO_PATH" "$MIRROR/$ISO_PATH$Q"
fi

echo "==> Verifying SHA-512 against assets/SHA512SUMS"
( cd "$WORKDIR" && shasum -a 512 -c "$SCRIPT_DIR/../assets/SHA512SUMS" )

echo "==> Ready in $WORKDIR"
