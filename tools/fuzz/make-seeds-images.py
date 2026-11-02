#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Semi per il fuzzing dei formati immagine di sola lettura (design 0007).

    python3 tools/fuzz/make-seeds-images.py <directory_corpus>

7zz non sa creare questi formati, quindi make-seeds.sh li lasciava partire da zero: il
fuzzer arrivava all'intestazione ma quasi mai alle directory e ai dati. Qui si creano
immagini piccole e valide con strumenti che non richiedono root:

- iso, udf: pycdlib (ISO 9660 semplice, Joliet, Rock Ridge, El Torito, UDF 2.60);
- fat: pyfatfs (FAT12, FAT16, FAT32);
- ext: mke2fs -d (ext2, ext3, ext4, con e senza extent e journal);
- squashfs: mksquashfs con i compressori disponibili.

Ogni strumento mancante viene saltato con un avviso.
"""

import io
import os
import shutil
import subprocess
import sys
import tempfile

FILES = {
    "a.txt": b"hello hello hello world\n",
    "dir/numbers.txt": "".join("%d\n" % i for i in range(1, 400)).encode(),
    "dir/sub/random.bin": os.urandom(3000),
    "zero": b"",
}


def make_tree(root):
    for name, data in FILES.items():
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def save(out, fmt, name, data):
    d = os.path.join(out, fmt)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def iso_seeds(out):
    try:
        import pycdlib
    except ImportError:
        print("avviso: pycdlib mancante, niente semi iso/udf", file=sys.stderr)
        return
    variants = [
        ("plain.iso", "iso", {}),
        ("joliet.iso", "iso", {"joliet": 3}),
        ("rockridge.iso", "iso", {"rock_ridge": "1.09"}),
        ("iso1999.iso", "iso", {"interchange_level": 4}),
        ("udf.iso", "udf", {"udf": "2.60"}),
        ("eltorito.iso", "iso", {"eltorito": True}),
    ]
    for name, fmt, opts in variants:
        iso = pycdlib.PyCdlib()
        eltorito = opts.pop("eltorito", False)
        iso.new(**opts)
        rr = "rock_ridge" in opts
        joliet = "joliet" in opts
        udf = "udf" in opts
        level4 = opts.get("interchange_level") == 4

        def iso_path(p, is_dir=False):
            # Nomi 8.3 in maiuscolo (livello 1); il livello 4 accetta i nomi cosi' come sono.
            if level4:
                return "/" + p.upper()
            parts = [x.upper()[:8] for x in p.split("/")]
            if not is_dir:
                stem, _, ext = p.split("/")[-1].upper().partition(".")
                parts[-1] = "%s.%s;1" % (stem[:8], ext[:3])
            return "/" + "/".join(parts)

        dirs = sorted({os.path.dirname(n) for n in FILES if os.path.dirname(n)}
                      | {"dir"}, key=len)
        for d in dirs:
            kw = {}
            if rr:
                kw["rr_name"] = os.path.basename(d)
            if joliet:
                kw["joliet_path"] = "/" + d
            if udf:
                kw["udf_path"] = "/" + d
            iso.add_directory(iso_path(d, True), **kw)
        for n, data in FILES.items():
            kw = {}
            if rr:
                kw["rr_name"] = os.path.basename(n)
            if joliet:
                kw["joliet_path"] = "/" + n
            if udf:
                kw["udf_path"] = "/" + n
            iso.add_fp(io.BytesIO(data), len(data), iso_path(n), **kw)
        if eltorito:
            boot = b"\xeb\xfe" + b"\0" * 2046
            iso.add_fp(io.BytesIO(boot), len(boot), "/BOOT.;1")
            iso.add_eltorito("/BOOT.;1", "/BOOT.CAT;1")
        buf = io.BytesIO()
        iso.write_fp(buf)
        iso.close()
        save(out, fmt, name, buf.getvalue())


def fat_seeds(out, work):
    try:
        from pyfatfs.PyFat import PyFat
        from pyfatfs.PyFatFS import PyFatFS
    except ImportError:
        print("avviso: pyfatfs mancante, niente semi fat", file=sys.stderr)
        return
    for fat_type, size in ((12, 1 << 20), (16, 16 << 20), (32, 40 << 20)):
        path = os.path.join(work, "fat%d.img" % fat_type)
        with open(path, "wb") as f:
            f.truncate(size)
        pf = PyFat()
        pf.mkfs(path, fat_type=fat_type, size=size, label="FUZZ")
        pf.close()
        fs = PyFatFS(path)
        for n, data in FILES.items():
            d = os.path.dirname(n)
            if d:
                fs.makedirs("/" + d, recreate=True)
            with fs.openbin("/" + n, "w") as f:
                f.write(data)
        fs.close()
        with open(path, "rb") as f:
            data = f.read()
        # Le immagini grandi sono quasi tutte zeri: i dati stanno nei primi settori.
        # Si tronca dopo l'ultimo byte diverso da zero (il gestore deve reggere anche
        # un'immagine piu' corta del dichiarato).
        end = len(data.rstrip(b"\0"))
        save(out, "fat", "fat%d.img" % fat_type, data[: (end + 511) & ~511])


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        print("avviso: %s: %s" % (" ".join(cmd[:2]), r.stderr.decode(errors="replace").strip()[:200]),
              file=sys.stderr)
    return r.returncode == 0


def ext_seeds(out, work, tree):
    mke2fs = shutil.which("mke2fs") or shutil.which("mkfs.ext4")
    if not mke2fs:
        print("avviso: mke2fs mancante, niente semi ext", file=sys.stderr)
        return
    variants = [
        ("ext2.img", ["-t", "ext2"]),
        ("ext3.img", ["-t", "ext3", "-J", "size=1"]),
        ("ext4.img", ["-t", "ext4", "-O", "^has_journal"]),
        ("ext4-journal.img", ["-t", "ext4", "-J", "size=1"]),
        ("ext4-64bit-csum.img", ["-t", "ext4", "-O", "^has_journal,64bit,metadata_csum"]),
        ("ext4-inline.img", ["-t", "ext4", "-O", "^has_journal,inline_data", "-I", "256"]),
    ]
    for name, opts in variants:
        path = os.path.join(work, name)
        if os.path.exists(path):
            os.remove(path)
        if run([mke2fs, "-q", "-F", "-b", "1024", "-N", "32", "-m", "0", "-E", "root_owner=0:0"]
               + opts + ["-d", tree, path, "4096" if "-J" in opts else "400"]):
            with open(path, "rb") as f:
                save(out, "ext", name, f.read())


def squashfs_seeds(out, work, tree):
    mk = shutil.which("mksquashfs")
    if not mk:
        print("avviso: mksquashfs mancante, niente semi squashfs", file=sys.stderr)
        return
    for comp in ("gzip", "lzo", "lz4", "xz", "zstd", "lzma"):
        path = os.path.join(work, "%s.sqfs" % comp)
        if os.path.exists(path):
            os.remove(path)
        if run([mk, tree, path, "-comp", comp, "-noappend", "-no-progress", "-quiet", "-all-root"]):
            with open(path, "rb") as f:
                save(out, "squashfs", "%s.sqfs" % comp, f.read())


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    out = os.path.realpath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="7z-seeds-img.", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    try:
        tree = os.path.join(work, "tree")
        make_tree(tree)
        iso_seeds(out)
        fat_seeds(out, work)
        ext_seeds(out, work, tree)
        squashfs_seeds(out, work, tree)
    finally:
        shutil.rmtree(work)
    for root, _, files in sorted(os.walk(out)):
        for f in sorted(files):
            p = os.path.join(root, f)
            print("%9d %s" % (os.path.getsize(p), os.path.relpath(p, out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
