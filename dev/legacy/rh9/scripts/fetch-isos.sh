#!/bin/sh
set -eu

WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
ISO_DIR="$WORKDIR/isos"
BASE_URL=${PAKKA_RH9_BASE_URL:-https://legacy.redhat.com/pub/redhat/linux/9/en/iso/i386}

mkdir -p "$ISO_DIR"
cd "$ISO_DIR"

fetch() {
  url=$1
  out=$2
  if [ -f "$out" ]; then
    echo "Already have $out"
  else
    curl -fL --progress-bar -O "$url"
  fi
}

fetch "$BASE_URL/MD5SUM" MD5SUM
fetch "$BASE_URL/shrike-i386-disc1.iso" shrike-i386-disc1.iso
fetch "$BASE_URL/shrike-i386-disc2.iso" shrike-i386-disc2.iso
fetch "$BASE_URL/shrike-i386-disc3.iso" shrike-i386-disc3.iso

verify_one() {
  expected=$1
  file=$2
  if command -v md5sum >/dev/null 2>&1; then
    actual=$(md5sum "$file" | awk '{print $1}')
  elif command -v md5 >/dev/null 2>&1; then
    actual=$(md5 -q "$file")
  else
    echo "Need md5sum or md5 to verify $file" >&2
    exit 1
  fi

  if [ "$actual" != "$expected" ]; then
    echo "MD5 mismatch for $file: expected $expected, got $actual" >&2
    exit 1
  fi
  echo "MD5 OK: $file"
}

verify_one 34048ce4cd069b624f6e021ba63ecde5 shrike-i386-disc1.iso
verify_one 6b8ba42f56b397d536826c78c9679c0a shrike-i386-disc2.iso
verify_one af38ac4316ba20df2dec5f990913396d shrike-i386-disc3.iso
