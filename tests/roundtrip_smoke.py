#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Prova di andata e ritorno su ogni sistema (anche Windows, issue #8).

Crea una cartella di prova (testo, dati casuali, zeri, pesi float, sottocartelle, nome
con spazi), la comprime in ogni formato e metodo principale (anche in parallelo:
-mmt, -mpfc, -mpfd), la estrae in una cartella nuova e confronta i file byte per byte.
Senza dipendenze oltre a Python e 7zz.
"""

import argparse
import filecmp
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")))


def make_tree(root):
    rnd = random.Random(7)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9)))
             for _ in range(2000)]
    text = b" ".join(rnd.choice(words) for _ in range(400000))
    files = {
        "text.txt": text,
        "random.bin": bytes(rnd.getrandbits(8) for _ in range(300000)),
        "zeros.bin": bytes(1 << 20),
        "weights.f32": b"".join(struct.pack("<f", rnd.gauss(0, 0.02)) for _ in range(200000)),
        os.path.join("sub", "dir", "nome con spazi.txt"): text[:5000],
        os.path.join("sub", "empty.txt"): b"",
    }
    for name, data in files.items():
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    return sorted(files)


def same_tree(a, b, names):
    for n in names:
        if not filecmp.cmp(os.path.join(a, n), os.path.join(b, n), shallow=False):
            return "diverso: " + n
    return None


CASES = [
    ("7z", ["-m0=lzma2", "-mx5", "-mmt4"]),
    ("7z", ["-m0=lzma2", "-mx1", "-ms=1m", "-mpfc=on"]),
    ("7z", ["-m0=zstd", "-mx3"]),
    ("7z", ["-m0=brotli", "-mx5", "-mmt4"]),
    ("7z", ["-m0=bwt", "-mx5", "-mmt4"]),
    ("7z", ["-m0=FLOAT"]),
    ("7z", ["-m0=Deflate", "-mx5"]),
    ("7z", ["-m0=PPMd"]),
    ("7z", ["-m0=BZip2", "-mmt4"]),
    ("zip", ["-mx3"]),
    ("zip", ["-mm=Deflate64"]),
    ("tar", []),
    ("wim", []),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    args = ap.parse_args()
    sz = os.path.abspath(args.sz)
    work = tempfile.mkdtemp(prefix="7z-roundtrip.")
    errors = 0
    try:
        src = os.path.join(work, "src")
        os.makedirs(src)
        names = make_tree(src)
        cwd = os.getcwd()
        for i, (fmt, opts) in enumerate(CASES):
            arc = os.path.join(work, "a%d.%s" % (i, fmt))
            out = os.path.join(work, "out%d" % i)
            label = "%-4s %s" % (fmt, " ".join(opts))
            try:
                os.chdir(src)
                run([sz, "a", "-t" + fmt, "-bso0", "-bsp0"] + opts + [arc, "*"])
                os.chdir(cwd)
                run([sz, "t", "-bso0", "-bsp0", arc])
                extract = [sz, "x", "-bso0", "-bsp0", "-o" + out, arc]
                if fmt == "7z":
                    extract.insert(2, "-mpfd=on")
                run(extract)
                diff = same_tree(src, out, names)
            except RuntimeError as e:
                os.chdir(cwd)
                diff = str(e)
            print("%-34s %s" % (label, diff or "ok"))
            if diff:
                errors += 1
            if os.path.exists(arc):
                os.remove(arc)
            shutil.rmtree(out, ignore_errors=True)

        # gzip di un solo file, con il Deflate parallelo (design 0009, 0012)
        gz = os.path.join(work, "t.gz")
        out = os.path.join(work, "outgz")
        run([sz, "a", "-tgzip", "-mx3", "-mmt4", "-bso0", "-bsp0", gz, os.path.join(src, "text.txt")])
        run([sz, "x", "-bso0", "-bsp0", "-o" + out, gz])
        ok = filecmp.cmp(os.path.join(src, "text.txt"), os.path.join(out, "text.txt"), shallow=False)
        print("%-34s %s" % ("gzip -mx3 -mmt4", "ok" if ok else "diverso"))
        errors += 0 if ok else 1
    finally:
        os.chdir(os.path.dirname(work))
        shutil.rmtree(work, ignore_errors=True)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
