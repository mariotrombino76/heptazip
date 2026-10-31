#!/usr/bin/env python3
"""File che cambiano dimensione tra la scansione e la lettura (docs/design/0003, par. 5.3).

Per ogni caso di tests/update_growing.cpp (grow-small, grow-huge, shrink) e per piu'
suddivisioni, crea l'archivio con -mpfc=off e -mpfc=on e verifica che:
  - gli archivi siano identici byte per byte;
  - l'archivio sia integro (7zz t) e contenga la dimensione REALE del file cambiato.
"""

import argparse
import filecmp
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

KIB = 1 << 10
MIB = 1 << 20
# dimensione reale di f2.txt per caso (le altre: 200 KiB + i * 10 KiB)
ACTUAL_F2 = {"grow-small": 700 * KIB, "grow-huge": 140 * MIB, "shrink": 10 * KIB}
SPLITS = [["s=off"], ["s=4m"]]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    ap.add_argument("--tool", required=True, help="eseguibile update_growing")
    args = ap.parse_args()

    work = Path(tempfile.mkdtemp(prefix="7z-growing.", dir="/dev/shm"))
    failures = []
    ncases = 0
    try:
        for case, actual in ACTUAL_F2.items():
            for split in SPLITS:
                if case == "grow-huge" and split != ["s=off"]:
                    continue  # 140 MiB: un solo caso basta
                ncases += 1
                name = f"{case}/{' '.join(split)}"
                arcs = {}
                for cfg in ("off", "on"):
                    arc = work / f"{case}-{split[0]}-{cfg}.7z"
                    p = subprocess.run([args.tool, str(arc), case, "x=1", "mt=4", *split, f"pfc={cfg}"],
                                       capture_output=True, text=True)
                    if p.returncode != 0:
                        failures.append(f"{name} [{cfg}]: update fallito: {p.stdout} {p.stderr[-500:]}")
                        break
                    arcs[cfg] = arc
                if len(arcs) != 2:
                    continue
                if not filecmp.cmp(arcs["off"], arcs["on"], shallow=False):
                    failures.append(f"{name}: archivi diversi")
                t = subprocess.run([args.sz, "t", "-bso0", "-bsp0", str(arcs["on"])], capture_output=True, text=True)
                if t.returncode != 0:
                    failures.append(f"{name}: 7zz t fallito: {t.stderr[-500:]}")
                slt = subprocess.run([args.sz, "l", "-slt", str(arcs["on"])], capture_output=True, text=True).stdout
                m = re.search(r"^Path = f2\.txt\nSize = (\d+)", slt, re.M)
                if not m or int(m.group(1)) != actual:
                    failures.append(f"{name}: dimensione di f2.txt {m.group(1) if m else '?'} invece di {actual}")
                for arc in arcs.values():
                    arc.unlink()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for f in failures:
        print("FAIL", f)
    print(f"{ncases - len(failures)}/{ncases} casi OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
