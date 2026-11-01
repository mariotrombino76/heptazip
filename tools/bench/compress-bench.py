#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Benchmark della compressione parallela dei folder .7z (-mpfc, docs/design/0003).

Uso:
    tools/bench/compress-bench.py [--bin 7zz] [-r N] [--threads 4,20]
                                  [--configs off,on] [--only REGEX]

Per ogni caso comprime i dati di bench-data/ in /dev/shm con ogni configurazione
e riporta il minimo delle ripetizioni (tempo reale), il carico medio della CPU
((user + sys) / reale, in "core") e la memoria massima (RSS).
Configurazioni: "base" (nessuna opzione, per binari senza -mpfc), "off" (-mpfc=off),
"on" (-mpfc=on). Con "off" e "on" verifica anche che gli archivi siano identici
byte per byte (requisito 4.1 del design), salvo i casi cifrati.

Casi:
  silesia-nonsolid      LZMA2 -mx5 -ms=off: 12 folder (uno per file)
  corpus-nonsolid       LZMA2 -mx5 -ms=off: 1285 file piccoli
  corpus-solid-8m       LZMA2 -mx5 -ms=8m
  corpus-solid-ext      LZMA2 -mx5 -ms=e
  silesia-lzma-nonsolid LZMA (al massimo 2 thread per folder)
  silesia-ppmd-16m      PPMd, blocchi solid da 16 MiB (PPMd usa 1 thread)
  silesia-zstd-16m      zstd -mx3, blocchi solid da 16 MiB
  silesia-bzip2-16m     BZip2, blocchi solid da 16 MiB
  silesia-deflate-16m   Deflate, blocchi solid da 16 MiB (1 thread)
  silesia-aes-nonsolid  LZMA2 -ms=off cifrato
  silesia-solid         solid di default (1 folder): non deve peggiorare
  corpus-default        solid di default: non deve peggiorare
"""

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = Path(os.environ.get("BENCH_DATA", REPO / "bench-data"))

CASES = [
    ("silesia-nonsolid", "silesia", ["-mx5", "-ms=off"]),
    ("corpus-nonsolid", "corpus", ["-mx5", "-ms=off"]),
    ("corpus-solid-8m", "corpus", ["-mx5", "-ms=8m"]),
    ("corpus-solid-ext", "corpus", ["-mx5", "-ms=e"]),
    ("silesia-lzma-nonsolid", "silesia", ["-m0=lzma", "-mx5", "-ms=off"]),
    ("silesia-ppmd-16m", "silesia", ["-m0=ppmd", "-mx5", "-ms=16m"]),
    ("silesia-zstd-16m", "silesia", ["-m0=zstd", "-mx3", "-ms=16m"]),
    ("silesia-bzip2-16m", "silesia", ["-m0=bzip2", "-mx5", "-ms=16m"]),
    ("silesia-deflate-16m", "silesia", ["-m0=deflate", "-mx5", "-ms=16m"]),
    ("silesia-aes-nonsolid", "silesia", ["-mx5", "-ms=off", "-psegreto"]),
    ("silesia-solid", "silesia", ["-mx5"]),
    ("corpus-default", "corpus", ["-mx5"]),
]

CONFIG_OPTS = {"base": [], "off": ["-mpfc=off"], "on": ["-mpfc=on"]}


def load_note(when: str) -> str:
    l1, l5, _ = os.getloadavg()
    warn = "  << ATTENZIONE: macchina carica" if when == "inizio" and l1 > 2 else ""
    return f"[{when}] load average 1m={l1:.2f} 5m={l5:.2f}{warn}"


def run(cmd, cwd):
    t0 = time.perf_counter()
    p = subprocess.run(["/usr/bin/time", "-f", "TIME=%U %S MAXRSS=%M", *cmd], cwd=cwd,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        sys.exit(f"comando fallito ({p.returncode}): {' '.join(map(str, cmd))}\n{p.stderr[-2000:]}")
    m = re.search(r"TIME=([\d.]+) ([\d.]+) MAXRSS=(\d+)", p.stderr)
    cpu = float(m.group(1)) + float(m.group(2))
    return dt, cpu, int(m.group(3)) / 1024


def num_blocks(sz, arc, extra):
    raw = subprocess.run([sz, "l", "-slt", *extra, str(arc)], capture_output=True, text=True).stdout
    m = re.search(r"^Blocks = (\d+)", raw, re.M)
    return int(m.group(1)) if m else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", type=Path, default=REPO / "build" / "7zz")
    ap.add_argument("-r", "--repeat", type=int, default=3)
    ap.add_argument("--threads", default=f"{os.cpu_count()}")
    ap.add_argument("--configs", default="off,on")
    ap.add_argument("--only", help="regex sui nomi dei casi")
    args = ap.parse_args()

    sz = str(args.bin.resolve())
    configs = args.configs.split(",")
    for c in configs:
        if c not in CONFIG_OPTS:
            sys.exit(f"configurazione sconosciuta: {c}")
    threads = [int(t) for t in args.threads.split(",")]
    if not (DATA / "silesia").is_dir() or not (DATA / "corpus").is_dir():
        sys.exit("servono bench-data/silesia e bench-data/corpus (tools/bench/make-corpus.sh --silesia)")

    work = Path(tempfile.mkdtemp(prefix="7z-cbench.", dir="/dev/shm"))
    try:
        print(f"bin: {sz}\nlavoro: {work}\n")
        print(load_note("inizio"))
        cols = "".join(f" {c + ' s':>8} {c + ' core':>9} {c + ' RSS':>8}" for c in configs)
        hdr = f"{'caso':<22} {'folder':>6} {'mmt':>4}{cols}" + (f" {'speedup':>8} {'identici':>8}" if "off" in configs and "on" in configs else "")
        print(hdr)
        print("-" * len(hdr))
        for name, src, opts in CASES:
            if args.only and not re.search(args.only, name):
                continue
            encrypted = any(o.startswith("-p") for o in opts)
            for mt in threads:
                res = {}
                arcs = {}
                for _ in range(args.repeat):
                    for c in configs:
                        arc = work / f"{name}-{c}.7z"
                        if arc.exists():
                            arc.unlink()
                        cmd = [sz, "a", "-t7z", *opts, f"-mmt={mt}", *CONFIG_OPTS[c], "-bso0", "-bsp0", str(arc), "."]
                        dt, cpu, rss = run(cmd, DATA / src)
                        best = res.get(c)
                        res[c] = (min(dt, best[0]), cpu if dt < best[0] else best[1], max(rss, best[2])) if best else (dt, cpu, rss)
                        arcs[c] = arc
                pw = [o for o in opts if o.startswith("-p")]
                blocks = num_blocks(sz, arcs[configs[0]], pw)
                line = f"{name:<22} {blocks:>6} {mt:>4}"
                for c in configs:
                    dt, cpu, rss = res[c]
                    line += f" {dt:>8.2f} {cpu / dt:>9.1f} {rss:>8.0f}"
                if "off" in configs and "on" in configs:
                    same = "-" if encrypted else ("si" if filecmp.cmp(arcs["off"], arcs["on"], shallow=False) else "NO")
                    line += f" {res['off'][0] / res['on'][0]:>7.2f}x {same:>8}"
                print(line, flush=True)
                for arc in arcs.values():
                    arc.unlink()
        print(load_note("fine"))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
