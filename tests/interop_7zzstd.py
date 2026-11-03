#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Interoperabilità con 7-Zip-zstd (design 0002 per zstd, 0014 per brotli).

    interop_7zzstd.py --ours build/7zz --theirs <7zz di 7-Zip-zstd>

Per brotli e zstd, a più livelli: un archivio creato da Heptazip si estrae con
7-Zip-zstd, e uno creato da 7-Zip-zstd si estrae con Heptazip; i dati devono essere
identici. Non è in CTest (serve il binario di 7-Zip-zstd): lo esegue la CI.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")[-2000:]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True)
    ap.add_argument("--theirs", required=True)
    args = ap.parse_args()
    tools = {"Heptazip": os.path.abspath(args.ours), "7-Zip-zstd": os.path.abspath(args.theirs)}
    rnd = random.Random(2)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9))) for _ in range(3000)]
    text = b" ".join(rnd.choice(words) for _ in range(500000))
    files = {"text.txt": text, "random.bin": bytes(rnd.getrandbits(8) for _ in range(300000)),
             "zeros.bin": bytes(1 << 20)}
    work = tempfile.mkdtemp(prefix="7z-interop.")
    errors = 0
    try:
        src = os.path.join(work, "src")
        os.makedirs(src)
        for n, d in files.items():
            with open(os.path.join(src, n), "wb") as f:
                f.write(d)
        cases = [("brotli", l) for l in (1, 5, 11)] + [("zstd", l) for l in (3, 19)]
        for method, level in cases:
            for maker, reader in (("Heptazip", "7-Zip-zstd"), ("7-Zip-zstd", "Heptazip")):
                arc = os.path.join(work, "a.7z")
                out = os.path.join(work, "out")
                label = "%-6s -mx%-2d %s -> %s" % (method, level, maker, reader)
                try:
                    cwd = os.getcwd()
                    os.chdir(src)
                    try:
                        run([tools[maker], "a", "-m0=" + method, "-mx%d" % level, "-bso0", "-bsp0", arc] + sorted(files))
                    finally:
                        os.chdir(cwd)
                    run([tools[reader], "x", "-bso0", "-bsp0", "-o" + out, arc])
                    bad = [n for n, d in files.items() if open(os.path.join(out, n), "rb").read() != d]
                    msg = "ok" if not bad else "DIVERSI: " + ", ".join(bad)
                except RuntimeError as e:
                    bad = True
                    msg = "ERRORE " + str(e).splitlines()[0]
                print("%-40s %s" % (label, msg), flush=True)
                errors += 1 if bad else 0
                if os.path.exists(arc):
                    os.remove(arc)
                shutil.rmtree(out, ignore_errors=True)
    finally:
        shutil.rmtree(work)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
