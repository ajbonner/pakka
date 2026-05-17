#!/bin/sh
# Build + smoke-test pakka inside the running NetBSD 3.0/sparc guest.
# Assumes the guest is already up (run-vm.sh) and SSH is reachable on
# host port $PAKKA_NBSD_SSH_PORT. NetBSD's base ssh is OpenSSH; no
# legacy KEX/cipher flags are needed in 2003-era SSH on the host
# side, unlike the RH9 probe.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
SSH_PORT=${PAKKA_NBSD_SSH_PORT:-10022}
SSH_KNOWN_HOSTS=${PAKKA_NBSD_KNOWN_HOSTS:-"$WORKDIR/known_hosts"}
ROOT_PASSWORD=${PAKKA_NBSD_ROOT_PASSWORD:-pakka}

"$SCRIPT_DIR/prepare-source-tarball.sh"

if ! command -v sshpass >/dev/null 2>&1; then
    echo "Need sshpass to drive the guest non-interactively" >&2
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
    root@127.0.0.1 'sh -s' <<'EOF'
set -e
rm -rf /root/pakka-src /root/pakka-src.tar.gz /root/pakka-build.log
ftp -o /root/pakka-src.tar.gz http://10.0.2.2:8000/pakka-src.tar.gz
mkdir /root/pakka-src
cd /root/pakka-src
tar xzf /root/pakka-src.tar.gz
# Bracket the build + smoke in a subshell with explicit log capture so
# the script's real exit status survives, the same pattern the RH9
# kickstart uses. NetBSD 3.0's /bin/sh is the BSD ash fork; that
# subshell + rc=$? form is portable here.
set +e
(
    set -e
    echo "== system =="
    uname -a
    echo "== toolchain =="
    cc --version 2>&1 || gcc --version
    make --version 2>&1 || true
    echo "== build =="
    # NetBSD's /usr/bin/make is BSD make, not GNU. pakka's Makefile
    # uses GNU-only features (wildcard, substitution refs); install
    # gmake from pkgsrc and invoke that instead.
    gmake
    echo "== smoke =="
    if ! ./pakka -h 2>&1 | head -1 | grep -q '^Pakka '; then
        echo "smoke test failed: pakka banner not printed" >&2
        exit 1
    fi
    file ./pakka
    ldd ./pakka 2>&1 || true
) > /root/pakka-build.log 2>&1
rc=$?
set -e
cat /root/pakka-build.log
exit "$rc"
EOF
