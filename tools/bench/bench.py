#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Benchmark comparativo di uno o piu' binari 7zz sullo stesso corpus.

Uso:
    tools/bench/bench.py [-r RIPETIZIONI] [-m PROFILO ...] [--csv FILE] \\
        [--corpus DIR] NOME=PERCORSO_7ZZ [NOME=PERCORSO_7ZZ ...]

Esempio:
    tools/bench/bench.py upstream=CPP/7zip/Bundles/Alone2/_o/7zz cmake=build/7zz

Per ogni profilo e ogni binario misura:
  - tempo di compressione (archivio scritto in /dev/shm per escludere il disco)
  - dimensione dell'archivio e rapporto di compressione
  - tempo di decompressione tramite `7zz t` (decodifica + CRC, senza scrittura su disco)

Si riporta il MINIMO delle ripetizioni: e' la stima piu' stabile del costo reale
su una macchina con rumore di fondo. I binari vengono alternati a ogni
ripetizione per distribuire equamente eventuali effetti termici/turbo.
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Stessi profili di common.sh (escluso xz/aes, che qui non aggiungono informazioni
# utili sulle prestazioni del codec rispetto a 7z-lzma2).
PROFILES = {
    "7z-lzma2-mx1": "-t7z -m0=lzma2 -mx1 -mmt=4",
    "7z-lzma2-mx5": "-t7z -m0=lzma2 -mx5 -mmt=4",
    "7z-lzma2-mx9-st": "-t7z -m0=lzma2 -mx9 -mmt=1",
    "7z-lzma2-mx9-mt": "-t7z -m0=lzma2 -mx9 -mmt=8",
    "7z-lzma-mx5": "-t7z -m0=lzma -mx5 -mmt=2",
    "7z-ppmd": "-t7z -m0=ppmd -mx7",
    "7z-bzip2": "-t7z -m0=bzip2 -mx5 -mmt=4",
    "7z-bcj2": "-t7z -mf=BCJ2 -mx5 -mmt=4",
    "zip-deflate": "-tzip -mx5 -mmt=4",
}
DEFAULT_PROFILES = ["7z-lzma2-mx1", "7z-lzma2-mx5", "7z-lzma2-mx9-mt", "7z-lzma2-mx9-st", "zip-deflate"]


def corpus_size(path: Path) -> int:
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def run_timed(cmd, cwd=None) -> float:
    t0 = time.perf_counter()
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL)
    return time.perf_counter() - t0


def load_note(when: str) -> str:
    """Carico medio del sistema: con altri processi attivi (build, altre sessioni)
    le misure multi-thread sono meno affidabili."""
    l1, l5, _ = os.getloadavg()
    # a fine misura il load include il nostro stesso carico: avvisiamo solo all'inizio
    warn = "  << ATTENZIONE: macchina carica, misure MT poco affidabili" if when == "inizio" and l1 > 2 else ""
    return f"[{when}] load average 1m={l1:.2f} 5m={l5:.2f} su {os.cpu_count()} CPU{warn}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binaries", nargs="+", help="NOME=PERCORSO del binario 7zz")
    ap.add_argument("-r", "--repeat", type=int, default=3, help="ripetizioni per misura (default 3)")
    ap.add_argument("-m", "--profile", action="append", choices=sorted(PROFILES), help="profili da eseguire")
    ap.add_argument("--corpus", type=Path, default=Path(os.environ.get("BENCH_DATA", REPO_ROOT / "bench-data")) / "corpus")
    ap.add_argument("--csv", type=Path, help="salva i risultati anche in CSV")
    args = ap.parse_args()

    bins = []
    for spec in args.binaries:
        name, sep, path = spec.partition("=")
        if not sep:
            name, path = Path(spec).parent.name or spec, spec
        p = Path(path).resolve()
        if not os.access(p, os.X_OK):
            sys.exit(f"binario non eseguibile: {p}")
        bins.append((name, p))

    corpus = args.corpus.resolve()
    if not corpus.is_dir():
        sys.exit(f"corpus non trovato: {corpus} (esegui tools/bench/make-corpus.sh)")
    raw = corpus_size(corpus)
    profiles = args.profile or DEFAULT_PROFILES

    shm = Path("/dev/shm")
    tmp_root = shm if shm.is_dir() and os.access(shm, os.W_OK) else None
    work = Path(tempfile.mkdtemp(prefix="7zbench.", dir=tmp_root))

    print(f"corpus: {corpus} ({raw / 2**20:.1f} MiB), ripetizioni: {args.repeat}, tmp: {work}\n")
    rows = []
    print(load_note("inizio"))
    try:
        for prof in profiles:
            opts = PROFILES[prof].split()
            ext = "zip" if "-tzip" in opts else "7z"
            res = {name: {"c": [], "d": [], "size": 0} for name, _ in bins}
            for _ in range(args.repeat):
                for name, path in bins:
                    arc = work / f"{name}.{ext}"
                    arc.unlink(missing_ok=True)
                    res[name]["c"].append(run_timed([str(path), "a", *opts, "-bso0", "-bsp0", str(arc), "."], cwd=corpus))
                    res[name]["size"] = arc.stat().st_size
                    res[name]["d"].append(run_timed([str(path), "t", "-bso0", "-bsp0", str(arc)]))
                    arc.unlink()
            base_c = base_d = None
            for name, _ in bins:
                c, d, size = min(res[name]["c"]), min(res[name]["d"]), res[name]["size"]
                base_c = base_c or c
                base_d = base_d or d
                rows.append({
                    "profile": prof, "binary": name, "size": size,
                    "ratio": raw / size,
                    "comp_s": c, "comp_MiBs": raw / c / 2**20, "comp_speedup": base_c / c,
                    "dec_s": d, "dec_MiBs": raw / d / 2**20, "dec_speedup": base_d / d,
                })
            print_table([r for r in rows if r["profile"] == prof])
    finally:
        print(load_note("fine"))
        shutil.rmtree(work, ignore_errors=True)

    if args.csv:
        with args.csv.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
        print(f"CSV salvato in {args.csv}")
    return 0


def print_table(rows):
    hdr = f"{'profilo':<17} {'binario':<14} {'dimensione':>12} {'ratio':>6} " \
          f"{'comp MiB/s':>10} {'x':>6} {'dec MiB/s':>10} {'x':>6}"
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['profile']:<17} {r['binary']:<14} {r['size']:>12} {r['ratio']:>6.3f} "
              f"{r['comp_MiBs']:>10.1f} {r['comp_speedup']:>6.3f} {r['dec_MiBs']:>10.1f} {r['dec_speedup']:>6.3f}")
    print()


if __name__ == "__main__":
    sys.exit(main())
