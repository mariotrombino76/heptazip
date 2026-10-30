#!/usr/bin/env python3
"""Benchmark dell'estrazione parallela dei folder .7z (-mpfd, docs/design/0001).

Uso:
    tools/bench/extract-bench.py [--bin 7zz] [-r N] [--threads 1,4,20] [--keep]

Crea in /dev/shm una serie di archivi rappresentativi e misura, per
-mpfd=off e -mpfd=on con vari -mmt:
  - t : decompressione + CRC (nessuna scrittura)
  - x : estrazione completa su /dev/shm (scrittura dei file)
riportando il minimo delle ripetizioni, lo speedup rispetto a pfd=off con lo
stesso -mmt e la memoria massima (RSS).

Archivi (dai dati di bench-data/):
  silesia-nonsolid  12 folder (uno per file), caso tipico favorevole
  corpus-solid-ext  -ms=e: un folder per estensione
  corpus-solid-8m   -ms=8m: blocchi solid da 8 MiB
  corpus-default    archivio di default (1-2 folder): non deve peggiorare
  silesia-solid     1 folder: non deve peggiorare
  mixed-bigblock    un folder grande LZMA2 a blocchi (silesia.tar, c=16m) + piccoli:
                    verifica il rischio "folder grande decompresso da un solo thread"
  aes-nonsolid      silesia non-solid cifrato (mixer AES+LZMA2 nei worker)
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

REPO = Path(__file__).resolve().parents[2]
DATA = Path(os.environ.get("BENCH_DATA", REPO / "bench-data"))


def load_note(when: str) -> str:
    l1, l5, _ = os.getloadavg()
    warn = "  << ATTENZIONE: macchina carica" if when == "inizio" and l1 > 2 else ""
    return f"[{when}] load average 1m={l1:.2f} 5m={l5:.2f}{warn}"


def run(cmd, cwd=None):
    t0 = time.perf_counter()
    p = subprocess.run(["/usr/bin/time", "-f", "MAXRSS=%M", *cmd], cwd=cwd,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        sys.exit(f"comando fallito ({p.returncode}): {' '.join(map(str, cmd))}\n{p.stderr[-2000:]}")
    return dt, int(re.search(r"MAXRSS=(\d+)", p.stderr).group(1)) / 1024


def make_archives(sz: str, work: Path):
    silesia = DATA / "silesia"
    corpus = DATA / "corpus"
    if not silesia.is_dir() or not corpus.is_dir():
        sys.exit("servono bench-data/silesia e bench-data/corpus (tools/bench/make-corpus.sh --silesia)")
    arcs = []

    def mk(name, src, opts, pw=None):
        arc = work / f"{name}.7z"
        cmd = [sz, "a", "-t7z", *opts, "-bso0", "-bsp0", str(arc)]
        if pw:
            cmd.append(f"-p{pw}")
        if src.is_dir():
            subprocess.run(cmd + ["."], cwd=src, check=True, stdout=subprocess.DEVNULL)
        else:
            subprocess.run(cmd + [str(src)], check=True, stdout=subprocess.DEVNULL)
        raw = subprocess.run([sz, "l", "-slt", *( [f"-p{pw}"] if pw else []), str(arc)],
                             capture_output=True, text=True).stdout
        blocks = re.search(r"^Blocks = (\d+)", raw, re.M)
        arcs.append((name, arc, pw, int(blocks.group(1)) if blocks else 0))

    mk("silesia-nonsolid", silesia, ["-mx5", "-ms=off"])
    mk("corpus-solid-ext", corpus, ["-mx5", "-ms=e"])
    mk("corpus-solid-8m", corpus, ["-mx5", "-ms=8m"])
    mk("corpus-default", corpus, ["-mx5"])
    mk("silesia-solid", silesia, ["-mx5"])
    # un folder grande a blocchi LZMA2 + alcuni folder piccoli
    mixed = work / "mixed-src"
    mixed.mkdir()
    shutil.copy(DATA / "silesia.tar", mixed / "big.tar")
    for f in sorted((silesia).iterdir())[:4]:
        shutil.copy(f, mixed / (f.name + ".dat"))
    mk("mixed-bigblock", mixed, ["-mx5", "-ms=e", "-m0=lzma2:c=16m"])
    shutil.rmtree(mixed)
    mk("aes-nonsolid", silesia, ["-mx5", "-ms=off"], pw="segreto")
    return arcs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", type=Path, default=REPO / "build-asm" / "7zz")
    ap.add_argument("-r", "--repeat", type=int, default=3)
    ap.add_argument("--threads", default=f"1,4,{os.cpu_count()}")
    ap.add_argument("--only", help="regex sui nomi degli archivi")
    ap.add_argument("--extra", default="", help="opzioni aggiuntive per t/x (es. -mmtf=off)")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    sz = str(args.bin.resolve())
    work = Path(tempfile.mkdtemp(prefix="7z-xbench.", dir="/dev/shm"))
    threads = [int(t) for t in args.threads.split(",")]
    extra = args.extra.split()
    try:
        print(f"bin: {sz}\nlavoro: {work}\n")
        arcs = make_archives(sz, work)
        print(load_note("inizio"))
        hdr = f"{'archivio':<18} {'folder':>6} {'op':>2} {'mmt':>4} {'off s':>7} {'on s':>7} {'speedup':>8} {'RSS off':>8} {'RSS on':>8}"
        print(hdr)
        print("-" * len(hdr))
        for name, arc, pw, blocks in arcs:
            if args.only and not re.search(args.only, name):
                continue
            pwa = [f"-p{pw}"] if pw else []
            for op in ("t", "x"):
                for mt in threads:
                    res = {}
                    for _ in range(args.repeat):
                        for pfd in ("off", "on"):
                            out = work / "out"
                            shutil.rmtree(out, ignore_errors=True)
                            cmd = [sz, op, f"-mmt={mt}", f"-mpfd={pfd}", *extra, *pwa, "-bso0", "-bsp0", str(arc)]
                            if op == "x":
                                cmd += [f"-o{out}", "-y"]
                            dt, rss = run(cmd)
                            best = res.get(pfd)
                            res[pfd] = (min(dt, best[0]), max(rss, best[1])) if best else (dt, rss)
                    off, on = res["off"], res["on"]
                    print(f"{name:<18} {blocks:>6} {op:>2} {mt:>4} {off[0]:>7.3f} {on[0]:>7.3f} "
                          f"{off[0] / on[0]:>7.2f}x {off[1]:>8.0f} {on[1]:>8.0f}", flush=True)
        print(load_note("fine"))
    finally:
        if args.keep:
            print(f"directory mantenuta: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
