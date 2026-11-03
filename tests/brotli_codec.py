#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Metodo brotli nei .7z (design 0014).

Per varie dimensioni (vuoto, 1 byte, intorno ai confini dei frame da livello * 1 MiB,
dati casuali) e livelli (0, 1, 3, 10):
- l'archivio si estrae e i dati sono identici;
- il flusso compresso è identico con -mmt1 e -mmt4 (frame indipendenti dai thread);
- la decompressione in parallelo (-mmt4) dà gli stessi dati;
- un byte cambiato nei dati compressi dà un errore (non dati sbagliati in silenzio).
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

M = 1 << 20


def run(cmd, check=True):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")))
    return r.returncode


def make_data():
    rnd = random.Random(14)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9))) for _ in range(2000)]
    text = bytearray()
    while len(text) < 4 * M:
        text += b" ".join(rnd.choice(words) for _ in range(12)) + b"\n"
    noise = bytes(rnd.getrandbits(8) for _ in range(M // 2))
    return {
        "empty": b"",
        "one": b"x",
        "small": bytes(text[:5000]),
        "1m": bytes(text[:M]),
        "1m+1": bytes(text[:M + 1]),
        "3m+5": bytes(text[:3 * M + 5]),
        "random": noise,
    }


def packed_stream(path):
    """The packed data of a .7z with one folder: after the 32 bytes of the signature
    header, up to the header (the offset of the header is at 12)."""
    with open(path, "rb") as f:
        b = f.read()
    hdr_off = int.from_bytes(b[12:20], "little")
    return b[32:32 + hdr_off]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    args = ap.parse_args()
    sz = os.path.abspath(args.sz)
    work = tempfile.mkdtemp(prefix="7z-brotli.")
    errors = 0
    try:
        for name, data in make_data().items():
            src = os.path.join(work, name)
            with open(src, "wb") as f:
                f.write(data)
            for level in (0, 1, 3, 10):
                if level == 10 and len(data) > M:
                    continue          # slow: q10 on small data only
                streams = {}
                for mt in (1, 4):
                    arc = os.path.join(work, "a%d.7z" % mt)
                    run([sz, "a", "-m0=brotli", "-mx%d" % level, "-mmt%d" % mt, "-bso0", "-bsp0", arc, src])
                    streams[mt] = packed_stream(arc)
                    out = os.path.join(work, "out")
                    run([sz, "x", "-mmt%d" % mt, "-bso0", "-bsp0", "-o" + out, arc])
                    with open(os.path.join(out, name), "rb") as f:
                        if f.read() != data:
                            errors += 1
                            print("ERRORE dati: %s -mx%d -mmt%d" % (name, level, mt))
                    shutil.rmtree(out)
                same = streams[1] == streams[4]
                errors += 0 if same else 1
                print("%-6s -mx%-2d %s" % (name, level, "ok, identico con 1 e 4 thread" if same else "DIVERSO tra 1 e 4 thread"))
                # a corrupted byte in the brotli data (after the frame header)
                if len(streams[1]) > 40:
                    arc = os.path.join(work, "a1.7z")
                    with open(arc, "r+b") as f:
                        f.seek(32 + 16 + (len(streams[1]) - 16) // 2)
                        b = f.read(1)
                        f.seek(-1, 1)
                        f.write(bytes([b[0] ^ 0x55]))
                    if run([sz, "t", "-bso0", "-bsp0", arc], check=False) == 0:
                        errors += 1
                        print("ERRORE: archivio corrotto accettato: %s -mx%d" % (name, level))
                for mt in (1, 4):
                    os.remove(os.path.join(work, "a%d.7z" % mt))
            os.remove(src)
    finally:
        shutil.rmtree(work)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
