#!/usr/bin/env python3
"""Importa i file di lingua della GUI da un pacchetto di 7-Zip e li adatta a Heptazip.

7-Zip distribuisce le traduzioni di 7zFM e 7zG (Lang/*.txt) solo con l'installatore, non con
i sorgenti. Questo script le copia in Lang/ del repository, cambia "7-Zip" in "Heptazip" nelle
voci che il fork ha rinominato e aggiunge le traduzioni delle stringhe nuove del fork (per ora
solo in italiano; le altre lingue mostrano l'inglese del programma per quelle stringhe).

Uso:
    python3 tools/import-lang.py 7z2605-x64.exe        # l'installatore (e' un archivio 7z)
    python3 tools/import-lang.py /percorso/di/Lang     # una cartella Lang gia' estratta

Serve build/7zz (o 7zz nel PATH) per estrarre l'installatore. Formato dei file: una riga con
un numero fissa l'ID corrente; ogni riga seguente e' la stringa di quell'ID e passa al
successivo; righe vuote e commenti (";") saltano un ID senza tradurlo (CPP/Common/Lang.cpp).
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEST = ROOT / "Lang"
SIGNATURE = ";!@Lang2@!UTF-8!"

# ID (CPP/7zip/UI/FileManager/resource.h, GUI/CompressDialogRes.h) in cui il programma dice
# "Heptazip" al posto di "7-Zip": il nome si sostituisce in ogni lingua.
REBRAND = {
    961: "IDM_ABOUT, voce di menu 'About ...'",
    2201: "IDT_SYSTEM_ASSOCIATE, 'Associate ... with:'",
    2301: "IDX_SYSTEM_INTEGRATE_TO_MENU, 'Integrate ... to shell context menu'",
    2900: "IDD_ABOUT, titolo del dialogo Informazioni",
    7810: "IDS_MEM_OPERATION_BLOCKED",
    7814: "IDS_MEM_USAGE_LIMIT_SET_BY_7ZIP",
}
# Voci da togliere in tutte le lingue: il testo del fork e' diverso dall'originale, e una
# traduzione dell'originale sarebbe sbagliata. Restano in inglese, salvo le lingue in FORK.
DROP = {
    2901: "IDT_ABOUT_INFO, '7-Zip is free software' -> frase di Heptazip",
}
# Stringhe del fork, per lingua: {ID: testo}. L'ID segue i define nei file resource.h.
FORK = {
    "it": {
        770: "&Tema",                  # IDM_VIEW_THEME (menu Visualizza)
        771: "&Sistema",               # IDM_VIEW_THEME_SYSTEM
        772: "&Chiaro",                # IDM_VIEW_THEME_LIGHT
        773: "&Scuro",                 # IDM_VIEW_THEME_DARK
        2901: "Heptazip: Copyright (c) 2026 Mario Trombino. Software libero, basato su 7-Zip "
              "di Igor Pavlov (www.7-zip.org).",
        4100: "P&rofilo:",             # IDT_COMPRESS_PROFILE (dialogo Aggiungi all'archivio)
        4101: "Personalizzato",        # IDS_COMPRESS_PROFILE_CUSTOM
        4102: "Il più veloce (zstd)",  # IDS_COMPRESS_PROFILE_FASTEST
        4103: "Estrazione veloce (brotli)",      # IDS_COMPRESS_PROFILE_FAST_EXTRACT
        4104: "Equilibrato (LZMA2)",   # IDS_COMPRESS_PROFILE_BALANCED
        4105: "Compressione massima (LZMA2)",    # IDS_COMPRESS_PROFILE_BEST
        4106: "Compatibile (ZIP)",     # IDS_COMPRESS_PROFILE_COMPATIBLE
        4107: "Testi e dati strutturati (BWT)",  # IDS_COMPRESS_PROFILE_TEXT
    },
}


def parse(text):
    """Legge un file di lingua come fa CLang: restituisce (commenti iniziali, {id: stringa})."""
    lines = text.split("\n")
    if not lines[0].startswith(SIGNATURE):
        raise ValueError("firma mancante")
    header, strings, id_ = [], {}, -1024
    in_header = True
    for line in lines[1:]:
        s = line.rstrip("\r")
        stripped = s.strip()
        if in_header and stripped.startswith(";"):
            header.append(s)
            id_ += 1
            continue
        in_header = False
        if not stripped or stripped.startswith(";"):
            id_ += 1
            continue
        if stripped.isdigit():
            n = int(stripped)
            if n < id_:
                raise ValueError(f"ID {n} dopo {id_}")
            id_ = n
            continue
        if id_ < 0:
            raise ValueError("stringa prima del primo ID")
        strings[id_] = s
        id_ += 1
    return header, strings


def render(header, strings):
    """Scrive il file: blocchi di ID consecutivi, una riga vuota tra un blocco e l'altro."""
    out = [SIGNATURE] + header + [""]
    prev = None
    for id_ in sorted(strings):
        if prev is None or id_ != prev + 1:
            if prev is not None:
                out.append("")
            out.append(str(id_))
        out.append(strings[id_])
        prev = id_
    out.append("")
    return "﻿" + "\n".join(out)


def adapt(name, text):
    header, strings = parse(text)
    changed = []
    dropped = []
    for id_ in REBRAND:
        if id_ not in strings:
            continue
        s = re.sub("7-zip", "Heptazip", strings[id_], flags=re.IGNORECASE)
        if s != strings[id_]:
            strings[id_] = s
            changed.append(id_)
        if any(unicodedata.numeric(ch, None) == 7 for ch in s):
            # "7-Zip" traslitterato (7-जिप, ૭-જિપ, ۷-زېپ...): meglio l'inglese del programma
            del strings[id_]
            dropped.append(id_)
    for id_ in DROP:
        strings.pop(id_, None)
    for id_, s in FORK.get(name, {}).items():
        strings[id_] = s
    header.append("; Heptazip (tools/import-lang.py): 7-Zip -> Heptazip in "
                  f"{', '.join(map(str, changed)) or 'nessuna voce'}; voci tolte: "
                  f"{', '.join(map(str, sorted(list(DROP) + dropped)))}; stringhe del fork: "
                  f"{len(FORK.get(name, {}))}")
    return render(header, strings)


def find_7zz():
    for c in (ROOT / "build" / "7zz", shutil.which("7zz")):
        if c and Path(c).exists():
            return str(c)
    sys.exit("serve build/7zz (o 7zz nel PATH) per estrarre l'installatore")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", type=Path, help="installatore di 7-Zip (7zNNNN-x64.exe) o cartella Lang")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        src = args.source
        if src.is_file():
            subprocess.run([find_7zz(), "x", "-bso0", "-bsp0", f"-o{tmp}", str(src), "Lang"], check=True)
            src = Path(tmp) / "Lang"
        files = sorted(p for p in src.iterdir() if p.suffix in (".txt", ".ttt"))
        if not files:
            sys.exit(f"nessun file di lingua in {src}")
        DEST.mkdir(exist_ok=True)
        for old in DEST.glob("*.t?t"):
            old.unlink()
        for p in files:
            text = p.read_text(encoding="utf-8-sig")
            if p.suffix == ".txt":
                text = adapt(p.stem, text)
            else:
                text = "﻿" + text  # en.ttt: riferimento inglese, copiato com'e'
            (DEST / p.name).write_text(text, encoding="utf-8", newline="\n")
        print(f"{len(files)} file scritti in {DEST}")


if __name__ == "__main__":
    main()
