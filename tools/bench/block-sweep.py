#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Sweep della dimensione dei blocchi LZMA2: rapporto vs decompressione multi-thread.

Uso:
    tools/bench/block-sweep.py [--bin 7zz] [--input FILE] [-r N] [-x LIVELLO]
                               [--blocks solid,auto,64m,32m,...] [--threads 1,4,20]

Per ogni dimensione di blocco comprime INPUT (un singolo file, es. silesia.tar)
in un archivio .7z con LZMA2 e misura:
  - dimensione e rapporto di compressione
  - tempo di compressione (encoder con tutti i thread) e memoria massima (cRSS)
  - tempo di decompressione (`7zz t -mmt=N`) e memoria massima (RSS) per ogni N

"solid" = un solo blocco (encoder a 1 thread di blocco, come -mmt=1)
"auto"  = scelta di default dell'encoder (4 x dizionario, 1..256 MiB)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def run(cmd):
    """Esegue cmd sotto /usr/bin/time; ritorna (secondi, max RSS in MiB)."""
    t0 = time.perf_counter()
    p = subprocess.run(["/usr/bin/time", "-f", "MAXRSS=%M", *cmd],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        sys.exit(f"comando fallito ({p.returncode}): {' '.join(cmd)}\n{p.stderr}")
    rss = int(re.search(r"MAXRSS=(\d+)", p.stderr).group(1)) / 1024
    return dt, rss


def load_note(when: str) -> str:
    """Carico medio del sistema: con altri processi attivi (build, altre sessioni)
    le misure multi-thread sono meno affidabili."""
    l1, l5, _ = os.getloadavg()
    # a fine misura il load include il nostro stesso carico: avvisiamo solo all'inizio
    warn = "  << ATTENZIONE: macchina carica, misure MT poco affidabili" if when == "inizio" and l1 > 2 else ""
    return f"[{when}] load average 1m={l1:.2f} 5m={l5:.2f} su {os.cpu_count()} CPU{warn}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", type=Path, default=REPO_ROOT / "build-asm" / "7zz")
    ap.add_argument("--input", type=Path, default=REPO_ROOT / "bench-data" / "silesia.tar")
    ap.add_argument("-r", "--repeat", type=int, default=3)
    ap.add_argument("-x", "--level", default="9", help="livello -mx (default 9)")
    ap.add_argument("--blocks", default="solid,auto,64m,32m,16m,8m,4m")
    ap.add_argument("--threads", default=f"1,4,{os.cpu_count()}")
    ap.add_argument("--extra", default="", help="opzioni aggiuntive per 'a' (es. -m0=lzma2:d=16m)")
    args = ap.parse_args()

    binp, inp = args.bin.resolve(), args.input.resolve()
    if not inp.is_file():
        sys.exit(f"input non trovato: {inp}")
    raw = inp.stat().st_size
    threads = [int(t) for t in args.threads.split(",")]
    ncpu = os.cpu_count()

    shm = Path("/dev/shm")
    work = Path(tempfile.mkdtemp(prefix="7zsweep.", dir=shm if shm.is_dir() else None))
    arc = work / "a.7z"

    print(f"bin: {binp}\ninput: {inp} ({raw / 2**20:.1f} MiB), -mx{args.level}, ripetizioni: {args.repeat}\n")
    hdr = f"{'blocco':>7} {'dimensione':>11} {'ratio':>6} {'Δsize':>7} {'comp MiB/s':>10} {'cRSS MiB':>8}"
    for t in threads:
        hdr += f" {'dec T' + str(t) + ' MiB/s':>15} {'RSS MiB':>8}"
    print(hdr)
    print("-" * len(hdr))

    base_size = None
    print(load_note("inizio"))
    try:
        for blk in args.blocks.split(","):
            opts = ["-t7z", f"-mx{args.level}", "-m0=lzma2"]
            if blk == "solid":
                opts.append("-mmt=1")
            else:
                opts.append(f"-mmt={ncpu}")
                if blk != "auto":
                    opts[-2] = f"-m0=lzma2:c={blk}"
            opts += args.extra.split()

            arc.unlink(missing_ok=True)
            ct, crss = run([str(binp), "a", *opts, "-bso0", "-bsp0", str(arc), str(inp)])
            size = arc.stat().st_size
            base_size = base_size or size
            line = (f"{blk:>7} {size:>11} {raw / size:>6.3f} {(size / base_size - 1) * 100:>+6.2f}%"
                    f" {raw / ct / 2**20:>10.1f} {crss:>8.0f}")
            for t in threads:
                best, rss = None, 0.0
                for _ in range(args.repeat):
                    dt, r = run([str(binp), "t", f"-mmt={t}", "-bso0", "-bsp0", str(arc)])
                    best = dt if best is None else min(best, dt)
                    rss = max(rss, r)
                line += f" {raw / best / 2**20:>15.1f} {rss:>8.0f}"
            print(line, flush=True)
    finally:
        print(load_note("fine"))
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
