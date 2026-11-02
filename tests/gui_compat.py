#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Compatibilita' con i gestori di archivi di Linux che usano 7z/7zz (issue #7).

Ark (KDE, plugins/cli7zplugin/cliplugin.cpp) e File Roller (GNOME,
src/fr-command-7z.c) non comprimono da soli: eseguono 7z/7zz e ne leggono l'output.
Questo test legge l'output di 7zz con le stesse regole:

- Ark: il programma e' "7-Zip" se l'output senza argomenti contiene "7-Zip"; l'elenco
  (7z l -slt) si legge solo dopo una riga ^7-Zip(?: \\(\\w\\))? ([\\d\\.]+) .*$, poi
  "--"/"----", "Type = ", "----------" e le voci "Path = ", "Size = ", "Folder = "...;
- File Roller: l'elenco (7zz l -slt) inizia alla riga "----------", voci separate da
  righe vuote, campi "nome = valore"; il supporto RAR si cerca come "Rar3" dopo
  "Codecs:" nell'output di 7zz i.

Per ogni formato (7z, zip, tar, gzip) l'elenco letto deve essere quello dei file
dell'archivio.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ARK_VERSION_LINE = re.compile(r"^7-Zip(?: \(\w\))? ([\d\.]+) .*$")
ARK_TYPES = {"7z", "bzip2", "gzip", "xz", "tar", "zip", "Rar", "Split"}


def run(cmd, cwd=None):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd)
    if r.returncode != 0:
        raise RuntimeError("%s: exit %d\n%s" % (" ".join(cmd), r.returncode, r.stdout.decode(errors="replace")))
    return r.stdout.decode("utf-8", errors="replace")


def ark_list(output):
    """Le voci (path, cartella) come le legge il plugin cli7z di Ark."""
    state = "title"
    entries = []
    cur = None
    for line in output.splitlines():
        if state == "title":
            if ARK_VERSION_LINE.match(line):
                state = "header"
        elif state == "header":
            if line in ("--", "----"):
                state = "archive"
        elif state == "archive":
            if line == "----------":
                state = "entry"
            elif line.startswith("Type = "):
                if line[7:].strip() not in ARK_TYPES:
                    raise RuntimeError("Ark: tipo non supportato " + line)
        elif state == "entry":
            if line.startswith("Path = "):
                cur = {"path": line[7:], "dir": False}
                entries.append(cur)
            elif cur is not None and line.startswith("Folder = "):
                cur["dir"] = line[9:].strip() == "+"
            elif cur is not None and line.startswith("Attributes = ") and "D" in line[13:]:
                cur["dir"] = True
    if state != "entry":
        raise RuntimeError("Ark: elenco mai iniziato (stato %s)" % state)
    return sorted((e["path"], e["dir"]) for e in entries)


def file_roller_list(output):
    """Le voci (path, cartella) come le legge fr-command-7z.c di File Roller."""
    started = False
    entries = []
    fields = {}

    def flush():
        if "Path" in fields:
            attrs = fields.get("Attributes", "")
            entries.append((fields["Path"], fields.get("Folder") == "+" or attrs.startswith("D")))
        fields.clear()

    for line in output.splitlines():
        if not started:
            started = (line == "----------")
            continue
        if line == "":
            flush()
            continue
        parts = line.split(" = ", 1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1]
    flush()
    if not started:
        raise RuntimeError("File Roller: elenco mai iniziato")
    return sorted(entries)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    args = ap.parse_args()
    sz = os.path.abspath(args.sz)
    errors = 0

    banner = run([sz])
    if "7-Zip" not in banner:
        print("ERRORE: Ark non riconosce 7-Zip nell'output senza argomenti")
        errors += 1
    info = run([sz, "i"])
    codecs = info.find("Codecs:")
    if codecs < 0 or "Rar3" not in info[codecs:]:
        print("ERRORE: File Roller non trova Rar3 dopo Codecs: in '7zz i'")
        errors += 1

    work = tempfile.mkdtemp(prefix="7z-gui-compat.")
    try:
        src = os.path.join(work, "src")
        os.makedirs(os.path.join(src, "dir", "sub"))
        files = {"a.txt": b"hello\n" * 100, os.path.join("dir", "b.bin"): bytes(range(256)) * 10,
                 os.path.join("dir", "sub", "c d.txt"): b"spazio nel nome\n"}
        for name, data in files.items():
            with open(os.path.join(src, name), "wb") as f:
                f.write(data)
        expected_files = sorted(files)

        for fmt, ext in (("7z", "7z"), ("zip", "zip"), ("tar", "tar"), ("gzip", "gz")):
            arc = os.path.join(work, "a." + ext)
            if fmt == "gzip":
                run([sz, "a", "-tgzip", "-bso0", "-bsp0", arc, os.path.join(src, "a.txt")])
                want = ["a.txt"]
            else:
                run([sz, "a", "-t" + fmt, "-bso0", "-bsp0", arc, "a.txt", "dir"], cwd=src)
                want = expected_files
            listing = run([sz, "l", "-slt", "--", arc])
            for who, parse in (("Ark", ark_list), ("File Roller", file_roller_list)):
                try:
                    got = [p for p, is_dir in parse(listing) if not is_dir]
                except RuntimeError as e:
                    print("ERRORE %s %s: %s" % (who, fmt, e))
                    errors += 1
                    continue
                ok = sorted(got) == sorted(want)
                if not ok:
                    errors += 1
                print("%-11s %-5s %s" % (who, fmt, "ok" if ok else "DIVERSO %s invece di %s" % (got, want)))
            os.remove(arc)
    finally:
        shutil.rmtree(work)
    print("errori:", errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
