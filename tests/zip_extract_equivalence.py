#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Equivalenza dell'estrazione zip in parallelo (design 0017, issue #86).

Crea zip con vari metodi e casi (Deflate ai livelli, Store, Deflate64, BZip2, LZMA,
PPMd, zstd, ZipCrypto, AES, molti file piccoli, zip da stdin con descrittori), ne deriva
copie corrotte (byte invertiti, tagli) e confronta la traccia delle callback di
estrazione (extract_trace -z) fra:

    A = -mmt=1            (percorso sequenziale)
    B = -mmt=<thread>     (elementi decodificati in parallelo)
    C = -mmt=<thread> -mpfd=off

Le tracce devono essere IDENTICHE (stessi stream, stessi risultati per elemento, stesso
esito), anche con selezioni parziali, modalita' test e password sbagliata.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

PASSWORD = "segreto"

# (nome, opzioni di creazione, cifrato)
PROFILES = [
    ("deflate5",    ["-mx5"], False),
    ("deflate1",    ["-mx1"], False),
    ("deflate9",    ["-mx9"], False),
    ("store",       ["-mx0"], False),
    ("deflate64",   ["-mm=Deflate64"], False),
    ("bzip2",       ["-mm=BZip2"], False),
    ("lzma",        ["-mm=LZMA"], False),
    ("ppmd",        ["-mm=PPMd"], False),
    ("zstd",        ["-mm=zstd"], False),
    ("zipcrypto",   ["-mx5", "-p" + PASSWORD, "-mem=ZipCrypto"], True),
    ("aes256",      ["-mx5", "-p" + PASSWORD, "-mem=AES256"], True),
]


def run(cmd, check=True, stdin=None):
    r = subprocess.run(cmd, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stderr.decode(errors="replace")))
    return r


def make_data(root):
    rnd = random.Random(86)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9))) for _ in range(3000)]

    def text(n):
        out = bytearray()
        while len(out) < n:
            out += b" ".join(rnd.choice(words) for _ in range(10)) + b"\n"
        return bytes(out[:n])

    os.makedirs(os.path.join(root, "dir", "sub"))
    files = {
        "a.txt": text(300000),
        "b.txt": text(1200000),
        "dir/c.bin": bytes(rnd.getrandbits(8) for _ in range(200000)),   # incomprimibile
        "dir/sub/d.txt": text(70000),
        "dir/sub/e.txt": text(5),
        "empty": b"",
        "zeros.bin": bytes(400000),
        "f.txt": text(2500000),
    }
    for name, data in files.items():
        with open(os.path.join(root, name), "wb") as f:
            f.write(data)
    # molti file piccoli
    small = os.path.join(root, "small")
    os.makedirs(small)
    for i in range(150):
        with open(os.path.join(small, "s%03d.txt" % i), "wb") as f:
            f.write(text(rnd.randint(0, 3000)))


def corrupt_copies(src, dst_dir):
    with open(src, "rb") as f:
        data = bytearray(f.read())
    rnd = random.Random(len(data))
    out = []
    for k, point in enumerate((0.1, 0.4, 0.75)):
        d = bytearray(data)
        pos = int(len(d) * point)
        for j in range(8):
            d[pos + j] ^= 0xFF
        p = os.path.join(dst_dir, "flip%d.zip" % k)
        with open(p, "wb") as f:
            f.write(d)
        out.append(p)
    for k, point in enumerate((0.3, 0.9)):
        p = os.path.join(dst_dir, "cut%d.zip" % k)
        with open(p, "wb") as f:
            f.write(data[:int(len(data) * point)])
        out.append(p)
    # un byte casuale cambiato in un punto casuale (ripetibile)
    d = bytearray(data)
    pos = rnd.randrange(len(d))
    d[pos] ^= 0x55
    p = os.path.join(dst_dir, "rnd.zip")
    with open(p, "wb") as f:
        f.write(d)
    out.append(p)
    return out


def trace(tool, arc, config, extra):
    r = run([tool, "-z", arc] + config + extra, check=False)
    return r.stdout.decode(errors="replace") + ("\nEXIT %d" % r.returncode)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--7zz", dest="sz", required=True)
    ap.add_argument("--trace", required=True, help="extract_trace")
    ap.add_argument("--threads", type=int, default=8)
    args = ap.parse_args()
    sz = os.path.abspath(args.sz)
    tool = os.path.abspath(args.trace)
    work = tempfile.mkdtemp(prefix="7z-zipmt.", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    errors = 0
    cases = 0
    try:
        data = os.path.join(work, "data")
        os.makedirs(data)
        make_data(data)
        archives = []
        for name, opts, enc in PROFILES:
            arc = os.path.join(work, name + ".zip")
            r = run([sz, "a", "-y", "-tzip", "-bso0", "-bsp0"] + opts + [arc, data], check=False)
            if r.returncode != 0:
                print("%-12s non creato (metodo non disponibile?): salto" % name)
                continue
            archives.append((name, arc, enc))
        # zip da stdin: descrittori di dati
        with open(os.path.join(data, "f.txt"), "rb") as f:
            arc = os.path.join(work, "stdin.zip")
            run([sz, "a", "-y", "-tzip", "-si", "-bso0", "-bsp0", "-mx3", arc], stdin=f)
            archives.append(("stdin", arc, False))
        configs_b = [["-mmt=%d" % args.threads], ["-mmt=%d" % args.threads, "-mpfd=off"]]
        for name, arc, enc in archives:
            bad_dir = os.path.join(work, name + "-bad")
            os.makedirs(bad_dir)
            variants = [arc] + corrupt_copies(arc, bad_dir)
            for v in variants:
                pw = ["-p" + PASSWORD] if enc else []
                extras = [[], ["-t"]]
                if name != "stdin":
                    extras.append(["-i0,3,7"])   # selezione parziale
                # il percorso sequenziale con piu' thread (-mpfd=off) decodifica BZip2 con
                # piu' thread: su dati corrotti la quantita' di uscita prima dell'errore puo'
                # differire da -mmt=1 (comportamento di 7-Zip): si confronta solo sugli
                # archivi integri. Il percorso parallelo decodifica a un thread per
                # elemento e deve coincidere con -mmt=1 sempre.
                cfgs = configs_b if v == arc else configs_b[:1]
                for extra in extras:
                    ref = trace(tool, v, ["-mmt=1"], pw + extra)
                    for cb in cfgs:
                        got = trace(tool, v, cb, pw + extra)
                        cases += 1
                        if got != ref:
                            errors += 1
                            print("DIVERSO: %s %s %s %s" % (name, os.path.basename(v), " ".join(cb), " ".join(extra)))
                            ra, rb = ref.splitlines(), got.splitlines()
                            for i in range(min(len(ra), len(rb))):
                                if ra[i] != rb[i]:
                                    print("  A: %s\n  B: %s" % (ra[i], rb[i]))
                                    break
                if enc:
                    ref = trace(tool, v, ["-mmt=1"], ["-pwrong"])
                    for cb in cfgs:
                        got = trace(tool, v, cb, ["-pwrong"])
                        cases += 1
                        if got != ref:
                            errors += 1
                            print("DIVERSO (password sbagliata): %s %s %s" % (name, os.path.basename(v), " ".join(cb)))
            print("%-12s ok" % name if errors == 0 else "%-12s (vedi sopra)" % name)
    finally:
        shutil.rmtree(work)
    print("casi: %d, errori: %d" % (cases, errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
