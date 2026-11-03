# 0014 — Brotli nei `.7z`

| | |
|---|---|
| **Stato** | Implementato (issue #18), attivo di default (`SEVENZIP_BROTLI`) |
| **Data** | 2026-11-02 |
| **Fase** | 3: nuovi algoritmi |
| **Codice** | `CPP/7zip/Compress/BrotliCoder.{h,cpp}`, `BrotliRegister.cpp`, `third_party/brotli` (1.2.1, MIT), `cmake/SevenZipBrotli.cmake`, makefile GCC e nmake |

---

## 1. Perché

Silesia, un core: brotli -q 11 dà il 23,48% con decompressione a ~480 MB/s, cioè meglio di
zstd -19 come rapporto (24,93%) e 3 volte più veloce di LZMA in decompressione
(22,96%, ~150 MB/s). La Fase 0 del nuovo algoritmo (design 0013) ha mostrato che brotli
occupa già il punto "buon rapporto, decompressione veloce": conviene offrirlo.

## 2. Formato: quello di 7-Zip-zstd

Per avere archivi leggibili in entrambe le direzioni si usa il formato di 7-Zip-zstd
(come per zstd, design 0002):

- **ID del metodo** `0x4F71102`, nome `BROTLI` (`-m0=brotli`);
- **proprietà**: 3 byte, versione maggiore e minore di brotli e livello (informative);
- **flusso**: una sequenza di frame indipendenti (brotli-mt); ogni frame è un frame
  "skippable" di zstd di 16 byte (`LE32 0x184D2A50`, `LE32 8`, `LE32` dimensione
  compressa, `LE16 0x5242` "BR", `LE16` numero di blocchi da 64 KiB del frame
  decompresso) seguito da un flusso brotli standard.

La dimensione dei frame dipende solo dal livello: l'uscita è la stessa con qualsiasi
numero di thread, e frame indipendenti si comprimono **e si decomprimono** in parallelo.
Il decoder accetta anche un flusso brotli senza frame (il `.br` a un thread).

| livello (`-mx`) | frame | note |
|---|---|---|
| 0..9 | livello × 1 MiB (0: 1 MiB) | come 7-Zip-zstd |
| 10, 11 | 32 MiB | q11: 23,90% con 11 MiB, **23,66%** con 32, 23,55% con 64, 23,48% un solo flusso |

La finestra è di 24 bit (16 MiB), come 7-Zip-zstd, ridotta per i frame piccoli (meno
memoria al decoder; il formato non cambia).

Memoria dell'encoder: brotli q10-q11 usa ~230-270 MB per thread (misurato): il numero
di thread è limitato perché gli encoder non superino metà della RAM.

## 3. Risultati

Silesia (212 MB), i7-12700H, 20 thread:

| `-m0=brotli` | rapporto | compressione | decompressione 1 thread | 20 thread |
|---|---|---|---|---|
| `-mx1` | 34,86% | 1695 MB/s | 375 MB/s | 2340 MB/s |
| `-mx5` | 28,28% | 221 MB/s | 518 MB/s | 1937 MB/s |
| `-mx9` | 26,97% | 20 MB/s | 522 MB/s | 1520 MB/s |
| `-mx11` | **23,66%** | 2,7 MB/s | 445 MB/s | 1227 MB/s |

Per confronto: LZMA2 `-mx9` 22,97% con decompressione ~150 MB/s per core; zstd -19
24,93%; brotli -q 11 (un flusso, un thread) 23,48% a 0,7 MB/s in compressione.

I livelli 9-11 di brotli sono limitati dalla banda di memoria con molti thread (q9:
oltre 4 thread non scala; la stessa cosa con 20 processi `brotli` separati).

## 4. Verifiche

- I frame del `.7z` estratti e decodificati dal programma `brotli` ufficiale danno i
  dati originali (silesia, 41 frame).
- `tests/brotli_codec.py` (CTest `brotli_codec`): dimensioni ai confini dei frame,
  livelli 0, 1, 3, 10; flusso identico con `-mmt1` e `-mmt4`; decompressione parallela;
  un byte corrotto dà un errore.
- `tests/roundtrip_smoke.py` (anche su Windows, MinGW e MSVC): `-m0=brotli -mx5 -mmt4`.
- `tests/interop_7zzstd.py` (job della CI): brotli (livelli 1, 5, 11) e zstd (3, 19)
  creati da Heptazip ed estratti da 7-Zip-zstd e viceversa, con la release ufficiale
  di 7-Zip-zstd per Linux (tag e SHA-256 fissati).
- Fuzzing del decoder (`fuzz_codec`, metodo `BROTLI`).
