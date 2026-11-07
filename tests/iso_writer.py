#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Scrittura di immagini ISO 9660 con Joliet e Rock Ridge (docs/design/0019).

Crea un albero con nomi lunghi, Unicode, collisioni di identificatori, directory vuote,
file vuoti, link simbolici e permessi; lo scrive con `7zz a -tiso` nelle varianti
Joliet+RR, solo RR, solo Joliet, e verifica:

- il round trip con il lettore di 7-Zip (albero Joliet o albero Rock Ridge con
  -mjoliet=off): contenuti, nomi, link e permessi uguali all'origine;
- la struttura con un parser indipendente scritto qui (descrittori, terminatore, path
  table L e M coerenti, record che non attraversano i settori, ordinamento, identificatori
  ISO 9660 validi e unici, SUSP/RRIP del root, estensioni dei file dentro il volume);
- se pycdlib è installato, anche la lettura dei tre alberi con pycdlib;
- la riscrittura di un'immagine esistente (aggiunta e cancellazione) e le opzioni
  -mvol, -mjoliet, -mrr.
"""

import argparse
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048


def run(args, check=True):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if check and r.returncode != 0:
        raise SystemExit("comando fallito: %s\n%s" % (" ".join(map(str, args)), r.stdout))
    return r.stdout


def make_tree(root):
    os.makedirs(os.path.join(root, "sub", "deep"))
    os.makedirs(os.path.join(root, "emptydir"))
    files = {
        "a.txt": b"hello\n",
        "A.TXT": b"HELLO\n",
        "a.TXT": b"Hello\n",
        "readme": b"no extension\n",
        "README": b"NO EXTENSION\n",
        "empty.txt": b"",
        "ab#$%.c": b"odd chars\n",
        "file.tar.gz": b"two dots\n",
        "file_tar.gz": b"underscore\n",
        "über.txt": b"umlaut\n",
        "ÜBER.txt": b"UMLAUT\n",
        "nome con spazi e àccenti.bin": bytes(range(256)) * 20,
        "x" * 120 + ".txt": b"long name\n",
        os.path.join("sub", "deep", "data.bin"): bytes((i * 7) & 0xFF for i in range(3 * SECTOR + 17)),
        os.path.join("sub", "exact.bin"): b"\x55" * (2 * SECTOR),
        os.path.join("sub", "one.bin"): b"\xAA",
    }
    for name, data in files.items():
        with open(os.path.join(root, name), "wb") as f:
            f.write(data)
    os.chmod(os.path.join(root, "sub", "one.bin"), 0o600)
    os.chmod(os.path.join(root, "sub", "deep"), 0o700)
    posix = os.name == "posix"
    if posix:
        os.symlink("a.txt", os.path.join(root, "link_rel"))
        os.symlink("/etc/hostname", os.path.join(root, "link_abs"))
        os.symlink("../sub/deep/data.bin", os.path.join(root, "emptydir", "link_up"))
    return posix


def snapshot(root):
    """{percorso relativo: (tipo, contenuto o destinazione del link)}"""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        if rel != ".":
            out[rel.replace(os.sep, "/")] = ("dir", None)
        for n in filenames + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]:
            p = os.path.join(dirpath, n)
            key = os.path.relpath(p, root).replace(os.sep, "/")
            if os.path.islink(p):
                out[key] = ("link", os.readlink(p))
            else:
                with open(p, "rb") as f:
                    out[key] = ("file", f.read())
    return out


# ---------- parser ISO 9660 indipendente

def both32(b, off):
    le = struct.unpack_from("<I", b, off)[0]
    be = struct.unpack_from(">I", b, off + 4)[0]
    assert le == be, "both-endian 32 non coerente a %d: %d != %d" % (off, le, be)
    return le


def both16(b, off):
    le = struct.unpack_from("<H", b, off)[0]
    be = struct.unpack_from(">H", b, off + 2)[0]
    assert le == be, "both-endian 16 non coerente"
    return le


def parse_record(b, off):
    ln = b[off]
    if ln == 0:
        return None
    assert ln >= 34 and off % SECTOR + ln <= SECTOR, "record che attraversa un settore o troppo corto"
    r = {
        "len": ln,
        "extent": both32(b, off + 2),
        "size": both32(b, off + 10),
        "flags": b[off + 25],
        "idlen": b[off + 32],
    }
    r["id"] = bytes(b[off + 33: off + 33 + r["idlen"]])
    sys_off = off + 33 + r["idlen"] + (1 if r["idlen"] % 2 == 0 else 0)
    r["susp"] = bytes(b[sys_off: off + ln])
    assert both16(b, off + 28) == 1
    return r


def parse_susp(data):
    """[(firma, versione, payload)]"""
    out = []
    pos = 0
    while pos + 4 <= len(data):
        sig = data[pos:pos + 2]
        ln = data[pos + 2]
        if sig == b"\x00\x00" or ln < 4:
            break
        out.append((sig.decode("ascii"), data[pos + 3], data[pos + 4: pos + ln]))
        pos += ln
    return out


def susp_all(img, susp):
    """voci SUSP di un record, seguendo le CE (aree di continuazione)"""
    ents = parse_susp(susp)
    while True:
        ce = [e for e in ents if e[0] == "CE"]
        if not ce:
            return ents
        ents = [e for e in ents if e[0] != "CE"]
        loc, off, ln = both32(ce[0][2], 0), both32(ce[0][2], 8), both32(ce[0][2], 16)
        assert (loc * SECTOR + off + ln) <= len(img)
        ents += parse_susp(img[loc * SECTOR + off: loc * SECTOR + off + ln])


def read_dir(img, extent, size, joliet):
    """record di una directory (senza . e ..), ordinati come scritti"""
    data = img[extent * SECTOR: extent * SECTOR + size]
    recs = []
    pos = 0
    while pos < len(data):
        r = parse_record(data, pos)
        if r is None:
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        recs.append(r)
        pos += r["len"]
    assert recs[0]["id"] == b"\x00" and recs[1]["id"] == b"\x01", ". e .. mancanti"
    assert recs[0]["extent"] == extent and recs[0]["size"] == size
    return recs


def sl_target(ents):
    """destinazione di un link dalle voci SL (componenti: ROOT, CURRENT, PARENT, nome)"""
    parts = []
    cur = b""
    for sig, ver, d in ents:
        if sig != "SL":
            continue
        pos = 1
        while pos < len(d):
            flags, ln = d[pos], d[pos + 1]
            body = d[pos + 2: pos + 2 + ln]
            pos += 2 + ln
            if flags & 8:
                parts.append(b"")      # root: il percorso inizia con /
            elif flags & 2:
                parts.append(b".")
            elif flags & 4:
                parts.append(b"..")
            else:
                cur += body
                if not flags & 1:
                    parts.append(cur)
                    cur = b""
    return b"/".join(parts).decode("utf-8")


def check_structure(path, joliet, rr, vol, links=None):
    with open(path, "rb") as f:
        img = f.read()
    assert len(img) % SECTOR == 0
    assert img[:16 * SECTOR] == b"\0" * (16 * SECTOR), "area di sistema non vuota"
    pvd = img[16 * SECTOR: 17 * SECTOR]
    assert pvd[0] == 1 and pvd[1:6] == b"CD001" and pvd[6] == 1
    assert pvd[40:72].decode("ascii").rstrip() == vol
    assert both32(pvd, 80) * SECTOR == len(img), "volume space size diverso dal file"
    assert both16(pvd, 128) == SECTOR
    assert pvd[881] == 1
    assert pvd[574:582] == b"HEPTAZIP"
    descs = [pvd]
    sector = 17
    svd = None
    if joliet:
        svd = img[17 * SECTOR: 18 * SECTOR]
        assert svd[0] == 2 and svd[1:6] == b"CD001" and svd[88:91] == b"%/E"
        assert both32(svd, 80) * SECTOR == len(img)
        descs.append(svd)
        sector = 18
    else:
        assert img[17 * SECTOR] == 255
    term = img[sector * SECTOR: sector * SECTOR + 7]
    assert term == b"\xff" + b"CD001" + b"\x01", "terminatore mancante"

    for d, is_joliet in ((pvd, False), (svd, True)):
        if d is None:
            continue
        pt_size = both32(d, 132)
        l_loc = struct.unpack_from("<I", d, 140)[0]
        m_loc = struct.unpack_from(">I", d, 148)[0]
        assert struct.unpack_from("<I", d, 144)[0] == 0 and struct.unpack_from(">I", d, 152)[0] == 0
        L = img[l_loc * SECTOR: l_loc * SECTOR + pt_size]
        M = img[m_loc * SECTOR: m_loc * SECTOR + pt_size]
        # L e M: stesse voci, soli numeri in ordine di byte diverso
        entries = []
        pos = 0
        while pos < pt_size:
            n = L[pos]
            ext_l = struct.unpack_from("<I", L, pos + 2)[0]
            par_l = struct.unpack_from("<H", L, pos + 6)[0]
            ext_m = struct.unpack_from(">I", M, pos + 2)[0]
            par_m = struct.unpack_from(">H", M, pos + 6)[0]
            assert M[pos] == n and ext_l == ext_m and par_l == par_m, "path table L e M diverse"
            ident = L[pos + 8: pos + 8 + n]
            assert ident == M[pos + 8: pos + 8 + n]
            entries.append((ext_l, par_l, ident))
            pos += 8 + n + (n & 1)
        assert pos == pt_size
        assert entries[0][2] == b"\x00" and entries[0][1] == 1, "root della path table"
        parents = [e[1] for e in entries]
        assert parents == sorted(parents), "path table non ordinata per directory genitore"

        root = parse_record(d, 156)
        assert root["id"] == b"\x00" and root["flags"] & 2 and root["len"] == 34
        assert root["extent"] == entries[0][0]

        # discesa ricorsiva: ordinamento, identificatori, estensioni
        dirs_seen = {root["extent"]}
        todo = [(root["extent"], root["size"], b"", "")]
        count = 0
        found_links = set()
        while todo:
            extent, size, prefix, prefix_rr = todo.pop()
            recs = read_dir(img, extent, size, is_joliet)
            ids = [r["id"] for r in recs[2:]]
            assert ids == sorted(ids), "record non ordinati in %r" % prefix
            assert len(ids) == len(set(ids)) or any(r["flags"] & 0x80 for r in recs), "identificatori duplicati in %r" % prefix
            for r in recs[2:]:
                count += 1
                ident = r["id"]
                if is_joliet:
                    name = ident.decode("utf-16-be")
                    assert len(ident) % 2 == 0 and len(name) <= 103
                    assert not any(c in name for c in "*/:;?\\")
                else:
                    name = ident.decode("ascii")
                    if r["flags"] & 2:
                        assert len(name) <= 31 and all(c.isupper() or c.isdigit() or c == "_" for c in name), name
                    else:
                        assert name.endswith(";1") and len(name) <= 32, name
                        base = name[:-2]
                        assert base.count(".") == 1
                        assert all(c.isupper() or c.isdigit() or c in "_." for c in base), name
                if r["flags"] & 2:
                    assert r["extent"] not in dirs_seen, "directory raggiunta due volte"
                    dirs_seen.add(r["extent"])
                    assert r["size"] % SECTOR == 0
                    nm_dir = ""
                    if rr and not is_joliet:
                        nm_dir = b"".join(e[2][1:] for e in susp_all(img, r["susp"]) if e[0] == "NM").decode("utf-8")
                    todo.append((r["extent"], r["size"], prefix + b"/" + ident, prefix_rr + "/" + nm_dir))
                assert (r["extent"] + (r["size"] + SECTOR - 1) // SECTOR) * SECTOR <= len(img), "estensione oltre il volume"
                if rr and not is_joliet:
                    ents = susp_all(img, r["susp"])
                    sigs = [e[0] for e in ents]
                    assert "NM" in sigs and "PX" in sigs and "TF" in sigs, "RRIP incompleto in %r: %s" % (name, sigs)
                    px = [e for e in ents if e[0] == "PX"][0][2]
                    assert len(px) == 40, "PX non nella forma RRIP 1.12"
                    mode = both32(px, 0)
                    assert bool(mode & 0o040000) == bool(r["flags"] & 2), "tipo PX diverso dal flag directory"
                    nm = b"".join(e[2][1:] for e in ents if e[0] == "NM").decode("utf-8")
                    rel = (prefix_rr + "/" + nm).lstrip("/")
                    if links and rel in links:
                        assert stat.S_ISLNK(mode), "PX non e' un link: %r" % rel
                        assert sl_target(ents) == links[rel], (sl_target(ents), links[rel])
                        found_links.add(rel)
                elif not rr and not is_joliet:
                    assert r["susp"] == b"" or r["susp"] == b"\0" * len(r["susp"]), "SUSP presente con -mrr=off"
            if rr and not is_joliet and extent == root["extent"]:
                assert parse_susp(recs[0]["susp"])[0][0] == "SP", "SP del root non in linea"
                ents = susp_all(img, recs[0]["susp"])
                assert ents[0][2] == b"\xbe\xef\x00", "SP del root"
                sigs = [e[0] for e in ents]
                assert "ER" in sigs, "ER del root mancante"
                er = [e for e in ents if e[0] == "ER"][0][2]
                assert er[4:4 + er[0]] == b"IEEE_P1282"
        assert len(dirs_seen) == len(entries), "directory nei record (%d) e nella path table (%d)" % (len(dirs_seen), len(entries))
        if rr and not is_joliet and links:
            assert found_links == set(links), "link SL mancanti: %s" % sorted(set(links) - found_links)
        print("  struttura %s: %d voci, %d directory ok" % ("Joliet" if is_joliet else "ISO 9660", count, len(entries)))


def check_pycdlib(path, joliet, rr, expected_names):
    try:
        import pycdlib
    except ImportError:
        print("  pycdlib non installato: verifica indipendente saltata")
        return
    iso = pycdlib.PyCdlib()
    iso.open(path)
    assert iso.has_joliet() == joliet
    assert iso.has_rock_ridge() == rr, (iso.has_rock_ridge(), rr)
    kinds = ["iso_path"] + (["joliet_path"] if joliet else []) + (["rr_path"] if rr else [])
    for kind in kinds:
        names = set()
        for dirname, dirlist, filelist in iso.walk(**{kind: "/"}):
            for n in filelist + dirlist:
                names.add((dirname.rstrip("/") + "/" + n).lstrip("/"))
        if kind == "rr_path":
            assert names == expected_names, sorted(names ^ expected_names)[:10]
    iso.close()
    print("  pycdlib: %s ok" % ", ".join(kinds))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--7zz", dest="zz", required=True)
    args = ap.parse_args()
    zz = os.path.abspath(args.zz)
    work = tempfile.mkdtemp(prefix="hz-iso-")
    try:
        src = os.path.join(work, "src")
        os.makedirs(src)
        posix = make_tree(src)
        want = snapshot(src)
        expected = set(k for k in want)   # nomi relativi, albero RR
        links = {k: v[1] for k, v in want.items() if v[0] == "link"}
        # 7-Zip non estrae i link con ".." o assoluti ("Dangerous link path"): si
        # verificano nell'immagine (voci SL) e si escludono dal confronto dopo l'estrazione
        unsafe = {k for k, t in links.items() if t.startswith("/") or ".." in t.split("/")}
        cases = [
            ("joliet+rr", ["-mvol=PROVA"], True, True, "PROVA"),
            ("rr", ["-mjoliet=off", "-mvol=solo rr"], False, True, "SOLO RR"),
            ("joliet", ["-mrr=off"], True, False, "ISOIMAGE"),
        ]
        for name, opts, joliet, rr, vol in cases:
            print("caso", name)
            iso = os.path.join(work, name + ".iso")
            run([zz, "a", "-tiso", "-snl", "-bso0", "-bd", "-w" + work, iso] + opts + [os.path.join(src, "*")])
            check_structure(iso, joliet, rr, vol, links if rr else None)
            check_pycdlib(iso, joliet, rr, expected)
            out = os.path.join(work, "out-" + name)
            run([zz, "x", "-snl", "-bso0", "-bd", "-o" + out, iso], check=False)
            # (7-Zip lascia un file vuoto al posto del link rifiutato)
            got = {k: v for k, v in snapshot(out).items() if k not in unsafe}
            want_safe = {k: v for k, v in want.items() if k not in unsafe}
            if joliet:
                # il lettore di 7-Zip usa l'albero Joliet: nomi > 103 caratteri troncati,
                # link come file vuoti; si confrontano i soli nomi che Joliet conserva
                for k, v in want_safe.items():
                    base = k.rsplit("/", 1)[-1]
                    if v[0] == "link" or len(base) > 103 or any(c in base for c in "*/:;?\\"):
                        continue
                    assert k in got and got[k] == v, "round trip Joliet: %r" % k
                print("  round trip Joliet ok")
            else:
                assert got == want_safe, "round trip RR diverso: %s" % sorted(set(got) ^ set(want_safe))[:10]
                if posix:
                    st = os.stat(os.path.join(out, "sub", "one.bin"))
                    assert stat.S_IMODE(st.st_mode) == 0o600, oct(st.st_mode)
                    st = os.stat(os.path.join(out, "sub", "deep"))
                    assert stat.S_IMODE(st.st_mode) == 0o700, oct(st.st_mode)
                print("  round trip Rock Ridge ok (permessi e link)")

        print("riscrittura di un'immagine esistente")
        iso = os.path.join(work, "joliet+rr.iso")
        extra = os.path.join(work, "extra.txt")
        with open(extra, "wb") as f:
            f.write(b"added later\n")
        run([zz, "a", "-tiso", "-bso0", "-bd", iso, extra])
        run([zz, "d", "-bso0", "-bd", iso, "sub/deep/data.bin"])
        listing = run([zz, "l", iso])
        assert "extra.txt" in listing and "data.bin" not in listing and "exact.bin" in listing
        check_structure(iso, True, True, "PROVA")
        out = os.path.join(work, "out-upd")
        run([zz, "x", "-bso0", "-bd", "-o" + out, iso])
        with open(os.path.join(out, "extra.txt"), "rb") as f:
            assert f.read() == b"added later\n"
        with open(os.path.join(out, "sub", "exact.bin"), "rb") as f:
            assert f.read() == want["sub/exact.bin"][1]
        print("  aggiunta e cancellazione ok")

        print("etichetta non valida rifiutata")
        r = subprocess.run([zz, "a", "-tiso", "-bso0", "-bd", os.path.join(work, "bad.iso"), "-mvol=troppo lunga per essere accettata da iso", extra],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert r.returncode != 0
        print("tutto ok")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
