#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
HTTP_DIR="$WORKDIR/http"

mkdir -p "$HTTP_DIR"
cd "$REPO_ROOT"

tar --exclude .git --exclude build --exclude pakka \
  -czf "$HTTP_DIR/pakka-src.tar.gz" .

echo "Wrote $HTTP_DIR/pakka-src.tar.gz"
