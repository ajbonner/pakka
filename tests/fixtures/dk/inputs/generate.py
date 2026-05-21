#!/usr/bin/env python3
"""Generate deterministic input bytes for the Daikatana fixture.

The four output files exercise the DK encoder and the add-path
extension policy:

    zeros.bmp     1024 NUL bytes                  -> zero-run tokens
    striped.tga    512 bytes of "abcd"-repeating  -> byte-RLE / back-ref
    random.bsp     256 LCG bytes                  -> STORED fallback
    notes.txt       short ASCII line              -> STORED by policy
                                                     (non-listed ext)

Output is deterministic across machines / Python versions / endianness
(no os.urandom, no time-of-day seed, no hash-randomized iteration).
Re-running overwrites the four files with byte-identical content.

After regenerating these inputs, rebuild user.pak with `dkpak -userpak`
over the staging layout described in ../README.md."""

import os
import sys


HERE = os.path.dirname(os.path.abspath(__file__))


def write_file(name, data):
    path = os.path.join(HERE, name)
    with open(path, "wb") as fp:
        fp.write(data)
    print(f"wrote {path}: {len(data)} bytes")


# 1024 NUL bytes — encoder uses zero-run tokens.
write_file("zeros.bmp", b"\x00" * 1024)

# 512 bytes of "abcd" repeated — after the literal prefix, back-refs
# at distance 4 (capped by the no-overlap rule).
write_file("striped.tga", b"abcd" * 128)

# 256 bytes from a fixed-seed LCG. High entropy, no profitable
# zero-run / byte-RLE / back-ref candidates; the encoder falls back to
# STORED because encoded output is larger than the source.
seed = 0xDEADBEEF
state = seed
out = bytearray()
for _ in range(256):
    state = (state * 1103515245 + 12345) & 0xFFFFFFFF
    out.append((state >> 16) & 0xFF)
write_file("random.bsp", bytes(out))

# Extension not on the compress list — stays STORED unconditionally.
write_file("notes.txt", b"Pakka DK fixture; see README.md.\n")
