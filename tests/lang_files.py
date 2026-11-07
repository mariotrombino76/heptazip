#!/usr/bin/env python3
"""Controlla i file di lingua della GUI in Lang/ (issue #104).

Ogni file deve essere leggibile da CLang (CPP/Common/Lang.cpp): firma, meno di 1 MiB, ID mai
decrescenti, sequenze di escape valide. Inoltre le voci rinominate dal fork non devono piu'
dire "7-Zip", e it.txt deve avere le stringhe del fork elencate in tools/import-lang.py.
"""
import argparse
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def load_importer():
    spec = importlib.util.spec_from_file_location("import_lang", ROOT / "tools" / "import-lang.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def check_escapes(text):
    """Come CLang: '\\' seguito da fine riga o fine file rende il file illeggibile."""
    i, n = 0, len(text)
    while i < n:
        if text[i] == "\\":
            if i + 1 >= n or text[i + 1] == "\n":
                return False
            i += 2
            continue
        i += 1
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lang", type=Path, default=ROOT / "Lang", help="cartella dei file (default: Lang/)")
    args = ap.parse_args()
    imp = load_importer()
    errors = []
    files = sorted(args.lang.glob("*.txt"))
    if len(files) < 90:
        errors.append(f"solo {len(files)} file .txt in {args.lang} (attesi almeno 90)")
    if not (args.lang / "en.ttt").is_file():
        errors.append("manca en.ttt, il riferimento inglese")
    for p in files:
        raw = p.read_bytes()
        if len(raw) >= 1 << 20:
            errors.append(f"{p.name}: {len(raw)} byte, CLang rifiuta i file da 1 MiB in su")
            continue
        text = raw.decode("utf-8-sig")
        if not check_escapes(text):
            errors.append(f"{p.name}: barra rovesciata a fine riga")
        try:
            _, strings = imp.parse(text)
        except ValueError as e:
            errors.append(f"{p.name}: {e}")
            continue
        for id_ in imp.REBRAND:
            s = strings.get(id_, "")
            if "7-zip" in s.lower():
                errors.append(f"{p.name}: la voce {id_} dice ancora 7-Zip: {s!r}")
        fork = imp.FORK.get(p.stem, {})
        for id_ in imp.DROP:
            if id_ in strings and id_ not in fork:
                errors.append(f"{p.name}: la voce {id_} doveva essere tolta")
        for id_, s in fork.items():
            if strings.get(id_) != s:
                errors.append(f"{p.name}: voce {id_} del fork attesa {s!r}, trovata {strings.get(id_)!r}")
    if "it" not in imp.FORK or len(imp.FORK["it"]) < 13:
        errors.append("tools/import-lang.py: mancano le stringhe italiane del fork")
    for e in errors:
        print("ERRORE:", e)
    print(f"{len(files)} file di lingua controllati, {len(errors)} errori")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
