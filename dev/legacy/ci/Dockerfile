# Legacy build floor — Debian 3.1 Sarge (June 2005) userland, plus GNU
# make 3.79.1 installed from source over the apt-shipped 3.80. Gives
# CI a single image that exercises the full RH9-era toolchain floor:
#   glibc 2.3.2, gcc 3.3, GNU make 3.79.1.
#
# Sarge predates sha256sum in coreutils, so the GNU make tarball is
# fetched + verified on a modern base in stage 1 and then copied into
# the legacy stage for the compile step. Build artifacts (the verified
# tarball) are architecture-independent C source.

FROM debian:bookworm-slim AS verifier
ARG MAKE_URL=http://ftp.gnu.org/gnu/make/make-3.79.1.tar.gz
ARG MAKE_SHA256=656e5a0f85fff8d12d4d244143f296c5b7c54b5cd090f0205ef08e26db760aa6
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates wget \
    && rm -rf /var/lib/apt/lists/* \
    && cd /tmp \
    && wget -q "$MAKE_URL" -O make-3.79.1.tar.gz \
    && echo "$MAKE_SHA256  make-3.79.1.tar.gz" | sha256sum -c -

# Sarge is 32-bit i386 only; this digest serves both amd64 (native ia32
# compat) and i386 platforms. arm64 hosts run it under QEMU emulation,
# which works but is slow.
FROM --platform=linux/386 debian/eol:sarge@sha256:1eb2cc32d515e6292ef3c0d37006cdba1004c7c3d4633946fe9fe9e6bc6bb4ad

# debian/eol images ship sources.list pointing at the live debian.org
# mirrors that no longer carry Sarge; force archive.debian.org and
# disable Valid-Until (Sarge's Release files long expired). Sarge's
# apt is pre-2005 — no --no-install-recommends, no signed-by; the
# --force-yes flag covers the unauthenticated-archive case.
RUN echo 'deb http://archive.debian.org/debian sarge main' > /etc/apt/sources.list \
    && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99no-check-valid-until \
    && apt-get update \
    && apt-get install -y --force-yes \
        gcc make libc6-dev binutils file \
    && rm -rf /var/lib/apt/lists/*

COPY --from=verifier /tmp/make-3.79.1.tar.gz /tmp/make-3.79.1.tar.gz

RUN cd /tmp \
    && tar xzf make-3.79.1.tar.gz \
    && cd make-3.79.1 \
    && ./configure --prefix=/usr/local --quiet \
    && make --quiet \
    && make install \
    && cd / \
    && rm -rf /tmp/make-3.79.1 /tmp/make-3.79.1.tar.gz \
    && /usr/local/bin/make --version | grep -q '3.79.1' \
    || (echo 'expected GNU make 3.79.1 on PATH'; exit 1)

WORKDIR /src
ENTRYPOINT ["/src/dev/legacy/ci/build.sh"]
