#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
MONITOR=${PAKKA_RH9_MONITOR:-"$WORKDIR/qemu-monitor.sock"}
DISC=${1:?usage: change-cd.sh <1|2|3|path-to-iso>}

case "$DISC" in
  1|2|3)
    ISO="$WORKDIR/isos/shrike-i386-disc${DISC}.iso"
    ;;
  *)
    ISO="$DISC"
    ;;
esac

if [ ! -S "$MONITOR" ]; then
  echo "QEMU monitor socket not found: $MONITOR" >&2
  echo "Start the installer with $SCRIPT_DIR/install-vm.sh first." >&2
  exit 1
fi

if [ ! -f "$ISO" ]; then
  echo "ISO not found: $ISO" >&2
  exit 1
fi

send_cmd() {
  if command -v ncat >/dev/null 2>&1; then
    printf '%s\n' "$1" | ncat -U "$MONITOR"
  elif command -v nc >/dev/null 2>&1; then
    printf '%s\n' "$1" | nc -U "$MONITOR"
  else
    echo "Need ncat or nc with Unix-socket support to talk to $MONITOR" >&2
    exit 1
  fi
}

send_cmd "change ide1-cd0 $ISO"
echo "Changed ide1-cd0 to $ISO"
