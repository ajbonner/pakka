#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ASSET_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../assets" && pwd)
WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
OUT=${PAKKA_RH9_KS_IMG:-"$WORKDIR/ks.img"}

mkdir -p "$WORKDIR"

if command -v hdiutil >/dev/null 2>&1; then
  DMG="$WORKDIR/ks.dmg"
  MNT="$WORKDIR/ksmnt"
  rm -f "$DMG" "$OUT"
  mkdir -p "$MNT"
  hdiutil create -size 1440k -fs MS-DOS -volname KS -layout NONE "$DMG" >/dev/null
  DEV=$(hdiutil attach "$DMG" -mountpoint "$MNT" -nobrowse | awk 'NR == 1 {print $1}')
  cp "$ASSET_DIR/ks.cfg" "$MNT/ks.cfg"
  hdiutil detach "$DEV" >/dev/null
  cp "$DMG" "$OUT"
elif command -v mformat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
  rm -f "$OUT"
  dd if=/dev/zero of="$OUT" bs=1024 count=1440
  mformat -i "$OUT" -f 1440 ::
  mcopy -i "$OUT" "$ASSET_DIR/ks.cfg" ::ks.cfg
else
  echo "Need either hdiutil or mtools (mformat and mcopy) to create $OUT" >&2
  exit 1
fi

echo "Created $OUT"
