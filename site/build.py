#!/usr/bin/env python3
"""Genera il sito di presentazione di Heptazip in due lingue.

    cd site && npm ci        # una volta: installa Tailwind (package-lock.json)
    python3 site/build.py    # scrive site/dist

Sorgente unica: src/page.it.html, il corpo della pagina in italiano con le classi di
Tailwind. Da li' escono

    dist/index.html      italiano
    dist/en/index.html   inglese, con la tabella di traduzione EN qui sotto

piu' sitemap.xml, llms.txt, il foglio di stile compilato da Tailwind (src/input.css) e i
file statici (caratteri da static/, logo da docs/logo). Ogni voce della tabella dichiara
quante volte deve comparire nel sorgente: se il testo italiano cambia e la traduzione no,
la build si ferma invece di pubblicare una pagina mezza tradotta.

I percorsi dei file sono relativi, cosi' il sito funziona sia alla radice di un dominio
sia in una sottocartella (GitHub Pages: /heptazip/).
"""
import argparse
import datetime
import json
import re
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOGO = HERE.parent / "docs" / "logo"
REPO = "https://github.com/mariotrombino76/heptazip"
DEFAULT_URL = "https://mariotrombino76.github.io/heptazip"
# Codice del tag "google-site-verification" dato dalla Search Console (metodo "Tag HTML"):
# vuoto = nessun tag. Non e' un segreto.
GOOGLE_SITE_VERIFICATION = "cglE_lnZMFGyuR0gDocY3gAZXG-NiyUFGiOwP3tu44w"

# lingua -> cartella sotto la radice del sito, percorso verso la radice, testi dell'intestazione
META = {
    "it": {
        "dir": "", "root": "",
        "title": "Heptazip: stessi comandi di 7-Zip, più veloce e più parallelo",
        "desc": "Heptazip è un fork di 7-Zip 26.05: stessi comandi, LZMA2 e gzip più veloci, lavoro in "
                "parallelo, metodi nuovi per testi e modelli AI. Per Linux e Windows.",
        "locale": "it_IT", "alt_locale": "en_US",
        "img_alt": "Logo di Heptazip: un ettagono diviso in sette lame attorno a un nucleo",
        "skip": "Vai al contenuto",
        "switch": ("en", "en/", "English"),
    },
    "en": {
        "dir": "en/", "root": "../",
        "title": "Heptazip: the same commands as 7-Zip, faster and more parallel",
        "desc": "Heptazip is a fork of 7-Zip 26.05: same commands, faster LZMA2 and gzip, parallel work, "
                "new methods for text and AI models. For Linux and Windows.",
        "locale": "en_US", "alt_locale": "it_IT",
        "img_alt": "Heptazip logo: a heptagon split into seven blades around a core",
        "skip": "Skip to content",
        "switch": ("it", "../", "Italiano"),
    },
}

MONO_B = '<span class="font-mono text-base">'
MONO_S = '<span class="font-mono text-sm">'

# (italiano, inglese, occorrenze attese nel sorgente). Applicate dalla piu' lunga alla piu' corta;
# i numeri restano con la virgola: translate() li porta al punto decimale.
EN = [
    ('aria-label="Principale"', 'aria-label="Main"', 1),
    ('aria-label="Heptazip, inizio pagina"', 'aria-label="Heptazip, top of page"', 1),
    ('>Funzioni</a>', '>Features</a>', 1),
    ('>Misure</a>', '>Benchmarks</a>', 1),
    ('>Limiti</a>', '>Limits</a>', 1),
    ('>Installazione</a>', '>Install</a>', 1),
    ('Fork di 7-Zip 26.05 · versione 26.05-m2', 'Fork of 7-Zip 26.05 · version 26.05-m2', 1),
    ('Stessi comandi di 7-Zip. Più veloce, più parallelo.', 'The same commands as 7-Zip. Faster, more parallel.', 1),
    ('Heptazip è un fork di 7-Zip per compilatori moderni. Il programma resta <span class="font-mono text-lg text-paper">7zz</span>, con gli stessi comandi e le stesse opzioni, e gli archivi .7z con i metodi standard si aprono con qualunque 7-Zip. In più lavora in parallelo dove 7-Zip non lo fa, e ha metodi nuovi per i testi e per i modelli AI.',
     'Heptazip is a fork of 7-Zip for modern compilers. The program is still <span class="font-mono text-lg text-paper">7zz</span>, with the same commands and options, and .7z archives made with the standard methods open with any 7-Zip. On top of that it works in parallel where 7-Zip does not, and adds new methods for text and for AI models.', 1),
    ('Scarica la 26.05-m2', 'Download 26.05-m2', 2),
    ('Esempio di comando:', 'Example command:', 1),
    ('7zz a archivio.7z cartella/', '7zz a archive.7z folder/', 2),
    ('sette lame · un nucleo', 'seven blades · one core', 1),
    (' volte</span>', ' times</span>', 3),
    ('Compressione LZMA2 al livello predefinito: 16,4 MB/s contro 9,4 di 7-Zip.',
     'LZMA2 compression at the default level: 16,4 MB/s against 9,4 for 7-Zip.', 1),
    ('Decompressione degli stessi archivi: 580 MB/s contro 221 di 7-Zip.',
     'Decompression of the same archives: 580 MB/s against 221 for 7-Zip.', 1),
    (f'gzip a {MONO_S}-mx5</span>: 105 MB/s contro 12,7 di 7-Zip, con un rapporto migliore.',
     f'gzip at {MONO_S}-mx5</span>: 105 MB/s against 12,7 for 7-Zip, with a better ratio.', 1),
    (f'Silesia con il metodo BWT a 72 MB/s. LZMA2 {MONO_S}-mx9</span> si ferma al 22,97%.',
     f'Silesia with the BWT method at 72 MB/s. LZMA2 {MONO_S}-mx9</span> stops at 22,97%.', 1),
    ('Silesia (212 MB), 20 thread, corsa del 5 novembre 2026. 7-Zip 26.05 è compilato con le sue routine assembly.',
     'Silesia (212 MB), 20 threads, run of 5 November 2026. 7-Zip 26.05 is built with its assembly routines.', 1),
    ('>Cosa aggiunge a 7-Zip</h2>', '>What it adds to 7-Zip</h2>', 1),
    ('Sette lavori sopra la base di 7-Zip. Nessuno cambia il modo di chiamare il programma.',
     'Seven pieces of work on top of 7-Zip. None of them changes how you call the program.', 1),
    ('>LZMA2 più veloce</h3>', '>Faster LZMA2</h3>', 1),
    (f'È il metodo predefinito. A {MONO_B}-mx5</span> comprime 1,7 volte più in fretta di 7-Zip e si decomprime 2,6 volte più in fretta, con un rapporto migliore. A {MONO_B}-mx1</span> e {MONO_B}-mx9</span> gli archivi sono identici a quelli di 7-Zip.',
     f'The default method. At {MONO_B}-mx5</span> it compresses 1,7 times faster than 7-Zip and decompresses 2,6 times faster, with a better ratio. At {MONO_B}-mx1</span> and {MONO_B}-mx9</span> the archives are identical to those of 7-Zip.', 1),
    ('>gzip e zip in parallelo</h3>', '>Parallel gzip and zip</h3>', 1),
    (f'Deflate a blocchi su tutti i core, compressi da libdeflate: oltre 8 volte più veloce di 7-Zip a {MONO_B}-mx5</span>, oltre 1 GB/s a {MONO_B}-mx1</span>. I file si aprono con qualunque gzip o unzip.',
     f'Deflate in blocks across all cores, compressed by libdeflate: over 8 times faster than 7-Zip at {MONO_B}-mx5</span>, over 1 GB/s at {MONO_B}-mx1</span>. The files open with any gzip or unzip.', 1),
    ('>Folder 7z in parallelo</h3>', '>Parallel 7z folders</h3>', 1),
    ('I folder dei .7z si comprimono e si estraggono insieme, con archivi identici byte per byte a quelli a un thread. Anche gli zip si estraggono su più file alla volta: 2 volte più veloce su Silesia.',
     'The folders of a .7z are compressed and extracted side by side, with archives byte-for-byte identical to the single-thread ones. Zip archives are also extracted several files at a time: 2 times faster on Silesia.', 1),
    ('>BWT per i testi</h3>', '>BWT for text</h3>', 1),
    (f'Burrows-Wheeler con libbsc, a blocchi paralleli, per testi, sorgenti e dati strutturati. Silesia al 22,41% a 72 MB/s, contro il 22,97% a 7,8 MB/s di LZMA2 {MONO_B}-mx9</span>. Sui binari resta meglio LZMA2.',
     f'Burrows-Wheeler with libbsc, in parallel blocks, for text, source code and structured data. Silesia at 22,41% at 72 MB/s, against 22,97% at 7,8 MB/s for LZMA2 {MONO_B}-mx9</span>. On binaries LZMA2 is still better.', 1),
    ('7zz a -m0=bwt testi.7z docs/', '7zz a -m0=bwt text.7z docs/', 1),
    ('>FLOAT per i modelli AI</h3>', '>FLOAT for AI models</h3>', 1),
    ('Per i pesi in .safetensors e .gguf: senza perdita e scelto in automatico. Qwen3-0.6B BF16 scende al 66,3% in 1 secondo; LZMA2 arriva al 70,2% in 90.',
     'For weights in .safetensors and .gguf: lossless and picked automatically. Qwen3-0.6B BF16 goes down to 66,3% in 1 second; LZMA2 reaches 70,2% in 90.', 1),
    ('7zz a modello.7z model.safetensors', '7zz a model.7z model.safetensors', 1),
    ('>zstd e brotli nel 7z</h3>', '>zstd and brotli in 7z</h3>', 1),
    (f'Due metodi in più nel formato 7z, compatibili con 7-Zip-zstd. brotli {MONO_B}-mx11</span> comprime meglio di zstd -19 e si decomprime a oltre 1 GB/s.',
     f'Two more methods in the 7z format, compatible with 7-Zip-zstd. brotli {MONO_B}-mx11</span> compresses better than zstd -19 and decompresses at over 1 GB/s.', 1),
    ('7zz a -m0=zstd -mx3 dati.7z dati/', '7zz a -m0=zstd -mx3 data.7z data/', 1),
    ('>Build moderno e fuzzing</h3>', '>Modern build and fuzzing</h3>', 1),
    ('CMake con GCC e Clang, test con CTest, sanitizer a ogni push, build ARM64. Campagne di fuzzing su 59 formati e sui codec, con oltre 20 bug corretti.',
     'CMake with GCC and Clang, tests with CTest, sanitizers on every push, ARM64 build. Fuzzing campaigns on 59 formats and on the codecs, with over 20 bugs fixed.', 1),
    ('Silesia · 212 MB · 20 thread', 'Silesia · 212 MB · 20 threads', 1),
    ('>Misure, con i concorrenti accanto</h2>', '>Benchmarks, with the competitors alongside</h2>', 1),
    ('Un solo file tar in memoria, due ripetizioni, ogni archivio verificato. Tutta la suite in una sola sessione a macchina scarica, il 5 novembre 2026. 7-Zip 26.05 è compilato con le sue routine assembly.',
     'A single tar file in memory, two repetitions, every archive verified. The whole suite in one session on an idle machine, on 5 November 2026. 7-Zip 26.05 is built with its assembly routines.', 1),
    ('Compressione al livello predefinito, MB/s', 'Compression at the default level, MB/s', 1),
    ('Decompressione degli stessi archivi, MB/s', 'Decompression of the same archives, MB/s', 1),
    ('Rapporto di compressione: Heptazip', 'Compression ratio: Heptazip', 1),
    ('Gli altri metodi, nella stessa corsa', 'The other methods, in the same run', 1),
    ('Rapporto di compressione, velocità di compressione e decompressione in MB/s e secondi di CPU in compressione, su Silesia con 20 thread. Rapporto più basso è meglio.',
     'Compression ratio, compression and decompression speed in MB/s and CPU seconds for compression, on Silesia with 20 threads. A lower ratio is better.', 1),
    ('>Strumento e profilo</th>', '>Tool and profile</th>', 1),
    ('>Rapporto</th>', '>Ratio</th>', 1),
    ('La tabella completa ha 102 righe, con memoria e tempi di CPU di ogni strumento: ',
     'The full table has 102 rows, with memory and CPU time for every tool (in Italian): ', 1),
    ('>Limiti</p>', '>Limits</p>', 1),
    ('>Dove non vince</h2>', '>Where it does not win</h2>', 1),
    ('Le stesse misure mostrano anche quello che costa. Meglio saperlo prima di scegliere.',
     'The same measurements also show what it costs. Better to know before you choose.', 1),
    (f'LZMA2 a {MONO_B}-mx5</span> usa più thread per lo stesso lavoro: 93 secondi di CPU su Silesia contro 58 di 7-Zip. xz -6 ne spende 103.',
     f'LZMA2 at {MONO_B}-mx5</span> uses more threads for the same work: 93 CPU seconds on Silesia against 58 for 7-Zip. xz -6 spends 103.', 1),
    ('>Memoria</h3>', '>Memory</h3>', 1),
    (f'A {MONO_B}-mx5</span> con 20 thread servono 1,4 GB, contro 0,9 GB di 7-Zip. Con meno thread la memoria scende in proporzione.',
     f'At {MONO_B}-mx5</span> with 20 threads it needs 1,4 GB, against 0,9 GB for 7-Zip. With fewer threads memory goes down in proportion.', 1),
    ('>Decompressione</h3>', '>Decompression</h3>', 1),
    (f'A {MONO_B}-mx5</span> xz -6 resta avanti, 635 MB/s contro 580. A {MONO_B}-mx1</span> e {MONO_B}-mx9</span> Heptazip e 7-Zip sono alla pari: il decoder è lo stesso.',
     f'At {MONO_B}-mx5</span> xz -6 stays ahead, 635 MB/s against 580. At {MONO_B}-mx1</span> and {MONO_B}-mx9</span> Heptazip and 7-Zip are level: the decoder is the same.', 1),
    ('>Compatibilità</h3>', '>Compatibility</h3>', 1),
    (f'Gli archivi con FLOAT e BWT si aprono solo con Heptazip; per FLOAT basta {MONO_B}-mf=off</span>. zstd e brotli richiedono 7-Zip-zstd. I metodi standard si aprono con qualunque 7-Zip.',
     f'Archives with FLOAT and BWT open only with Heptazip; {MONO_B}-mf=off</span> turns FLOAT off. zstd and brotli need 7-Zip-zstd. The standard methods open with any 7-Zip.', 1),
    ('>Installazione</h2>', '>Install</h2>', 1),
    ('Un solo eseguibile statico per x86-64, con le routine assembly.',
     'A single static executable for x86-64, with the assembly routines.', 1),
    ('Estrai lo zip in una cartella e avvia il file manager. Non serve installazione e convive con 7-Zip. Gli eseguibili non sono firmati: SmartScreen chiede conferma al primo avvio.',
     'Extract the zip into a folder and start the file manager. No installation is needed and it coexists with 7-Zip. The executables are not signed: SmartScreen asks for confirmation on first launch.', 1),
    ('>Dai sorgenti</h3>', '>From source</h3>', 1),
    ('Servono CMake 3.20, GCC 13 o Clang 18, o versioni successive.',
     'You need CMake 3.20, GCC 13 or Clang 18, or later versions.', 1),
    ('aria-label="Comandi per Linux"', 'aria-label="Commands for Linux"', 1),
    ('aria-label="File del pacchetto Windows"', 'aria-label="Files in the Windows package"', 1),
    ('aria-label="Comandi per compilare dai sorgenti"', 'aria-label="Commands to build from source"', 1),
    ('Verifica i pacchetti: sha256sum', 'Verify the packages: sha256sum', 1),
    ('Copyright (c) 2026 Mario Trombino. Il codice scritto per Heptazip è distribuito con la licenza BSD a 2 clausole. 7-Zip è di Igor Pavlov e mantiene le sue licenze: GNU LGPL, restrizione unRAR, parti BSD. Heptazip non è un prodotto ufficiale di 7-Zip.',
     'Copyright (c) 2026 Mario Trombino. The code written for Heptazip is distributed under the 2-clause BSD license. 7-Zip is by Igor Pavlov and keeps its licenses: GNU LGPL, unRAR restriction, BSD parts. Heptazip is not an official 7-Zip product.', 1),
    ('aria-label="Risorse"', 'aria-label="Resources"', 1),
    ('>Release</a>', '>Releases</a>', 1),
    (f'href="{REPO}/wiki"', f'href="{REPO}/wiki/Home-en"', 1),
    (f'<a href="{REPO}/blob/develop/docs/benchmark-mercato.md" class="inline-flex', f'<a href="{REPO}/wiki/Performance" class="inline-flex', 1),
]


def rep(s, a, b, n=1):
    """Sostituzione con il numero di occorrenze atteso."""
    if s.count(a) != n:
        raise SystemExit(f"build.py: attese {n} occorrenze, trovate {s.count(a)}: {a[:90]!r}")
    return s.replace(a, b)


def text_nodes(s, fn):
    """Applica fn al solo testo, lasciando intatti i tag."""
    return "".join(t if t.startswith("<") else fn(t) for t in re.split(r"(<[^>]+>)", s))


def translate(s):
    for it, en, n in sorted(EN, key=lambda t: -len(t[0])):
        s = rep(s, it, en, n)
    return text_nodes(s, lambda t: re.sub(r"(\d),(\d)", r"\1.\2", t))  # 16,4 -> 16.4


def finish(s, lang):
    m = META[lang]
    s = s.replace("@@ROOT@@", m["root"])
    # scelta della lingua, prima del pulsante GitHub
    code, href, label = m["switch"]
    link = (f'<a href="{href}" lang="{code}" hreflang="{code}" class="inline-flex min-h-11 items-center text-fog underline '
            f'underline-offset-4 hover:text-paper focus-visible:outline-2 focus-visible:outline-offset-4 '
            f'focus-visible:outline-core">{label}</a>\n')
    gh = f'<a href="{REPO}" class="inline-flex min-h-11 items-center border border-rule'
    s = rep(s, gh, link + gh)
    # niente a capo dentro i nomi e dentro le opzioni
    s = text_nodes(s, lambda t: re.sub(r"7-Zip(-zstd)?", lambda k: f'<span class="whitespace-nowrap">{k.group(0)}</span>', t))
    s = rep(s, "Qwen3-0.6B", '<span class="whitespace-nowrap">Qwen3-0.6B</span>')
    s = s.replace('class="font-mono text-base"', 'class="font-mono text-base whitespace-nowrap"')
    s = s.replace('<span class="font-mono text-sm">', '<span class="font-mono text-sm whitespace-nowrap">')
    return s


def head(lang, site):
    m = META[lang]
    root, url = m["root"], f"{site}/{m['dir']}"
    ld = {
        "@context": "https://schema.org", "@type": "SoftwareApplication",
        "name": "Heptazip", "description": m["desc"], "url": url,
        "image": f"{site}/heptazip-card.png",
        "applicationCategory": "UtilitiesApplication", "operatingSystem": "Linux, Windows",
        "softwareVersion": "26.05-m2", "inLanguage": lang,
        "downloadUrl": f"{REPO}/releases/latest",
        "license": f"{REPO}/blob/develop/LICENSE-Heptazip",
        "sameAs": [REPO],
        "isBasedOn": {"@type": "SoftwareApplication", "name": "7-Zip", "url": "https://7-zip.org/"},
        "author": {"@type": "Person", "name": "Mario Trombino"},
        "offers": {"@type": "Offer", "price": "0", "priceCurrency": "EUR"},
    }
    verify = (f'<meta name="google-site-verification" content="{GOOGLE_SITE_VERIFICATION}">\n'
              if GOOGLE_SITE_VERIFICATION else "")
    return f"""<!doctype html>
<html lang="{lang}" class="motion-safe:scroll-smooth">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{m["title"]}</title>
<meta name="description" content="{m["desc"]}">
<meta name="author" content="Mario Trombino">
<meta name="theme-color" content="#0F1D2E">
{verify}<link rel="canonical" href="{url}">
<link rel="alternate" hreflang="it" href="{site}/">
<link rel="alternate" hreflang="en" href="{site}/en/">
<link rel="alternate" hreflang="x-default" href="{site}/en/">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Heptazip">
<meta property="og:url" content="{url}">
<meta property="og:title" content="{m["title"]}">
<meta property="og:description" content="{m["desc"]}">
<meta property="og:image" content="{site}/heptazip-card.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta property="og:image:alt" content="{m["img_alt"]}">
<meta property="og:locale" content="{m["locale"]}">
<meta property="og:locale:alternate" content="{m["alt_locale"]}">
<meta name="twitter:card" content="summary_large_image">
<link rel="icon" href="{root}favicon.png" type="image/png">
<link rel="preload" href="{root}outfit.woff2" as="font" type="font/woff2" crossorigin>
<link rel="stylesheet" href="{root}style.css">
<script type="application/ld+json">{json.dumps(ld, ensure_ascii=False)}</script>
</head>
<body class="bg-ink">
<a href="#contenuto" class="sr-only focus:not-sr-only focus:fixed focus:left-4 focus:top-4 focus:z-50 focus:bg-core focus:px-5 focus:py-3 focus:font-medium focus:text-ink">{m["skip"]}</a>
"""


def sitemap(site):
    today = datetime.date.today().isoformat()
    alt = "".join(f'    <xhtml:link rel="alternate" hreflang="{h}" href="{site}/{d}"/>\n'
                  for h, d in (("it", ""), ("en", "en/"), ("x-default", "en/")))
    urls = "".join(f"  <url>\n    <loc>{site}/{d}</loc>\n    <lastmod>{today}</lastmod>\n{alt}  </url>\n" for d in ("", "en/"))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" '
            'xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' + urls + "</urlset>\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=HERE / "dist", help="cartella di uscita (default: site/dist)")
    ap.add_argument("--site-url", default=DEFAULT_URL, help="indirizzo pubblico, per canonical, hreflang e sitemap")
    ap.add_argument("--no-css", action="store_true", help="non compilare il foglio di stile con Tailwind")
    args = ap.parse_args()
    out, site = args.out, args.site_url.rstrip("/")

    if out.exists():
        shutil.rmtree(out)
    (out / "en").mkdir(parents=True)

    body = (HERE / "src" / "page.it.html").read_text(encoding="utf-8").strip()
    pages = {"it": body, "en": translate(body)}
    for lang, b in pages.items():
        page = head(lang, site) + finish(b, lang) + "\n</body>\n</html>\n"
        (out / META[lang]["dir"] / "index.html").write_text(page, encoding="utf-8")

    (out / "sitemap.xml").write_text(sitemap(site), encoding="utf-8")
    shutil.copy(HERE / "src" / "llms.txt", out / "llms.txt")
    for f in sorted((HERE / "static").iterdir()):
        shutil.copy(f, out / f.name)
    for src, dst in (("heptazip-logo-dark.svg", "heptazip-logo-dark.svg"),
                     ("heptazip-mark-dark.svg", "heptazip-mark-dark.svg"),
                     ("heptazip-mark-32.png", "favicon.png"),
                     ("Outfit-OFL.txt", "outfit-LICENSE.txt"),
                     ("IBMPlexMono-OFL.txt", "ibm-plex-mono-LICENSE.txt")):
        shutil.copy(LOGO / src, out / dst)

    if not args.no_css:
        # Tailwind legge le classi dai due index.html appena scritti (@source in src/input.css)
        subprocess.run(["npx", "--no-install", "@tailwindcss/cli", "-i", "src/input.css",
                        "-o", str(out.resolve() / "style.css"), "--minify"], cwd=HERE, check=True)
    print(f"sito scritto in {out} ({site}/)")


if __name__ == "__main__":
    main()
