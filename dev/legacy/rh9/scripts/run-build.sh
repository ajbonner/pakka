#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_RH9_WORKDIR:-/private/tmp/pakka-rh9}
SSH_PORT=${PAKKA_RH9_SSH_PORT:-10022}
SSH_KNOWN_HOSTS=${PAKKA_RH9_KNOWN_HOSTS:-"$WORKDIR/known_hosts"}
ROOT_PASSWORD=${PAKKA_RH9_ROOT_PASSWORD:-pakka}

"$SCRIPT_DIR/prepare-source-tarball.sh"

if ! command -v sshpass >/dev/null 2>&1; then
  echo "Need sshpass to run the guest build non-interactively" >&2
  exit 1
fi

python3 -m http.server 8000 --bind 0.0.0.0 --directory "$WORKDIR/http" &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null || true' EXIT INT TERM
sleep 1

sshpass -p "$ROOT_PASSWORD" ssh \
  -p "$SSH_PORT" \
  -o StrictHostKeyChecking=no \
  -o "UserKnownHostsFile=$SSH_KNOWN_HOSTS" \
  -o HostKeyAlgorithms=+ssh-rsa \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -o KexAlgorithms=+diffie-hellman-group1-sha1 \
  -o Ciphers=+aes128-cbc,3des-cbc \
  root@127.0.0.1 '/root/run-pakka-build.sh'
