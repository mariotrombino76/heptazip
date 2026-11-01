#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
"""Test dell'output JSON di "7zz l -json" (docs/json-output.md).

Per ogni caso verifica che:
  - stdout sia un unico documento JSON in UTF-8 valido (nient'altro su stdout);
  - per ogni archivio, gli elementi e le loro proprieta' coincidano con "-slt -slmu"
    (stessi nomi convertiti in camelCase, stessi valori; date in ISO 8601 UTC);
  - il codice di uscita sia lo stesso di "l" senza -json;
  - gli archivi non apribili finiscano in "errors".
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import tarfile
from pathlib import Path

PASSWORD = "segreto"


def camel(name: str) -> str:
    words = [w for w in re.split(r"[^A-Za-z0-9]+", name) if w]
    out = []
    for k, w in enumerate(words):
        w = w.lower() if w.isupper() else w
        out.append(w[0].lower() + w[1:] if k == 0 else w[0].upper() + w[1:])
    return "".join(out)


def parse_slt(text: str):
    """Elementi del formato -slt: blocchi 'Chiave = valore' dopo la riga '----------'."""
    if "----------" not in text:
        return []
    body = text.split("----------", 1)[1]
    items = []
    for block in re.split(r"\n\s*\n", body):
        props = {}
        for line in block.splitlines():
            if " = " in line:
                k, v = line.split(" = ", 1)
                props[k] = v
            elif line.endswith(" ="):
                props[line[:-2]] = ""
        if "Path" in props:
            items.append(props)
    return items


def slt_value_matches(slt: str, js) -> bool:
    if isinstance(js, bool):
        return slt == ("+" if js else "-")
    if isinstance(js, int):
        return slt == str(js)
    if isinstance(js, str) and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z", js):
        # -slt -slmu: "2026-10-30 21:23:53.1473363Z"
        return slt == js.replace("T", " ")
    return slt == js


def run(sz, args):
    return subprocess.run([sz, *args], capture_output=True)


def check_case(sz, name, args, failures):
    """Confronta 'l -json' con 'l -slt -slmu' per gli stessi argomenti."""
    pj = run(sz, ["l", "-json", *args])
    pt = run(sz, ["l", "-slt", "-slmu", *args])
    problems = []
    try:
        doc = json.loads(pj.stdout.decode("utf-8"))  # UTF-8 stretto
    except Exception as e:
        failures.append(f"{name}: stdout non e' JSON valido: {e}\n{pj.stdout[:300]!r}")
        return None
    if pj.returncode != pt.returncode:
        problems.append(f"exit code -json={pj.returncode} -slt={pt.returncode}")

    slt_items = parse_slt(pt.stdout.decode("utf-8", "replace"))
    js_items = [it for a in doc["archives"] for it in a["items"]]
    if len(slt_items) != len(js_items):
        problems.append(f"numero di elementi: -slt={len(slt_items)} -json={len(js_items)}")
    else:
        for s_it, j_it in zip(slt_items, js_items):
            if s_it["Path"] != j_it["path"]:
                problems.append(f"path {s_it['Path']!r} != {j_it['path']!r}")
                continue
            for k, v in s_it.items():
                if k in ("Path", "Folder"):
                    continue
                key = camel(k)
                if v == "":
                    if key in j_it:
                        problems.append(f"{j_it['path']}: {key} presente ma vuoto in -slt")
                    continue
                if key not in j_it:
                    problems.append(f"{j_it['path']}: manca {key} (-slt {k} = {v})")
                elif not slt_value_matches(v, j_it[key]):
                    problems.append(f"{j_it['path']}: {key} -slt={v!r} -json={j_it[key]!r}")
    if problems:
        failures.append(f"{name}:\n  " + "\n  ".join(problems[:10]))
    return doc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--7zz", dest="sz", required=True)
    args = ap.parse_args()
    sz = str(Path(args.sz).resolve())

    tmp = Path(tempfile.mkdtemp(prefix="7z-json.", dir="/dev/shm"))
    failures = []
    ncases = 0
    try:
        data = tmp / "data"
        (data / 'dir "q"' / "sub").mkdir(parents=True)
        (data / "vuota").mkdir()
        (data / 'dir "q"' / "città 😀.txt").write_text("ciao\n" * 1000)
        (data / 'dir "q"' / "sub" / "tab\tnew.txt").write_bytes(b"x" * 3000)
        (data / "zero").write_bytes(b"")
        (data / "bin.dat").write_bytes(bytes(range(256)) * 500)

        def mk(name, opts):
            arc = tmp / name
            subprocess.run([sz, "a", *opts, "-bso0", "-bsp0", str(arc), "."], cwd=data, check=True)
            return arc

        a7 = mk("a.7z", ["-mx5"])
        a7ns = mk("ns.7z", ["-mx1", "-ms=off"])
        azip = mk("a.zip", ["-tzip"])
        aenc = mk("enc.7z", ["-mhe=on", f"-p{PASSWORD}"])
        tgz = tmp / "a.tar.gz"
        with tarfile.open(tgz, "w:gz") as t:
            t.add(data, arcname=".")

        cases = [
            ("7z", [str(a7)]),
            ("7z-nonsolid", [str(a7ns)]),
            ("zip", [str(azip)]),
            ("tar.gz (annidato)", [str(tgz)]),
            ("cifrato, password giusta", [f"-p{PASSWORD}", str(aenc)]),
            ("piu' archivi", [str(a7), str(azip)]),
            ("filtro *.txt -r", [str(a7), "*.txt", "-r"]),
        ]
        for name, cargs in cases:
            ncases += 1
            check_case(sz, name, cargs, failures)

        # tar.gz: 7-Zip elenca il livello gzip, con l'elemento a.tar (come -slt)
        doc = json.loads(run(sz, ["l", "-json", str(tgz)]).stdout)
        types = [lv["type"] for lv in doc["archives"][0]["levels"]]
        names = [it["path"] for it in doc["archives"][0]["items"]]
        ncases += 1
        if types != ["gzip"] or names != ["a.tar"]:
            failures.append(f"tar.gz: livelli {types} elementi {names}")

        # dati estranei dopo la fine: si apre con avviso, riportato nel livello
        tail = tmp / "coda.7z"
        tail.write_bytes(a7ns.read_bytes() + b"X" * 100)
        ncases += 1
        doc = check_case(sz, "dati dopo la fine", [str(tail)], failures)
        if doc is not None:
            lv = doc["archives"][0]["levels"][0] if doc["archives"] else {}
            if not doc["archives"] or not (lv.get("warnings") or lv.get("tailSize") or lv.get("errors")):
                failures.append(f"dati dopo la fine: avviso non riportato: {lv}")

        # archivio troncato (intestazione mancante): non si apre -> "errors"
        trunc = tmp / "tronco.7z"
        trunc.write_bytes(a7ns.read_bytes()[: a7ns.stat().st_size * 2 // 3])

        # file inesistente: errore fatale prima dell'elenco -> stdout vuoto,
        # stesso exit code di "l" (contratto in docs/json-output.md)
        ncases += 1
        pj = run(sz, ["l", "-json", str(tmp / "manca.7z")])
        pt = run(sz, ["l", str(tmp / "manca.7z")])
        if pj.stdout.strip() or pj.returncode != pt.returncode or pj.returncode == 0:
            failures.append(f"inesistente: stdout={pj.stdout[:80]!r} exit -json={pj.returncode} l={pt.returncode}")

        # errori di apertura: non archivio, password sbagliata -> "errors"
        notarc = tmp / "testo.txt"
        notarc.write_text("non sono un archivio\n")
        for name, cargs, expect in [
            ("non archivio", [str(notarc)], "testo.txt"),
            ("troncato", [str(tmp / "tronco.7z")], "tronco.7z"),
            ("password sbagliata", ["-psbagliata", str(aenc)], "enc.7z"),
        ]:
            ncases += 1
            pj = run(sz, ["l", "-json", *cargs])
            pt = run(sz, ["l", *cargs])
            try:
                doc = json.loads(pj.stdout.decode("utf-8"))
            except Exception as e:
                failures.append(f"{name}: JSON non valido: {e}")
                continue
            if not doc["errors"] or not doc["errors"][0]["path"].endswith(expect):
                failures.append(f"{name}: 'errors' non contiene {expect}: {doc['errors']}")
            if doc["archives"]:
                failures.append(f"{name}: 'archives' dovrebbe essere vuoto")
            if pj.returncode != pt.returncode:
                failures.append(f"{name}: exit code -json={pj.returncode} l={pt.returncode}")

        # -json con un comando diverso da l: errore di riga di comando, niente JSON
        ncases += 1
        p = run(sz, ["t", "-json", str(a7)])
        if p.returncode != 7 or b"-json" not in p.stderr + p.stdout:
            failures.append(f"t -json: atteso errore di riga di comando (7), exit={p.returncode}")
    finally:
        shutil.rmtree(tmp)

    for f in failures:
        print("FAIL", f)
    print(f"{ncases - len(failures)}/{ncases} casi OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
