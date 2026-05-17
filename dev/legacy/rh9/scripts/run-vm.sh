#!/bin/sh
set -eu

WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
DISK=${PAKKA_RH9_DISK:-"$WORKDIR/rh9-pakka.qcow2"}
SSH_PORT=${PAKKA_RH9_SSH_PORT:-10022}
RAM=${PAKKA_RH9_RAM:-512}

if [ ! -f "$DISK" ]; then
  echo "VM disk not found: $DISK" >&2
  echo "Run dev/legacy/rh9/scripts/install-vm.sh first." >&2
  exit 1
fi

exec qemu-system-i386 \
  -M pc \
  -m "$RAM" \
  -drive "file=$DISK,format=qcow2,if=ide" \
  -net nic,model=rtl8139 \
  -net "user,hostfwd=tcp::$SSH_PORT-:22" \
  -nographic \
  -no-reboot
