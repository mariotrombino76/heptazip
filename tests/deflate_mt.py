#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Deflate a blocchi in parallelo (docs/design/0009) e livelli veloci (0010).

Per ogni dimensione (intorno ai multipli del blocco di 1 MiB, vuoto, dati casuali e zeri)
e per ogni livello crea .gz e .zip con -mmt1, -mmt3 e -mmt8 e verifica che:

- i file siano IDENTICI byte per byte per ogni numero di thread;
- un decoder indipendente (zlib di Python) li decomprima negli stessi dati, e con
  il CRC giusto (gzip.decompress e zipfile.testzip lo controllano).

Deflate64 si verifica con 7zz stesso (zlib non lo legge).

Poi il decoder (design 0011): .gz di zlib con ogni strategia, decompressi da 7zz.
"""

import argparse
import gzip
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib

M = 1 << 20


def make_data():
    rnd = random.Random(9)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9)))
             for _ in range(3000)]
    text = bytearray()
    while len(text) < 5 * M:
        line = b" ".join(rnd.choice(words) for _ in range(rnd.randint(3, 14)))
        text += line + b"%d\n" % rnd.randint(0, 99999)
    noise = bytes(rnd.getrandbits(8) for _ in range(2 * M))
    return {
        "empty": b"",
        "small": bytes(text[:3000]),
        "1m-1": bytes(text[:M - 1]),
        "1m": bytes(text[:M]),
        "1m+1": bytes(text[:M + 1]),
        "3m": bytes(text[:3 * M]),
        "4m+77": bytes(text[:4 * M + 77]),
        "random-2m": noise,
        "zeros-3m": bytes(3 * M),
        "mixed": bytes(text[:M + 5000]) + noise[:M] + bytes(M // 2) + bytes(text[2 * M:3 * M]),
    }


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")))


def decode_zlib(sz, work, datas):
    """Decoder (design 0011): .gz made by zlib with every strategy, small windows and
    flushes (empty stored blocks), decoded by 7zz; also cut at 2/3 (error, and the same
    bytes as the data before it)."""
    errors = 0
    arc = os.path.join(work, "z.gz")
    for name, data in datas.items():
        for level, strategy, wbits, flush in ((1, zlib.Z_DEFAULT_STRATEGY, 15, 0),
                                               (6, zlib.Z_DEFAULT_STRATEGY, 15, 0),
                                               (9, zlib.Z_DEFAULT_STRATEGY, 9, 0),
                                               (6, zlib.Z_FILTERED, 15, 0),
                                               (6, zlib.Z_HUFFMAN_ONLY, 15, 0),
                                               (6, zlib.Z_RLE, 15, 0),
                                               (6, zlib.Z_FIXED, 15, 0),
                                               (6, zlib.Z_DEFAULT_STRATEGY, 15, 100003)):
            c = zlib.compressobj(level, zlib.DEFLATED, 16 + wbits, 8, strategy)
            parts = []
            step = flush or max(1, len(data))
            for i in range(0, len(data), step):
                parts.append(c.compress(data[i:i + step]))
                if flush:
                    parts.append(c.flush(zlib.Z_SYNC_FLUSH))
            parts.append(c.flush())
            packed = b"".join(parts)
            for cut in (False, True):
                if cut and len(packed) < 100:
                    continue
                with open(arc, "wb") as f:
                    f.write(packed[:len(packed) * 2 // 3] if cut else packed)
                r = subprocess.run([sz, "e", "-so", arc], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                # cut: error; as 7-Zip 26.05 the decoder can write some bytes decoded
                # after the end of the input before it detects it (up to a few matches)
                ok = (r.returncode != 0 and data.startswith(r.stdout[:max(0, len(r.stdout) - 1024)])) \
                     if cut else (r.returncode == 0 and r.stdout == data)
                if not ok:
                    errors += 1
                    print("ERRORE decodifica: %s zlib -%d strategia %d wbits %d flush %d%s" %
                          (name, level, strategy, wbits, flush, " tagliato" if cut else ""))
    os.remove(arc)
    print("decodifica zlib: errori", errors)
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    args = ap.parse_args()
    work = tempfile.mkdtemp(prefix="7z-deflate-mt.", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    errors = 0
    try:
        datas = make_data()
        for name, data in datas.items():
            src = os.path.join(work, name)
            with open(src, "wb") as f:
                f.write(data)
            # -mx1..-mx4: lazy matching (design 0010), also with more passes (cache of
            # the matches); -mx5 and -mx9: optimal parser
            for fmt, method, levels in (("gzip", None, ("-mx1", "-mx2", "-mx3", "-mx4", "-mx5", "-mx9",
                                                        "-mx3 -mpass=3")),
                                        ("zip", "Deflate", ("-mx3", "-mx5")),
                                        ("zip", "Deflate64", ("-mx3", "-mx5"))):
                for level in levels:
                    digests = {}
                    for mt in ("-mmt1", "-mmt3", "-mmt8"):
                        arc = os.path.join(work, "a" + mt + (".gz" if fmt == "gzip" else ".zip"))
                        cmd = [args.sz, "a", "-t" + fmt] + level.split() + [mt, "-bso0", "-bsp0"]
                        if method:
                            cmd.append("-mm=" + method)
                        run(cmd + [arc, src])
                        with open(arc, "rb") as f:
                            packed = f.read()
                        digests[mt] = hashlib.sha256(packed).hexdigest()
                        if fmt == "gzip":
                            ok = gzip.decompress(packed) == data
                        elif method == "Deflate":
                            with zipfile.ZipFile(arc) as z:
                                ok = z.testzip() is None and z.read(name) == data
                        else:
                            out = os.path.join(work, "out")
                            run([args.sz, "e", "-y", "-bso0", "-bsp0", "-o" + out, arc])
                            with open(os.path.join(out, name), "rb") as f:
                                ok = f.read() == data
                            shutil.rmtree(out)
                        os.remove(arc)
                        if not ok:
                            errors += 1
                            print("ERRORE dati: %s %s %s %s" % (name, fmt, method or "", level + " " + mt))
                    same = len(set(digests.values())) == 1
                    if not same:
                        errors += 1
                    print("%-10s %-5s %-9s %s %s" % (name, fmt, method or "Deflate", level,
                                                     "identici" if same else "DIVERSI " + str(digests)))
            os.remove(src)
        errors += decode_zlib(args.sz, work, datas)
    finally:
        shutil.rmtree(work)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
