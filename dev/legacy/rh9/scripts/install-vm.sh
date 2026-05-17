#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
DISK=${PAKKA_RH9_DISK:-"$WORKDIR/rh9-pakka.qcow2"}
MONITOR=${PAKKA_RH9_MONITOR:-"$WORKDIR/qemu-monitor.sock"}
SSH_PORT=${PAKKA_RH9_SSH_PORT:-10022}
RAM=${PAKKA_RH9_RAM:-512}

"$SCRIPT_DIR/fetch-isos.sh"
"$SCRIPT_DIR/make-kickstart-floppy.sh"
"$SCRIPT_DIR/prepare-source-tarball.sh"

mkdir -p "$WORKDIR/boot"
if [ ! -f "$WORKDIR/boot/isolinux/vmlinuz" ] || [ ! -f "$WORKDIR/boot/isolinux/initrd.img" ]; then
  if ! command -v bsdtar >/dev/null 2>&1; then
    echo "Need bsdtar to extract the Red Hat 9 boot kernel and initrd from disc 1" >&2
    exit 1
  fi
  bsdtar -xf "$WORKDIR/isos/shrike-i386-disc1.iso" -C "$WORKDIR/boot" \
    isolinux/vmlinuz isolinux/initrd.img
fi

if [ ! -f "$DISK" ]; then
  qemu-img create -f qcow2 "$DISK" 6G
fi

rm -f "$MONITOR"

cat <<EOF
Starting Red Hat 9 installer.

When Anaconda prompts "Please insert disc 2 to continue", run this from another shell:

  $SCRIPT_DIR/change-cd.sh 2

Then press Enter in the QEMU console. The install exits after its first reboot.
EOF

exec qemu-system-i386 \
  -M pc \
  -m "$RAM" \
  -drive "file=$DISK,format=qcow2,if=ide" \
  -drive "file=$WORKDIR/ks.img,format=raw,if=floppy" \
  -cdrom "$WORKDIR/isos/shrike-i386-disc1.iso" \
  -kernel "$WORKDIR/boot/isolinux/vmlinuz" \
  -initrd "$WORKDIR/boot/isolinux/initrd.img" \
  -append "ks=floppy text console=ttyS0 devfs=nomount ramdisk_size=8192" \
  -net nic,model=rtl8139 \
  -net "user,hostfwd=tcp::$SSH_PORT-:22" \
  -monitor "unix:$MONITOR,server,nowait" \
  -nographic \
  -no-reboot
