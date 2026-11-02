#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Semi per fuzz_codec (design 0007): stream validi nel formato d'ingresso dell'harness.

    python3 tools/fuzz/make-seeds-codec.py <directory_corpus> [zstd]

Crea <directory_corpus>/<metodo>/... per LZMA, LZMA2, Deflate, BZip2, ZSTD (se c'e' il
programma zstd) e per i filtri (BCJ, ARM64, Delta, ...). Gli altri metodi (PPMD, Rar*,
BCJ2) partono da zero. Il formato dell'input e' descritto in tests/fuzz/fuzz_codec.cpp.
"""

import bz2
import lzma
import os
import struct
import subprocess
import sys
import zlib

FLAG_OUT_SIZE = 0x20
FLAG_FINISH = 0x40
FLAG_IN_SIZE = 0x80


def sample_data():
    text = "".join("line %d: hello hello world %d\n" % (i, i * i) for i in range(600)).encode()
    with open("/usr/bin/true", "rb") as f:
        elf = f.read(24000)
    return [text[:200], text, elf, os.urandom(2000), b"\0" * 5000]


def seed(props, payload, out_size=None, finish=True):
    flags = len(props)
    assert flags < 32
    head = b""
    if out_size is not None:
        flags |= FLAG_OUT_SIZE
        head = struct.pack("<I", out_size)
    if finish:
        flags |= FLAG_FINISH
    return bytes([flags | FLAG_IN_SIZE]) + props + head + payload


def lzma_props(lc=3, lp=0, pb=2, dict_size=1 << 16):
    return bytes([(pb * 5 + lp) * 9 + lc]) + struct.pack("<I", dict_size)


def lzma2_dict_byte(dict_size):
    for b in range(41):
        if ((2 | (b & 1)) << (b // 2 + 11)) >= dict_size:
            return b
    return 40


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    out = os.path.realpath(sys.argv[1])
    zstd = sys.argv[2] if len(sys.argv) == 3 else None
    seeds = {}

    def add(method, data):
        seeds.setdefault(method, []).append(data)

    for i, d in enumerate(sample_data()):
        dict_size = 1 << 16
        raw1 = lzma.compress(d, format=lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
        add("LZMA", seed(lzma_props(dict_size=dict_size), raw1, len(d)))
        add("LZMA", seed(lzma_props(dict_size=dict_size), raw1, None, finish=False))
        raw2 = lzma.compress(d, format=lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}])
        add("LZMA2", seed(bytes([lzma2_dict_byte(dict_size)]), raw2, len(d)))
        add("Deflate", seed(b"", zlib.compress(d, 9, -15)[:], len(d)))
        c = zlib.compressobj(1, zlib.DEFLATED, -15)
        add("Deflate", seed(b"", c.compress(d) + c.flush(), None))
        add("Deflate64", seed(b"", zlib.compress(d, 6, -15), len(d)))
        add("BZip2", seed(b"", bz2.compress(d, 9), len(d)))
        if zstd:
            r = subprocess.run([zstd, "-q", "-c", "-19"], input=d, stdout=subprocess.PIPE, check=True)
            add("ZSTD", seed(b"", r.stdout, len(d)))
        for f in ("BCJ", "ARM64", "ARM", "ARMT", "PPC", "SPARC", "IA64", "RISCV", "Swap2", "Swap4"):
            add(f, seed(b"", d[:4096], None))
        add("Delta", seed(bytes([3]), d[:4096], None))

    for method, items in sorted(seeds.items()):
        d = os.path.join(out, method)
        os.makedirs(d, exist_ok=True)
        for i, data in enumerate(items):
            with open(os.path.join(d, "seed%02d" % i), "wb") as f:
                f.write(data)
        print("%-10s %d semi" % (method, len(items)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
