#!/bin/sh
# Build + smoke-test pakka inside the running NetBSD 3.0/sparc guest.
# Assumes run-vm.sh has the guest up with SSH reachable on host port
# $PAKKA_NBSD_SSH_PORT. Uses scp to ship a source tarball, then
# ssh+gmake to build and run the symlink-safe extract smoke from
# tests/pakka.bats.
#
# NetBSD 3.0/sparc has no usable pkgsrc binary archive (vendor only
# keeps 7.0+ in pkgsrc-archive), so the run-vm.sh post-install dance
# builds GNU make 3.81 from upstream source. That's documented in
# README.md and only needs to happen once.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
SSH_PORT=${PAKKA_NBSD_SSH_PORT:-10022}
ROOT_PASSWORD=${PAKKA_NBSD_ROOT_PASSWORD:-pakka}

if ! command -v sshpass >/dev/null 2>&1; then
    echo "Need sshpass to drive the guest non-interactively" >&2
    exit 1
fi

mkdir -p "$WORKDIR/http"
(cd "$REPO_ROOT" && tar --exclude .git --exclude build --exclude pakka \
    -czf "$WORKDIR/http/pakka-src.tar.gz" .)

# NetBSD 3.0's sshd offers only legacy algorithms; modern OpenSSH
# clients need explicit overrides to negotiate them.
SSH_OPTS="-p $SSH_PORT \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=$WORKDIR/known_hosts \
  -o HostKeyAlgorithms=+ssh-rsa \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1 \
  -o Ciphers=+aes128-cbc \
  -o MACs=+hmac-sha1"

# shellcheck disable=SC2086  # ssh options intentionally word-split
sshpass -p "$ROOT_PASSWORD" scp $SSH_OPTS \
    "$WORKDIR/http/pakka-src.tar.gz" root@127.0.0.1:/tmp/pakka-src.tar.gz

# shellcheck disable=SC2086
sshpass -p "$ROOT_PASSWORD" ssh $SSH_OPTS root@127.0.0.1 'sh -s' <<'EOF'
set -e
rm -rf /root/pakka-src
mkdir /root/pakka-src
cd /root/pakka-src
tar xzf /tmp/pakka-src.tar.gz
set +e
(
    set -e
    echo "== system =="
    uname -a
    echo "== toolchain =="
    gcc --version | head -1
    /usr/local/bin/make --version | head -1
    echo "== build =="
    /usr/local/bin/make
    echo "== smoke: banner =="
    if ! ./pakka -h 2>&1 | head -1 | grep -q '^Pakka '; then
        echo "banner FAIL" >&2; exit 1
    fi
    echo banner-OK
    echo "== smoke: symlink-safe extract =="
    rm -rf /tmp/scratch
    mkdir -p /tmp/scratch/out /tmp/scratch/outside
    {
        printf 'PACK\014\000\000\000\100\000\000\000models/x'
        dd if=/dev/zero bs=1 count=48 2>/dev/null
        printf '\114\000\000\000\000\000\000\000'
    } > /tmp/scratch/ok.pak
    ln -s /tmp/scratch/outside /tmp/scratch/out/models
    if ./pakka -xf /tmp/scratch/ok.pak -C /tmp/scratch/out 2>/dev/null; then
        echo "symlink-extract NOT REJECTED" >&2; exit 1
    fi
    [ ! -e /tmp/scratch/outside/x ] || { echo "escape happened" >&2; exit 1; }
    echo symlink-rejected-OK
    echo "== smoke: happy extract =="
    mkdir -p /tmp/scratch/out2
    ./pakka -xf /tmp/scratch/ok.pak -C /tmp/scratch/out2
    [ -f /tmp/scratch/out2/models/x ] || { echo "happy-extract failed" >&2; exit 1; }
    echo happy-OK
    echo "== file =="
    file ./pakka
) > /root/pakka-build.log 2>&1
rc=$?
set -e
cat /root/pakka-build.log
exit "$rc"
EOF
