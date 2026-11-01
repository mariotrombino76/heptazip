#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Suite di equivalenza della compressione .7z parallela (docs/design/0003, par. 6).

Genera dati di test deterministici e, per ogni profilo, crea l'archivio due volte:

    A = 7zz a <opzioni> <config-a>      (es. -mpfc=off: ciclo sequenziale)
    B = 7zz a <opzioni> <config-b>      (es. -mpfc=on)

Gli archivi devono essere IDENTICI byte per byte, e anche l'output della console
(-bb3: ordine dei file, avvisi) e il codice di uscita (requisito 4.1). Per gli archivi
cifrati (IV casuale) si confrontano console e codice di uscita, e si verifica che B
si estragga e coincida con i dati.

Casi: metodi (LZMA2, LZMA, zstd, PPMd, BZip2, Deflate, Deflate64, Copy, BCJ2, FLOAT, filtri),
suddivisioni (-ms=off, -ms=e, -ms=<dim>, -qs, solid), un folder > 64 MiB in mezzo a
folder piccoli, file vuoti e directory, file illeggibili, aggiornamento di un archivio
esistente (aggiunta, sostituzione con ricompressione dei folder esistenti).
"""

import argparse
import difflib
import filecmp
import os
import random
import struct
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PASSWORD = "segreto"

# (nome, opzioni, cifrato)
PROFILES = [
    ("lzma2-mx1-nonsolid", ["-mx1", "-ms=off"], False),
    ("lzma2-mx5-nonsolid", ["-mx5", "-ms=off"], False),
    ("lzma2-mx9-1m",       ["-mx9", "-ms=1m"], False),
    ("lzma2-ext",          ["-mx5", "-ms=e"], False),
    ("lzma2-qs-2m",        ["-mx5", "-ms=2m", "-mqs"], False),
    ("lzma2-blocks",       ["-mx5", "-ms=4m", "-m0=lzma2:c=256k"], False),
    ("lzma2-solid",        ["-mx5"], False),
    ("lzma-nonsolid",      ["-m0=lzma", "-ms=off"], False),
    ("lzma-hc4-2m",        ["-m0=lzma:mf=hc4", "-ms=2m"], False),
    ("zstd-mx3-1m",        ["-m0=zstd", "-mx3", "-ms=1m"], False),
    ("zstd-mx19-nonsolid", ["-m0=zstd", "-mx19", "-ms=off"], False),
    ("zstd-window-4m",     ["-m0=zstd:d=256k", "-mx5", "-ms=4m"], False),
    ("ppmd-1m",            ["-m0=ppmd", "-ms=1m"], False),
    ("bzip2-2m",           ["-m0=bzip2", "-ms=2m"], False),
    ("deflate-nonsolid",   ["-m0=deflate", "-ms=off"], False),
    ("deflate64-1m",       ["-m0=deflate64", "-ms=1m"], False),
    ("copy-nonsolid",      ["-mx0", "-ms=off"], False),
    ("bcj2-nonsolid",      ["-mf=BCJ2", "-ms=off"], False),
    ("delta-1m",           ["-mf=Delta:4", "-ms=1m"], False),
    ("arm64-nonsolid",     ["-mf=ARM64", "-ms=off"], False),
    # metodo FLOAT (design 0008): pesi BF16/FP32 tra file di altro tipo
    ("float-nonsolid",     ["-m0=FLOAT", "-ms=off"], False),
    ("float-solid-2m",     ["-m0=FLOAT", "-ms=2m"], False),
    ("float-blocks-64k",   ["-m0=FLOAT:c=64k", "-mx1"], False),
    # pochi job alla volta: l'ammissione attende memoria (non thread)
    ("lowmem-nonsolid",    ["-mx5", "-ms=off", "-mmemuse=120m"], False),
    ("aes-nonsolid",       ["-mx3", "-ms=off", f"-p{PASSWORD}"], True),
    ("aes-headers-1m",     ["-mx3", "-ms=1m", "-mhe=on", f"-p{PASSWORD}"], True),
]

THREADS = ["-mmt=2", "-mmt=on"]
# profili provati anche con -mmt=4 (peso dei thread intermedio)
THREADS_4 = {"lzma2-mx5-nonsolid", "zstd-mx3-1m", "bzip2-2m", "lzma-nonsolid"}


def make_data(root: Path) -> None:
    """Dati deterministici: testo di varie dimensioni, file vuoti, dati casuali,
    eseguibili ELF (gruppo con filtro BCJ/ARM64), directory vuote."""
    r = random.Random(3)
    words = [bytes(r.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(2, 9)))
             for _ in range(500)]
    for d in range(6):
        sub = root / f"dir{d}"
        sub.mkdir(parents=True)
        for k in range(30):
            name = sub / f"f{k:02}.{['txt', 'c', 'log', 'dat'][k % 4]}"
            if k % 13 == 0:
                name.write_bytes(b"")
                continue
            n = r.choice([r.randint(1, 512), r.randint(512, 32 << 10), r.randint(32 << 10, 300 << 10)])
            out = bytearray()
            while len(out) < n:
                out += r.choice(words) + b" "
            name.write_bytes(bytes(out[:n]))
    (root / "empty-dir").mkdir()
    (root / "random.bin").write_bytes(r.randbytes(1 << 20))
    (root / "repeat.txt").write_bytes(b"0123456789abcdef" * (1 << 16))
    # pesi di un modello: gaussiani in BF16, e in FP32 dopo un byte (fase 1)
    bf16 = bytearray()
    for _ in range(1 << 20):
        v = struct.unpack("<I", struct.pack("<f", r.gauss(0.0, 0.02)))[0]
        bf16 += struct.pack("<H", (v + 0x7FFF + ((v >> 16) & 1)) >> 16)
    (root / "weights.bf16").write_bytes(bytes(bf16))
    (root / "weights-fp32.bin").write_bytes(b"\x07" + struct.pack("<65536f", *(r.gauss(0.0, 0.02) for _ in range(65536))))
    # maschera di attenzione (0/1 triangolare) dopo pesi FP16: blocchi zstd del metodo FLOAT
    mask = b"".join(bytes(1 if j <= i else 0 for j in range(1024)) for i in range(1024))
    fp16 = struct.pack("<131072e", *(r.gauss(0.0, 0.02) for _ in range(131072)))
    (root / "model-fp16-mask.bin").write_bytes(fp16 + mask)
    # file di modelli riconosciuti dall'analisi: FLOAT al posto di LZMA2 (design 0008)
    sub = root / "models"
    sub.mkdir()
    bf16 = struct.pack("<65536H", *((v + 0x7FFF + ((v >> 16) & 1)) >> 16 for v in
        (struct.unpack("<I", struct.pack("<f", r.gauss(0.0, 0.02)))[0] for _ in range(65536))))
    header = (b'{"w":{"dtype":"BF16","shape":[256,256],"data_offsets":[0,131072]},'
              b'"mask":{"dtype":"U8","shape":[512,512],"data_offsets":[131072,393216]}}')
    header += b" " * ((-len(header)) % 8)
    tri = b"".join(bytes(1 if j <= i else 0 for j in range(512)) for i in range(512))
    (sub / "model.safetensors").write_bytes(struct.pack("<Q", len(header)) + header + bf16 + tri)
    def gguf_str(x):
        return struct.pack("<Q", len(x)) + x
    kv = gguf_str(b"general.file_type") + struct.pack("<II", 4, 1)   # MOSTLY_F16
    gg = b"GGUF" + struct.pack("<IQQ", 3, 0, 1) + kv
    gg += b"\0" * ((-len(gg)) % 32)
    (sub / "tiny-f16.gguf").write_bytes(gg + struct.pack("<65536e", *(r.gauss(0.0, 0.02) for _ in range(65536))))
    # GGUF Q4_K_M come lo scrive llama-quantize: general.file_type dopo il vocabolario
    # (oltre i 16 KiB letti dall'analisi), poi strutture Q4_K (blocchi mode 3 di FLOAT)
    toks = b"".join(gguf_str(b"tok%05d" % i) for i in range(30000))
    kv = (gguf_str(b"tokenizer.ggml.tokens") + struct.pack("<IIQ", 9, 8, 30000) + toks
          + gguf_str(b"general.file_type") + struct.pack("<II", 4, 15))   # MOSTLY_Q4_K_M
    gg = b"GGUF" + struct.pack("<IQQ", 3, 0, 2) + kv
    gg += b"\0" * ((-len(gg)) % 32)
    def nib():
        return max(0, min(15, int(round(r.gauss(7.5, 2.5)))))
    q4k = bytearray()
    for _ in range(2000):
        q4k += struct.pack("<ee", 0.001 + 0.002 * r.random(), 0.002 + 0.002 * r.random())
        q4k += bytes(max(0, min(63, int(r.gauss(32, 10)))) for _ in range(12))
        q4k += bytes(nib() | (nib() << 4) for _ in range(128))
    (sub / "tiny-q4k.gguf").write_bytes(gg + bytes(q4k))
    # eseguibili: per il gruppo dei filtri exe (analisi dell'intestazione ELF)
    elfs = [Path(p) for p in ("/usr/bin/true", "/usr/bin/ls", "/usr/bin/cat") if Path(p).is_file()]
    for i, src in enumerate(elfs):
        shutil.copyfile(src, root / f"prog{i}")


def make_big(root: Path) -> None:
    """Un file > 64 MiB (comprimibile, quindi veloce) tra file piccoli: il folder
    grande va inline e i piccoli in parallelo prima e dopo."""
    (root / "m-big.dat").write_bytes(b"big-file-data-" * ((65 << 20) // 14 + 1))
    for i in range(6):
        (root / f"a{i}.txt").write_bytes((b"small %d " % i) * (20000 + i * 3000))
        (root / f"z{i}.txt").write_bytes((b"after %d " % i) * (15000 + i * 2000))


def run(sz, args, cwd):
    p = subprocess.run([sz, *args], cwd=cwd, capture_output=True)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer", "runtime error:",
                     "ERROR: LeakSanitizer")


def sanitizer_report(text: str):
    for m in SANITIZER_MARKERS:
        pos = text.find(m)
        if pos >= 0:
            return "\n    ".join(text[pos:].splitlines()[:25])
    return None


def compare(sz, name, base_args, cfg_a, cfg_b, cwd, work, encrypted, failures, src_dir=None,
            prepare=None, files=(), expect_rc=(0,)):
    """Crea l'archivio con le due configurazioni e confronta.
    (prepare): archivio di partenza, copiato prima del comando (aggiornamento).
    (files): file da aggiungere (dopo il nome dell'archivio; vuoto: tutto (cwd))."""
    arcs = {}
    outs = {}
    fname = name.replace("/", "_").replace(" ", "")
    for tag, cfg in (("a", cfg_a), ("b", cfg_b)):
        arc = work / f"{fname}.{tag}.7z"
        if arc.exists():
            arc.unlink()
        if prepare:
            shutil.copyfile(prepare, arc)
        rc, out, err = run(sz, [*base_args, *cfg, "-bb3", "-bsp0", str(arc), *files], cwd)
        rep = sanitizer_report(err) or sanitizer_report(out)
        if rep:
            failures.append(f"{name} [{tag}]: report del sanitizer:\n    {rep}")
            return
        arcs[tag] = arc
        # la riga con il percorso dell'archivio cambia solo per il nome (.a/.b)
        out = out.replace(arc.name, "ARC")
        if encrypted:
            # IV casuali nell'intestazione compressa: la dimensione cambia a ogni esecuzione
            out = "\n".join(l for l in out.splitlines() if not l.startswith("Archive size:"))
        outs[tag] = (rc, out, err.replace(arc.name, "ARC"))
    problems = []
    # un caso che fallisce in entrambe le configurazioni non verifica nulla
    if outs["a"][0] not in expect_rc:
        problems.append(f"codice di uscita inatteso {outs['a'][0]} (atteso {expect_rc}):\n    "
                        + outs["a"][2][-600:])
    if outs["a"][0] != outs["b"][0]:
        problems.append(f"codice di uscita A={outs['a'][0]} B={outs['b'][0]}")
    for k, what in ((1, "stdout"), (2, "stderr")):
        if outs["a"][k] != outs["b"][k]:
            diff = difflib.unified_diff(outs["a"][k].splitlines(), outs["b"][k].splitlines(),
                                        "A", "B", lineterm="", n=2)
            problems.append(f"{what} diverso:\n    " + "\n    ".join(list(diff)[:30]))
    if not encrypted:
        if arcs["a"].exists() != arcs["b"].exists():
            problems.append("un solo archivio creato")
        elif arcs["a"].exists() and not filecmp.cmp(arcs["a"], arcs["b"], shallow=False):
            problems.append(f"archivi diversi ({arcs['a'].stat().st_size} / {arcs['b'].stat().st_size} byte)")
    elif src_dir is not None and outs["b"][0] == 0:
        # cifrato: B deve estrarsi e coincidere con i dati
        out_dir = work / f"{fname}.x"
        shutil.rmtree(out_dir, ignore_errors=True)
        rc, _, err = run(sz, ["x", f"-p{PASSWORD}", "-bso0", "-bsp0", f"-o{out_dir}", str(arcs["b"])], cwd)
        if rc != 0 or not same_tree(src_dir, out_dir):
            problems.append(f"estrazione di B fallita o diversa (rc={rc})")
        shutil.rmtree(out_dir, ignore_errors=True)
    for arc in arcs.values():
        if arc.exists():
            arc.unlink()
    if problems:
        failures.append(f"{name}:\n  " + "\n  ".join(problems))


def check_auto_float(sz, data: Path, work: Path, failures) -> None:
    """Senza opzioni i GGUF (F16, e Q4_K_M con la chiave oltre il buffer dell'analisi)
    vanno al metodo FLOAT, e l'archivio si estrae uguale. (model.safetensors ha tanti
    tensori U8 quanti BF16: resta a LZMA2.)"""
    arc = work / "auto-float.7z"
    rc, out, err = run(sz, ["a", "-t7z", "-bso0", "-bsp0", str(arc), "models"], data)
    if rc != 0:
        failures.append(f"auto-float: codice di uscita {rc}: {err[-300:]}")
        return
    rc, out, err = run(sz, ["l", "-slt", str(arc)], data)
    methods = {}
    path = None
    for line in out.splitlines():
        if line.startswith("Path = "):
            path = line[7:]
        elif line.startswith("Method = ") and path:
            methods[Path(path).name] = line[9:]
    for name in ("tiny-f16.gguf", "tiny-q4k.gguf"):
        if not methods.get(name, "").startswith("FLOAT"):
            failures.append(f"auto-float: {name} con metodo {methods.get(name)!r}, atteso FLOAT")
    out_dir = work / "auto-float.x"
    rc, _, err = run(sz, ["x", "-bso0", "-bsp0", f"-o{out_dir}", str(arc)], data)
    if rc != 0 or not same_tree(data / "models", out_dir / "models"):
        failures.append(f"auto-float: estrazione fallita o diversa (rc={rc})")
    shutil.rmtree(out_dir, ignore_errors=True)
    arc.unlink()


def same_tree(a: Path, b: Path) -> bool:
    c = filecmp.dircmp(a, b)
    if c.left_only or c.right_only or c.diff_files or c.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, c.common_files, shallow=False)
    if mismatch or errors:
        return False
    return all(same_tree(a / d, b / d) for d in c.common_dirs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    ap.add_argument("--config-a", default="-mpfc=off")
    ap.add_argument("--config-b", default="-mpfc=on")
    ap.add_argument("--only", help="sottostringa dei nomi dei casi")
    args = ap.parse_args()
    sz = str(Path(args.sz).resolve())
    cfg_a = args.config_a.split()
    cfg_b = args.config_b.split()

    work = Path(tempfile.mkdtemp(prefix="7z-upd-equiv.", dir="/dev/shm"))
    failures = []
    ncases = 0
    try:
        data = work / "data"
        make_data(data)
        big = work / "big"
        big.mkdir()
        make_big(big)
        print(f"A: {' '.join(cfg_a)}\nB: {' '.join(cfg_b)}\nlavoro: {work}")
        if not args.only or args.only in "auto-float":
            ncases += 1
            check_auto_float(sz, data, work, failures)

        def case(name, *a, **kw):
            nonlocal ncases
            if args.only and args.only not in name:
                return
            ncases += 1
            compare(sz, name, *a, **kw)

        for pname, opts, enc in PROFILES:
            for mt in THREADS + (["-mmt=4"] if pname in THREADS_4 else []):
                case(f"{pname}/{mt}", ["a", "-t7z", *opts, mt], cfg_a, cfg_b, data, work, enc,
                     failures, src_dir=data)

        # folder grande (> 64 MiB) in mezzo: percorso inline tra quelli paralleli
        for opts in (["-mx1", "-ms=off"], ["-m0=zstd", "-mx1", "-ms=1m"]):
            case(f"big-in-middle/{' '.join(opts)}", ["a", "-t7z", *opts, "-mmt=on"], cfg_a, cfg_b,
                 big, work, False, failures)

        # file illeggibili: avviso, file escluso dal folder (Processed = false)
        locked = data / "dir3" / "f05.c"
        os.chmod(locked, 0)
        try:
            for opts in (["-mx5", "-ms=off"], ["-mx5", "-ms=1m"], ["-m0=zstd", "-ms=off"]):
                # codice 1: avviso (file non letto)
                case(f"unreadable/{' '.join(opts)}", ["a", "-t7z", *opts, "-mmt=on"], cfg_a, cfg_b,
                     data, work, False, failures, expect_rc=(1,))
        finally:
            os.chmod(locked, 0o644)

        # aggiornamento: archivio di partenza creato una volta (sequenziale), poi
        # aggiunta di file nuovi, e sostituzione di file (ricompressione dei folder)
        base = work / "base.7z"
        rc, _, err = run(sz, ["a", "-t7z", "-mx3", "-ms=2m", "-mpfc=off", "-bso0", "-bsp0",
                              str(base), "dir0", "dir1", "random.bin", "models"], data)
        if rc != 0:
            failures.append(f"creazione dell'archivio di partenza fallita: {err[-500:]}")
        else:
            case("update/add", ["a", "-t7z", "-mx3", "-ms=2m", "-mmt=on"],
                 cfg_a, cfg_b, data, work, False, failures, prepare=base,
                 files=["dir2", "dir3", "repeat.txt"])
            # un folder FLOAT esiste gia': i nuovi file di modelli vanno nello stesso gruppo
            case("update/add-models", ["a", "-t7z", "-mx3", "-ms=2m", "-mmt=on"],
                 cfg_a, cfg_b, data, work, False, failures, prepare=base,
                 files=["weights.bf16", "dir2"])
            case("update/add-nonsolid", ["a", "-t7z", "-mx5", "-ms=off", "-mmt=on"],
                 cfg_a, cfg_b, data, work, False, failures, prepare=base,
                 files=["dir4", "dir5", "prog0"])
            # file nuovi con lo stesso nome di file esistenti (u: sostituzione)
            upd = work / "upd"
            shutil.copytree(data / "dir1", upd / "dir1")
            for f in sorted((upd / "dir1").iterdir())[::3]:
                f.write_bytes(f.read_bytes()[::-1] + b"changed")
            case("update/replace", ["u", "-t7z", "-mx3", "-ms=2m", "-mmt=on"],
                 cfg_a, cfg_b, upd, work, False, failures, prepare=base, files=["dir1"])
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for f in failures:
        print("FAIL", f)
    print(f"{ncases - len(failures)}/{ncases} casi equivalenti")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
