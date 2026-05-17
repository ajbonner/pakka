#!/bin/sh
# Boot NetBSD 3.0/sparc miniroot under qemu-system-sparc and step the
# operator through sysinst. Creates a fresh target disk image and an
# HTTP server on host:8000 that serves binary sets the guest fetches
# during install. The install is interactive — sysinst on NetBSD 3.0
# has no kickstart-equivalent — so this script just sets up the
# environment and hands you the serial console.
#
# Once you finish sysinst and reboot, terminate this script (Ctrl-A x
# in QEMU serial console) and use run-vm.sh for subsequent boots.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
DISK_SIZE=${PAKKA_NBSD_DISK_SIZE:-2G}
RAM_MB=${PAKKA_NBSD_RAM:-256}
SSH_PORT=${PAKKA_NBSD_SSH_PORT:-10022}
HTTP_PORT=${PAKKA_NBSD_HTTP_PORT:-8000}

"$SCRIPT_DIR/fetch-iso.sh"

MINIROOT="$WORKDIR/installation/miniroot/miniroot.fs"
TARGET_DISK="$WORKDIR/netbsd-sparc.qcow2"

if [ ! -f "$MINIROOT" ]; then
    echo "Missing $MINIROOT — fetch-iso.sh did not extract it" >&2
    exit 1
fi

if [ -f "$TARGET_DISK" ]; then
    echo "==> Target disk $TARGET_DISK already exists; reusing"
else
    echo "==> Creating $TARGET_DISK ($DISK_SIZE)"
    qemu-img create -f qcow2 "$TARGET_DISK" "$DISK_SIZE"
fi

# Serve binary sets to the guest. NetBSD's sysinst can fetch sets from
# an HTTP source ("Install from: http"); point it at 10.0.2.2 (QEMU's
# default host alias) and the path /binary/sets.
echo "==> Serving $WORKDIR over HTTP on $HTTP_PORT (Ctrl-C this script
    once sysinst finishes and the guest reboots)"
python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0 --directory "$WORKDIR" &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null || true' EXIT INT TERM

cat <<NOTES
== sysinst hints ==
* When sysinst asks for terminal type, choose vt100.
* Install target: sd0 (the empty 2 GB disk). The miniroot we booted
  from is sd1; do not pick it.
* For "Install from", choose http with:
    host    : 10.0.2.2
    port    : 8000
    URL     : /binary/sets
  Choose the base, comp, etc, kern-GENERIC, and text sets. Skip x*
  sets — pakka doesn't need X11.
* In the post-install configuration menu set the root password (the
  run-build.sh default is "pakka"; override via PAKKA_NBSD_ROOT_PASSWORD).
* Then "Configure network" → enable DHCP on the lance NIC.
* Finally enter the post-install shell ("e: Utility menu"
  → "a: Run /bin/sh"). sysinst mounts the install target at
  /targetroot, so edit there to make sshd survive reboot:
    echo 'sshd=YES' >> /targetroot/etc/rc.conf
    echo 'PermitRootLogin yes' >> /targetroot/etc/ssh/sshd_config
* Install gmake from NetBSD's pkgsrc binary archive (pakka's Makefile
  needs GNU make; base.tgz only ships BSD make). After reboot, log in
  as root and run:
    PKG_PATH=http://ftp.netbsd.org/pub/pkgsrc/packages/NetBSD/sparc/3.0/All \\
      pkg_add gmake
* Reboot. Pick "Reboot the computer" from the sysinst menu, then
  Ctrl-A x in this serial console once the login prompt appears.
  Hand off to run-vm.sh.
NOTES

# SCSI topology: target install disk at unit 0 (sd0), miniroot at unit 1.
# sysinst writes /etc/fstab against sd0, and run-vm.sh boots sd0 as the
# only disk after install, so the on-disk fstab matches the runtime
# device. `boot-device=disk1` makes OpenBIOS auto-boot from the
# miniroot during install without an operator prompt.
qemu-system-sparc \
    -M SS-5 \
    -m "$RAM_MB" \
    -nographic \
    -prom-env "boot-device=disk1" \
    -drive file="$TARGET_DISK",format=qcow2,if=scsi,bus=0,unit=0,media=disk \
    -drive file="$MINIROOT",format=raw,if=scsi,bus=0,unit=1,media=disk \
    -net nic,model=lance \
    -net user,hostfwd=tcp::"$SSH_PORT"-:22 \
    -no-reboot
qemu_rc=$?
exit "$qemu_rc"
