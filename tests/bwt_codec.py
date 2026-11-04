#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Metodo BWT (libbsc) nei .7z (design 0016, issue #81).

Per varie dimensioni (vuoto, 1 byte, intorno ai confini dei blocchi da 2 MiB del
livello 1, dati casuali) e livelli (1, 3, 5, con il coder adattivo e un blocco dato):
- l'archivio si estrae e i dati sono identici;
- il flusso compresso e' identico con -mmt1 e -mmt4 (blocchi indipendenti dai thread);
- la decompressione in parallelo (-mmt4) da' gli stessi dati;
- un byte cambiato nei dati compressi da' un errore (non dati sbagliati in silenzio).
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

M = 1 << 20
HEADER = 28   # the libbsc block header


def run(cmd, check=True):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")))
    return r.returncode


def make_data():
    rnd = random.Random(16)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9))) for _ in range(2000)]
    text = bytearray()
    while len(text) < 5 * M:
        text += b" ".join(rnd.choice(words) for _ in range(12)) + b"\n"
    noise = bytes(rnd.getrandbits(8) for _ in range(M // 2))
    return {
        "empty": b"",
        "one": b"x",
        "small": bytes(text[:5000]),
        "2m": bytes(text[:2 * M]),
        "2m+1": bytes(text[:2 * M + 1]),
        "5m-3": bytes(text[:5 * M - 3]),
        "random": noise,
        "zeros": bytes(3 * M),
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
    work = tempfile.mkdtemp(prefix="7z-bwt.")
    errors = 0
    # (label, method switches)
    variants = [
        ("-mx1", ["-m0=bwt", "-mx1"]),
        ("-mx3", ["-m0=bwt", "-mx3"]),
        ("-mx5", ["-m0=bwt", "-mx5"]),
        ("a=2 c=1m", ["-m0=bwt:a=2:c=1m", "-mx5"]),
    ]
    try:
        for name, data in make_data().items():
            src = os.path.join(work, name)
            with open(src, "wb") as f:
                f.write(data)
            for label, switches in variants:
                streams = {}
                for mt in (1, 4):
                    arc = os.path.join(work, "a%d.7z" % mt)
                    run([sz, "a"] + switches + ["-mmt%d" % mt, "-bso0", "-bsp0", arc, src])
                    streams[mt] = packed_stream(arc)
                    out = os.path.join(work, "out")
                    run([sz, "x", "-mmt%d" % mt, "-bso0", "-bsp0", "-o" + out, arc])
                    with open(os.path.join(out, name), "rb") as f:
                        if f.read() != data:
                            errors += 1
                            print("ERRORE dati: %s %s -mmt%d" % (name, label, mt))
                    shutil.rmtree(out)
                same = streams[1] == streams[4]
                errors += 0 if same else 1
                print("%-6s %-9s %s" % (name, label, "ok, identico con 1 e 4 thread" if same else "DIVERSO tra 1 e 4 thread"))
                # a corrupted byte in the data of the first block (after its header)
                if len(streams[1]) > HEADER + 8:
                    arc = os.path.join(work, "a1.7z")
                    with open(arc, "r+b") as f:
                        f.seek(32 + HEADER + (len(streams[1]) - HEADER) // 2)
                        b = f.read(1)
                        f.seek(-1, 1)
                        f.write(bytes([b[0] ^ 0x55]))
                    if run([sz, "t", "-bso0", "-bsp0", arc], check=False) == 0:
                        errors += 1
                        print("ERRORE: archivio corrotto accettato: %s %s" % (name, label))
                for mt in (1, 4):
                    os.remove(os.path.join(work, "a%d.7z" % mt))
            os.remove(src)
    finally:
        shutil.rmtree(work)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
