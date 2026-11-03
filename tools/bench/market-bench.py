#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Benchmark di mercato: il fork contro gli altri strumenti di compressione.

Uso:
    tools/bench/market-bench.py [--fork build/7zz] [--upstream 7zz-26.05] [-r 2]
                                [--inputs silesia,corpus] [--only REGEX]

Per ogni strumento disponibile e per ogni profilo comprime un input a stream singolo
(tar), poi lo decomprime in /dev/null, e riporta:
  - rapporto (dimensione compressa / originale);
  - velocita' di compressione e di decompressione (MB/s, tempo reale, minimo delle
    ripetizioni);
  - secondi di CPU (user + sys) di compressione e decompressione: misura dell'energia
    consumata (piu' core per lo stesso lavoro = piu' energia);
  - memoria massima (RSS).
Ogni archivio viene verificato una volta (decompressione confrontata con l'originale).
Tutti gli strumenti usano tutti i core se lo supportano (-T0, -p N, -mmt...).
Gli strumenti assenti vengono saltati (vedi la riga "assenti" all'inizio).
"""

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = Path(os.environ.get("BENCH_DATA", REPO / "bench-data"))
NCPU = os.cpu_count() or 1


def seven(name, binary, opts):
    """Profilo per 7zz: archivio .7z con un solo file; estrazione su stdout."""
    return {
        "tool": name, "profile": " ".join(opts), "need": binary,
        "comp": lambda src, dst: ([binary, "a", "-t7z", *opts, "-bso0", "-bsp0", str(dst), str(src)], None),
        "decomp": lambda arc: ([binary, "e", "-so", "-bso0", "-bse0", "-bsp0", str(arc)], None),
        "ext": ".7z",
    }


def seven_fmt(name, binary, fmt, opts, ext):
    """Profilo per 7zz con un formato a stream (gzip, bzip2): stessi file di gzip/bzip2."""
    return {
        "tool": name, "profile": " ".join([f"-t{fmt}", *opts]), "need": binary,
        "comp": lambda src, dst: ([binary, "a", f"-t{fmt}", *opts, "-bso0", "-bsp0", str(dst), str(src)], None),
        "decomp": lambda arc: ([binary, "e", "-so", "-bso0", "-bse0", "-bsp0", str(arc)], None),
        "ext": ext,
    }


def stream(name, exe, copts, dopts, ext, stdin=True):
    """Profilo per uno strumento a stream: input -> stdout."""
    return {
        "tool": name, "profile": " ".join(copts), "need": exe,
        "comp": lambda src, dst: ([exe, *copts, "-c", str(src)], dst),
        "decomp": lambda arc: ([exe, *dopts, "-c", str(arc)], None),
        "ext": ext,
    }


def profiles(fork, upstream):
    p = []
    for opts in (["-m0=lzma2", "-mx1"], ["-m0=lzma2", "-mx5"], ["-m0=lzma2", "-mx9"],
                 ["-m0=zstd", "-mx3"], ["-m0=zstd", "-mx19"],
                 ["-m0=brotli", "-mx5"], ["-m0=brotli", "-mx11"],   # design 0014
                 ["-m0=FLOAT"]):   # pesi dei modelli AI (design 0008); su altri dati ~ copia
        p.append(seven("7zz-modern", fork, opts))
    # Deflate e BZip2 di 7-Zip nei loro formati, contro gzip/pigz/libdeflate e bzip2/lbzip2
    # (-tbzip2 -mx5 e' il predefinito, blocchi da 900k come bzip2 -9; -mx9 fa 7 passate)
    for fmt, opts, ext in (("gzip", ["-mx1"], ".gz"), ("gzip", ["-mx3"], ".gz"),
                           ("gzip", ["-mx4"], ".gz"), ("gzip", ["-mx5"], ".gz"),
                           ("gzip", ["-mx9"], ".gz"),
                           ("bzip2", ["-mx5"], ".bz2"), ("bzip2", ["-mx9"], ".bz2")):
        p.append(seven_fmt("7zz-modern", fork, fmt, opts, ext))
    if upstream:
        for opts in (["-m0=lzma2", "-mx1"], ["-m0=lzma2", "-mx5"], ["-m0=lzma2", "-mx9"]):
            p.append(seven("7zz-26.05", upstream, opts))
        for fmt, opts, ext in (("gzip", ["-mx1"], ".gz"), ("gzip", ["-mx5"], ".gz"),
                               ("bzip2", ["-mx5"], ".bz2")):
            p.append(seven_fmt("7zz-26.05", upstream, fmt, opts, ext))
    p += [
        stream("zstd", "zstd", ["-3", "-T0", "-q"], ["-d", "-q"], ".zst"),
        stream("zstd", "zstd", ["-19", "-T0", "-q"], ["-d", "-q"], ".zst"),
        stream("zstd", "zstd", ["--ultra", "-22", "-T0", "-q"], ["-d", "-q"], ".zst"),
        stream("xz", "xz", ["-1", "-T0"], ["-d", "-T0"], ".xz"),
        stream("xz", "xz", ["-6", "-T0"], ["-d", "-T0"], ".xz"),
        stream("xz", "xz", ["-9", "-T0"], ["-d", "-T0"], ".xz"),
        stream("brotli", "brotli", ["-q", "5"], ["-d"], ".br"),
        stream("brotli", "brotli", ["-q", "11", "-w", "24"], ["-d"], ".br"),
        stream("lz4", "lz4", ["-1", "-q"], ["-d", "-q"], ".lz4"),
        stream("pigz", "pigz", ["-6", "-p", str(NCPU)], ["-d"], ".gz"),
        stream("pigz", "pigz", ["-9", "-p", str(NCPU)], ["-d"], ".gz"),
        stream("gzip", "gzip", ["-6"], ["-d"], ".gz"),
        stream("libdeflate", "libdeflate-gzip", ["-1"], ["-d"], ".gz"),
        stream("libdeflate", "libdeflate-gzip", ["-6"], ["-d"], ".gz"),
        stream("libdeflate", "libdeflate-gzip", ["-9"], ["-d"], ".gz"),
        stream("libdeflate", "libdeflate-gzip", ["-12"], ["-d"], ".gz"),
        stream("bzip2", "bzip2", ["-9"], ["-d"], ".bz2"),
        stream("lbzip2", "lbzip2", ["-9", f"-n{NCPU}"], ["-d", f"-n{NCPU}"], ".bz2"),
        stream("bzip3", "bzip3", ["-j", str(NCPU)], ["-d", "-j", str(NCPU)], ".bz3"),
        stream("plzip", "plzip", ["-6", f"-n{NCPU}"], ["-d", f"-n{NCPU}"], ".lz"),
    ]
    return p


def available(exe):
    return Path(exe).is_file() if "/" in exe else shutil.which(exe) is not None


def timed(cmd, out_path=None):
    """Esegue (cmd) misurando tempo reale, CPU (user + sys) e RSS massimo."""
    out = open(out_path, "wb") if out_path else subprocess.DEVNULL
    try:
        t0 = time.perf_counter()
        p = subprocess.run(["/usr/bin/time", "-f", "TIME=%U %S %M", *cmd],
                           stdout=out, stderr=subprocess.PIPE, text=False)
        dt = time.perf_counter() - t0
    finally:
        if out_path:
            out.close()
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}: exit {p.returncode}: {err[-400:]}")
    m = re.findall(r"TIME=([\d.]+) ([\d.]+) (\d+)", err)[-1]
    return dt, float(m[0]) + float(m[1]), int(m[2]) / 1024


def make_inputs(work, names):
    inputs = []
    for name in names:
        if name == "silesia":
            src = DATA / "silesia.tar"
        elif name == "corpus":
            src = work / "corpus.tar"
            with tarfile.open(src, "w") as t:
                t.add(DATA / "corpus", arcname="corpus")
        else:
            src = Path(name)
        if not src.is_file():
            sys.exit(f"input mancante: {src}")
        inputs.append((name, src))
    return inputs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fork", type=Path, default=REPO / "build" / "7zz")
    ap.add_argument("--upstream", type=Path, help="7zz di 7-Zip originale (riferimento)")
    ap.add_argument("-r", "--repeat", type=int, default=2)
    ap.add_argument("--inputs", default="silesia,corpus")
    ap.add_argument("--only", help="regex su 'strumento profilo'")
    args = ap.parse_args()

    profs = profiles(str(args.fork.resolve()), str(args.upstream.resolve()) if args.upstream else None)
    missing = sorted({p["need"] for p in profs if not available(p["need"])})
    profs = [p for p in profs if available(p["need"])
             and (not args.only or re.search(args.only, f"{p['tool']} {p['profile']}"))]

    work = Path(tempfile.mkdtemp(prefix="7z-market.", dir="/dev/shm"))
    try:
        inputs = make_inputs(work, args.inputs.split(","))
        l1 = os.getloadavg()[0]
        print(f"CPU logiche: {NCPU}   load 1m: {l1:.2f}   ripetizioni: {args.repeat}")
        print(f"assenti: {', '.join(missing) if missing else '-'}\n")
        hdr = (f"{'input':8} {'strumento':11} {'profilo':24} {'rapporto':>8} {'comp MB/s':>9} "
               f"{'dec MB/s':>9} {'comp CPU s':>10} {'dec CPU s':>9} {'comp RSS':>8} {'dec RSS':>8}")
        print(hdr)
        print("-" * len(hdr))
        for iname, src in inputs:
            size = src.stat().st_size
            for p in profs:
                arc = work / f"a{p['ext']}"
                best_c = best_d = None
                try:
                    for _ in range(args.repeat):
                        if arc.exists():
                            arc.unlink()
                        cmd, out = p["comp"](src, arc)
                        r = timed(cmd, out)
                        best_c = r if best_c is None or r[0] < best_c[0] else best_c
                    for _ in range(args.repeat):
                        cmd, _ = p["decomp"](arc)
                        r = timed(cmd)
                        best_d = r if best_d is None or r[0] < best_d[0] else best_d
                    # verifica: decompressione confrontata con l'originale
                    check = work / "check.out"
                    cmd, _ = p["decomp"](arc)
                    with open(check, "wb") as fo:
                        subprocess.run(cmd, stdout=fo, stderr=subprocess.DEVNULL, check=True)
                    ok = filecmp.cmp(check, src, shallow=False)
                    check.unlink()
                except (RuntimeError, subprocess.CalledProcessError) as e:
                    print(f"{iname:8} {p['tool']:11} {p['profile']:24} ERRORE: {e}")
                    continue
                csize = arc.stat().st_size
                arc.unlink()
                print(f"{iname:8} {p['tool']:11} {p['profile']:24} {100 * csize / size:7.2f}% "
                      f"{size / best_c[0] / 1e6:9.1f} {size / best_d[0] / 1e6:9.1f} "
                      f"{best_c[1]:10.1f} {best_d[1]:9.2f} {best_c[2]:8.0f} {best_d[2]:8.0f}"
                      f"{'' if ok else '  << VERIFICA FALLITA'}", flush=True)
        print(f"\nload 1m alla fine: {os.getloadavg()[0]:.2f}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
