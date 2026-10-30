#!/usr/bin/env python3
"""Suite di equivalenza dell'estrazione .7z (docs/design/0001, par. 6.1).

Genera dati di test deterministici, crea una matrice di archivi .7z (solid,
non-solid, molti folder, BCJ2, PPMd, BZip2, Deflate, copia, AES, LZMA2 a blocchi),
ne deriva copie corrotte e confronta la traccia delle callback di estrazione
prodotta da due configurazioni:

    A = <trace-a> <archivio> <config-a> <caso>
    B = <trace-b> <archivio> <config-b> <caso>

Le tracce devono essere IDENTICHE. Esempi:
  # stessa build, configurazioni diverse (es. -mmt=1 vs tutti i core, o pfd=off/on)
  extract_equivalence.py --7zz build/7zz --trace-a build/tests/extract_trace \\
      --config-a=-mmt=1 --config-b=-mmt=on
  # due build diverse, stessa configurazione (es. prima/dopo un refactoring)
  extract_equivalence.py --7zz build/7zz --trace-a vecchio/extract_trace \\
      --trace-b build/tests/extract_trace

Per evitare una suite "vacua", sugli archivi integri senza errori iniettati la
traccia A deve anche risultare tutta OK (ogni RESULT = 0, EXTRACT hr=0).
"""

import argparse
import difflib
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import struct
import tempfile
import zlib
from pathlib import Path

PASSWORD = "segreto"

# (nome, opzioni di creazione, cifrato)
PROFILES = [
    ("solid-lzma2",        ["-mx5", "-mmt=4"], False),
    ("solid-lzma2-blocks", ["-mx5", "-m0=lzma2:c=1m", "-mmt=4"], False),
    ("nonsolid",           ["-mx3", "-ms=off"], False),
    ("solid-ext",          ["-mx3", "-ms=e"], False),
    ("small-solid",        ["-mx3", "-ms=1m"], False),
    ("bcj2",               ["-mx5", "-mf=BCJ2", "-ms=2m"], False),
    ("ppmd",               ["-m0=ppmd", "-ms=off"], False),
    ("bzip2",              ["-m0=bzip2", "-ms=2m", "-mmt=2"], False),
    ("deflate",            ["-m0=deflate", "-ms=off"], False),
    ("copy",               ["-mx0"], False),
    ("aes-headers",        ["-mx3", "-ms=1m", "-mhe=on", f"-p{PASSWORD}"], True),
    ("aes-nonsolid",       ["-mx3", "-ms=off", f"-p{PASSWORD}"], True),
]

# archivi da cui derivare copie corrotte
CORRUPT_FROM = ["solid-lzma2", "solid-lzma2-blocks", "nonsolid", "small-solid", "bcj2", "aes-nonsolid"]
CORRUPT_POINTS = [0.25, 0.50, 0.75]
# archivi su cui iniettare errori nelle callback (annullamento, scrittura fallita)
FAULT_FROM = ["solid-lzma2", "nonsolid", "small-solid", "bcj2"]


# ---------------------------------------------------------------------------
# Dati di test
# ---------------------------------------------------------------------------

def make_test_data(root: Path) -> None:
    """Albero di file deterministico (~10 MiB) con casi particolari."""
    r = random.Random(20261029)
    words = ["".join(r.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(2, 10)))
             for _ in range(2000)]

    def text(n: int) -> bytes:
        out, size = [], 0
        while size < n:
            line = " ".join(r.choice(words) for _ in range(r.randint(5, 20))) + "\n"
            out.append(line)
            size += len(line)
        return "".join(out).encode()[:n]

    (root / "docs" / "sub").mkdir(parents=True)
    (root / "empty-dir").mkdir()
    (root / "bin").mkdir()
    for i in range(40):
        (root / "docs" / f"t{i:02}.txt").write_bytes(text(r.randint(1, 200) * 1024))
    for i in range(3):
        (root / "docs" / "sub" / f"vuoto{i}.txt").write_bytes(b"")
    (root / "docs" / "sub" / "ripetuto.txt").write_bytes(text(64 * 1024) * 32)  # 2 MiB
    (root / "random.bin").write_bytes(r.randbytes(1 << 20))
    (root / "zeros.bin").write_bytes(bytes(3 << 20))
    # "pseudo-x86": byte casuali con molte istruzioni CALL/JMP (E8/E9) per BCJ/BCJ2
    prog = bytearray(r.randbytes(600 * 1024))
    for pos in range(0, len(prog) - 5, 37):
        prog[pos] = 0xE8 if (pos // 37) % 2 else 0xE9
    (root / "bin" / "prog.exe").write_bytes(bytes(prog))
    # contenuti duplicati
    dup = text(100 * 1024)
    (root / "docs" / "dup-a.txt").write_bytes(dup)
    (root / "docs" / "sub" / "dup-b.txt").write_bytes(dup)


SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
                     "WARNING: ThreadSanitizer", "runtime error:")


def sanitizer_report(stderr: str):
    """Estratto del report di un sanitizer presente in stderr, o None."""
    for marker in SANITIZER_MARKERS:
        pos = stderr.find(marker)
        if pos >= 0:
            return "\n    ".join(stderr[pos:].splitlines()[:25])
    return None


def make_many_files(root: Path) -> None:
    """3000 file piccoli + file vuoti: molti piu' passi della finestra di
    pianificazione dell'estrazione parallela (docs/design/0001)."""
    r = random.Random(7)
    for d in range(30):
        sub = root / f"d{d:02}"
        sub.mkdir(parents=True)
        for k in range(100):
            n = 0 if k % 17 == 0 else r.randint(1, 4096)
            (sub / f"f{k:03}.txt").write_bytes(bytes(r.choice(b"abcdefgh \n") for _ in range(n)))


def create_archive(sz: str, data: Path, arc: Path, opts) -> None:
    p = subprocess.run([sz, "a", "-t7z", *opts, "-bso0", "-bsp0", str(arc), "."],
                       cwd=data, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0 or sanitizer_report(p.stderr):
        raise RuntimeError(f"creazione di {arc.name} fallita (rc={p.returncode}):\n{p.stderr[-4000:]}")


def corrupt_copy(src: Path, dst: Path, point: float) -> None:
    """Inverte 16 byte a 'point' (frazione della dimensione) dell'archivio."""
    data = bytearray(src.read_bytes())
    pos = max(32, int(len(data) * point))  # dopo la signature header (32 byte)
    for k in range(pos, min(pos + 16, len(data))):
        data[k] ^= 0x55
    dst.write_bytes(bytes(data))


def unsupported_method_copy(src: Path, dst: Path) -> bool:
    """Copia un archivio PPMd con intestazione NON compressa (-mhc=off) cambiando
    l'ID del metodo del PRIMO folder in uno sconosciuto (03 04 01 -> 03 04 7F) e
    ricalcolando i CRC delle intestazioni: l'archivio si apre, ma quel folder
    produce E_NOTIMPL -> kUnsupportedMethod (ramo di ProcessFolderDecodeResult).

    Formato (DOC/7zFormat.txt, SignatureHeader): byte 8..11 StartHeaderCRC,
    12..19 NextHeaderOffset, 20..27 NextHeaderSize, 28..31 NextHeaderCRC."""
    data = bytearray(src.read_bytes())
    next_off, next_size = struct.unpack_from("<QQ", data, 12)
    start = 32 + next_off
    header = data[start:start + next_size]
    pos = header.find(b"\x23\x03\x04\x01")  # flag 0x23: ID di 3 byte + proprieta'
    if pos < 0:
        return False
    data[start + pos + 3] = 0x7F
    struct.pack_into("<I", data, 28, zlib.crc32(data[start:start + next_size]))
    struct.pack_into("<I", data, 8, zlib.crc32(data[12:32]))
    dst.write_bytes(bytes(data))
    return True


# ---------------------------------------------------------------------------
# Esecuzione dei casi
# ---------------------------------------------------------------------------

def run_trace(tool: str, arc: Path, config, case_args):
    """Ritorna (exit code, traccia su stdout, eventuale report di un sanitizer)."""
    p = subprocess.run([tool, str(arc), *config, *case_args],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, sanitizer_report(p.stderr)


def num_items(tool: str, arc: Path, pw_args) -> int:
    _, out, _ = run_trace(tool, arc, [], [*pw_args, "-t", "-i0"])
    m = re.search(r"^ITEMS (\d+)$", out, re.M)
    return int(m.group(1)) if m else 0


def selections(n: int):
    """Selezioni di elementi: tutti, primi, sparsi, singolo centrale, ultimo."""
    sel = [("all", [])]
    if n > 0:
        sel += [
            ("first3", ["-i" + ",".join(str(i) for i in range(min(3, n)))]),
            ("every3", ["-i" + ",".join(str(i) for i in range(1, n, 3))]),
            ("middle", [f"-i{n // 2}"]),
            ("last",   [f"-i{n - 1}"]),
        ]
    return [s for s in sel if s[1] != ["-i"]]


def trace_is_clean(out: str) -> bool:
    if "EXTRACT hr=0x00000000" not in out:
        return False
    for line in out.splitlines():
        if line.startswith("RESULT "):
            parts = line.split()
            if parts[2] != "0":
                return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--7zz", dest="sz", required=True, help="7zz usato per CREARE gli archivi")
    ap.add_argument("--trace-a", required=True)
    ap.add_argument("--trace-b", help="default: uguale a --trace-a")
    ap.add_argument("--config-a", default="", help="opzioni per A, es. '-mmt=1'")
    ap.add_argument("--config-b", default="", help="opzioni per B, es. '-mmt=on'")
    ap.add_argument("--keep", action="store_true", help="non cancellare la directory di lavoro")
    args = ap.parse_args()

    trace_a = str(Path(args.trace_a).resolve())
    trace_b = str(Path(args.trace_b).resolve()) if args.trace_b else trace_a
    cfg_a, cfg_b = shlex.split(args.config_a), shlex.split(args.config_b)
    sz = str(Path(args.sz).resolve())

    shm = Path("/dev/shm")
    work = Path(tempfile.mkdtemp(prefix="7z-equiv.", dir=shm if shm.is_dir() and os.access(shm, os.W_OK) else None))
    print(f"A: {trace_a} {' '.join(cfg_a)}\nB: {trace_b} {' '.join(cfg_b)}\nlavoro: {work}")

    cases = []  # (descrizione, archivio, argomenti, deve_essere_ok)
    try:
        data = work / "data"
        data.mkdir()
        make_test_data(data)
        arcs = work / "arcs"
        arcs.mkdir()

        for name, opts, enc in PROFILES:
            arc = arcs / f"{name}.7z"
            create_archive(sz, data, arc, opts)
            pw = [f"-p{PASSWORD}"] if enc else []
            n = num_items(trace_a, arc, pw)
            if n == 0:
                print(f"ERRORE: impossibile leggere {arc.name}")
                return 1
            for sel_name, sel_args in selections(n):
                for mode in ([], ["-t"]):
                    cases.append((f"{name}/{sel_name}{'/test' if mode else ''}", arc, [*pw, *sel_args, *mode], True))
            if name in FAULT_FROM:
                cases.append((f"{name}/abort@3", arc, ["--abort-at-file=3"], False))
                cases.append((f"{name}/failwrite@2", arc, ["--fail-write-at-file=2"], False))
            if enc:
                cases.append((f"{name}/wrong-password", arc, ["-psbagliata"], False))

        # molti passi: oltre la finestra di pianificazione dell'estrazione parallela
        many = work / "many"
        many.mkdir()
        make_many_files(many)
        arc = arcs / "many-files.7z"
        create_archive(sz, many, arc, ["-mx1", "-ms=off"])
        cases.append(("many-files/all", arc, [], True))
        cases.append(("many-files/all/test", arc, ["-t"], True))
        cases.append(("many-files/every7", arc, ["-i" + ",".join(str(k) for k in range(3, 3000, 7))], True))
        cases.append(("many-files/abort@1500", arc, ["--abort-at-file=1500"], False))

        # metodo non supportato in un solo folder (E_NOTIMPL)
        src = arcs / "ppmd-plainheader.7z"
        create_archive(sz, data, src, ["-m0=ppmd", "-ms=off", "-mhc=off"])
        dst = arcs / "unsupported-method.7z"
        if not unsupported_method_copy(src, dst):
            print("ERRORE: ID di PPMd non trovato nell'intestazione")
            return 1
        for mode in ([], ["-t"]):
            cases.append((f"unsupported-method{'/test' if mode else ''}", dst, mode, False))
        cases.append(("unsupported-method/every3", dst, ["-i" + ",".join(str(i) for i in range(0, 40, 3))], False))

        for name in CORRUPT_FROM:
            src = arcs / f"{name}.7z"
            pw = [f"-p{PASSWORD}"] if dict((p[0], p[2]) for p in PROFILES)[name] else []
            for pt in CORRUPT_POINTS:
                dst = arcs / f"{name}.corrupt{int(pt * 100)}.7z"
                corrupt_copy(src, dst, pt)
                for mode in ([], ["-t"]):
                    cases.append((f"{dst.stem}{'/test' if mode else ''}", dst, [*pw, *mode], False))
            # archivio troncato
            dst = arcs / f"{name}.truncated.7z"
            dst.write_bytes(src.read_bytes()[: int(src.stat().st_size * 0.6)])
            cases.append((dst.stem, dst, pw, False))

        failures = 0
        for desc, arc, case_args, must_be_clean in cases:
            rc_a, out_a, san_a = run_trace(trace_a, arc, cfg_a, case_args)
            rc_b, out_b, san_b = run_trace(trace_b, arc, cfg_b, case_args)
            problems = []
            for side, rep in (("A", san_a), ("B", san_b)):
                if rep:
                    problems.append(f"SANITIZER ({side}):\n    {rep}")
            if rc_a == 3 or rc_b == 3:
                problems.append("VIOLAZIONE: callback chiamate in concorrenza")
            if rc_a != rc_b:
                problems.append(f"exit code diverso: A={rc_a} B={rc_b}")
            if out_a != out_b:
                diff = list(difflib.unified_diff(out_a.splitlines(), out_b.splitlines(), "A", "B", lineterm="", n=2))
                problems.append("tracce diverse:\n    " + "\n    ".join(diff[:30]))
            if must_be_clean and not trace_is_clean(out_a):
                problems.append("caso integro ma la traccia A non e' tutta OK (suite non valida?)")
            if problems:
                failures += 1
                print(f"FAIL {desc}\n  " + "\n  ".join(problems))
        print(f"\n{len(cases) - failures}/{len(cases)} casi equivalenti")
        return 1 if failures else 0
    finally:
        if args.keep:
            print(f"directory mantenuta: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
