<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/heptazip-logo-dark.svg">
    <img alt="Heptazip" src="docs/logo/heptazip-logo.svg" width="420">
  </picture>
</p>

🇮🇹 Italiano · 🇬🇧 [English](README.en.md)

# Heptazip

Heptazip è un fork di [7-Zip](https://7-zip.org) 26.05 (Igor Pavlov) per Linux e
compilatori moderni: più veloce in compressione e decompressione, parallelo dove 7-Zip
non lo è, e con un metodo nuovo per i modelli di intelligenza artificiale. Il programma
resta `7zz`, con gli stessi comandi e opzioni; gli archivi `.7z` con i metodi standard
si aprono con qualunque 7-Zip.

Il progetto si chiamava Tensor7 fino al 2 novembre 2026 (nome cambiato per non confonderlo
con una società omonima).

## Cosa aggiunge

- **Metodo FLOAT** per i pesi dei modelli AI, senza perdita e scelto in automatico per
  `.safetensors` e `.gguf`: BF16/FP16/FP32 con l'esponente in Huffman, GGUF quantizzati
  (Q2_K … Q8_0) con un modello a contesti e rANS. Esempi (`7zz a` senza opzioni):
  Qwen3-0.6B BF16 al 66,3% (LZMA2 70,2%) in 1 s invece di 90; DeepHat-7B Q4_K_M al
  93,2% (LZMA2 97,8%) 18 volte più veloce.
- **zstd** e **brotli** nel formato 7z (`-m0=zstd`, `-m0=brotli`), compatibili con
  7-Zip-zstd: brotli `-mx11` comprime meglio di zstd -19 e decomprime oltre 1 GB/s con
  più thread.
- **Metodo BWT** per testi e dati strutturati (`-m0=bwt`, libbsc: Burrows-Wheeler con
  libsais e coder QLFC, blocchi in parallelo): silesia al 22,41% a 72 MB/s con 20 thread,
  contro 22,97% a 7,6 MB/s di LZMA2 `-mx9`; webster 16,0% contro 20,2%. Solo Heptazip,
  come FLOAT; sui binari resta meglio LZMA2.
- **Estrazione e compressione parallele** dei folder 7z (`-mpfd`, `-mpfc`), con archivi
  identici byte per byte a quelli a un thread; **estrazione parallela degli zip** sui file
  (`-mpfd`): 2 volte più veloce su silesia, da 3 a 4 volte sugli zip AES, stessi risultati
  per file dell'estrazione a un thread.
- **gzip e zip in parallelo**: Deflate a blocchi su tutti i core, oltre 8 volte più veloce
  di 7-Zip (`-tgzip -mx5`: 105 MB/s, meglio di pigz -9 come rapporto), file leggibili da
  qualunque gzip/unzip e identici per ogni numero di thread. I blocchi sono compressi
  da libdeflate: `-mx3` comprime più di pigz -6 a 2,4 volte la velocità (790 MB/s),
  `-mx1` supera 1 GB/s. Decompressione Deflate 2,6-2,9 volte più veloce di 7-Zip (silesia
  770-800 MB/s, dal 3 al 15% sotto libdeflate; sul corpus più veloce di libdeflate).
- **LZMA2 più veloce** (il metodo di default): a `-mx5` silesia al 23,22% a 16,4 MB/s con
  decompressione a 580 MB/s, contro 23,40%, 9,4 e 221 MB/s di 7-Zip 26.05 compilato con le
  sue routine assembly; meglio di xz -6 in rapporto e compressione (23,35%, 13,3 MB/s) e al
  91% della sua decompressione (635). Il costo è in CPU: più thread per lo stesso lavoro
  (93 s contro 58 di 7-Zip, 103 di xz -6).
  Dal livello 5 dimensione parola 64 e blocchi pari al dizionario del livello, con un
  dizionario di 16 MiB per i blocchi da 32 MiB del livello 5 (1,4 GB di memoria su silesia
  a 20 thread, non 2,3); a `-mx1` e `-mx9` gli archivi sono identici a quelli di 7-Zip e si
  decomprimono alla stessa velocità (il decoder assembly è lo stesso; 1,4-1,5 volte un
  7-Zip compilato senza assembly). Con un solo blocco (file medi, `-mx9`, LZMA
  puro) il match finder lavora su più thread, con archivi identici: silesia `-mx9` da 42 a
  26 s, un file da 12 MB a `-mx5` da 2,9 a 1,7 s.
- **Più velocità**: assembly attivo di default, CRC32 con PCLMULQDQ, match finder
  ottimizzato, decoder SIMD (AVX2, NEON).
- **Build moderno** con CMake (GCC/Clang, C++20, LTO/PGO opzionali), test con CTest,
  sanitizer, fuzzing (libFuzzer) e build ARM64.

Gli archivi con FLOAT si estraggono solo con Heptazip: per la compatibilità con 7-Zip
basta `-mf=off`.

## Documentazione

- [Sito](https://mariotrombino76.github.io/heptazip/): presentazione del progetto, in italiano e in
  inglese (sorgenti in [site](site)).
- [Wiki](https://github.com/mariotrombino76/heptazip/wiki): installazione, guida rapida,
  quale metodo usare, prestazioni, compatibilità, domande frequenti.
- [BUILD-modern.md](BUILD-modern.md): compilare, testare, usare le opzioni nuove.
- [docs/design](docs/design): una nota per ogni scelta (formati, misure, alternative).
- [docs/benchmark-mercato.md](docs/benchmark-mercato.md): confronto con zstd, xz,
  brotli, pigz e altri.

## Versioni

`26.05-m3`: la base 7-Zip (26.05) e il rilascio di Heptazip su quella base. I pacchetti
(Linux x64, Windows x64) sono nelle [release](https://github.com/mariotrombino76/heptazip/releases),
le novità in [docs/release-notes](docs/release-notes).

Il pacchetto Windows richiede Windows 10 versione 1903 (maggio 2019) o successiva, oppure
Windows 11: la GUI (7zFM, 7zG) usa le API del tema scuro e del DPI per monitor di quelle versioni,
e Windows 7 e 8.1 non sono più supportati da Microsoft. Il pacchetto contiene la cartella `Lang`
con le traduzioni della GUI (le 92 lingue di 7-Zip; le stringhe nuove di Heptazip per ora solo in
italiano): 7zFM e 7zG usano da soli la lingua di Windows, si cambia da Strumenti > Opzioni > Lingua.
Vedi [Lang/README.md](Lang/README.md).

## Branch

| branch | contenuto |
|---|---|
| `develop` | sviluppo: base delle pull request, build e test (GitHub Actions) a ogni push |
| `modern` | linea stabile, aggiornata da `develop` a ogni release |
| `main` | 7-Zip 26.05 originale, come riferimento (non riceve il lavoro di Heptazip) |

Ogni modifica passa da una issue, un branch da `develop` e una pull request.

## Licenza e crediti

Heptazip: Copyright (c) 2026 Mario Trombino. Il codice scritto per Heptazip è distribuito
con la licenza BSD a 2 clausole: chi lo riusa deve mantenere l'avviso di copyright e il
testo della licenza. Dettagli, file per file: [LICENSE-Heptazip](LICENSE-Heptazip) (in inglese:
[LICENSE-Heptazip.en](LICENSE-Heptazip.en)).

7-Zip è di Igor Pavlov: il codice originale mantiene le sue licenze (GNU LGPL, con la
restrizione unRAR per il codice RAR, e le parti BSD; vedi `DOC/License.txt`). zstd è
distribuito con licenza BSD, il compressore di libdeflate e brotli con licenza MIT, libbsc
(metodo BWT) con licenza Apache-2.0. Heptazip non è un prodotto ufficiale di 7-Zip.
