#!/bin/sh
# Boot the previously-installed NetBSD 3.0/sparc disk image. Mirrors
# dev/legacy/rh9/scripts/run-vm.sh: forward host:10022 to guest:22 so
# run-build.sh can SSH in and drive the build.
set -eu

WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
RAM_MB=${PAKKA_NBSD_RAM:-256}
SSH_PORT=${PAKKA_NBSD_SSH_PORT:-10022}

TARGET_DISK="$WORKDIR/netbsd-sparc.qcow2"

if [ ! -f "$TARGET_DISK" ]; then
    echo "No installed disk at $TARGET_DISK — run install-vm.sh first" >&2
    exit 1
fi

exec qemu-system-sparc \
    -M SS-5 \
    -m "$RAM_MB" \
    -nographic \
    -drive file="$TARGET_DISK",format=qcow2,if=scsi,bus=0,unit=0,media=disk \
    -net nic,model=lance \
    -net user,hostfwd=tcp::"$SSH_PORT"-:22
