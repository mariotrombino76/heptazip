# Benchmark di mercato

Il fork confrontato con gli altri strumenti di compressione: obiettivo del progetto è essere
il migliore strumento di compressione e decompressione (velocità, rapporto, memoria) e il
più efficiente per energia consumata (secondi di CPU per lo stesso lavoro).

## Come riprodurlo

```sh
tools/bench/build-competitors.sh      # concorrenti in ~/devtools/bin (versioni e SHA-256 fissati)
PATH=~/devtools/bin:$PATH tools/bench/market-bench.py --upstream <7zz di 7-Zip 26.05>
```

Strumenti (2026-10-31): 7zz del fork (`build/7zz`, Release, GCC 13), 7-Zip 26.05 originale
(branch `main`, `Alone2/makefile.gcc`), zstd 1.5.7, xz 5.8.4, brotli 1.2.1, lz4 1.10.0,
bzip3 1.5.5, plzip 1.13 (lzlib 1.16), pigz 2.8, gzip 1.12, bzip2 1.0.8. Tutti compilati con le
opzioni di release predefinite, senza `-march=native`. lbzip2 2.5 non compila con la glibc
attuale ed è escluso.

i7-12700H, 20 thread logici, WSL2; input a stream singolo in `/dev/shm`: `silesia.tar`
(202 MB) e il tar del corpus (`bench-data/corpus`, 110 MB, con un file casuale da 16 MiB);
2 ripetizioni (minimo); decompressione verso `/dev/null`, ogni archivio verificato.
Tutti gli strumenti multi-thread usano tutti i core.

## Risultati (2026-10-31)

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -m0=lzma2 -mx1             27.88%     189.1     651.3       18.8      4.23       87       42
silesia  7zz-modern  -m0=lzma2 -mx5             23.40%      10.3     169.1       57.1      2.17      907      256
silesia  7zz-modern  -m0=lzma2 -mx9             22.97%       5.2     109.9       62.6      2.04     2089      254
silesia  7zz-modern  -m0=zstd -mx3              31.24%    1103.8    1219.2        1.3      0.17      236        8
silesia  7zz-modern  -m0=zstd -mx19             24.93%      10.3    1296.3      107.6      0.16      824       14
silesia  7zz-26.05   -m0=lzma2 -mx1             27.88%     196.3     622.6       19.0      4.28       87       42
silesia  7zz-26.05   -m0=lzma2 -mx5             23.40%       9.5     166.1       60.2      2.20      907      256
silesia  7zz-26.05   -m0=lzma2 -mx9             22.97%       4.7     108.5       67.2      2.06     2089      255
silesia  zstd        -3 -T0 -q                  31.24%    1065.7    1156.1        1.5      0.20      159        6
silesia  zstd        -19 -T0 -q                 24.93%      10.3    1163.1      107.7      0.19      823       12
silesia  zstd        --ultra -22 -T0 -q         24.67%       2.3     879.7       95.0      0.25      970      133
silesia  xz          -1 -T0                     27.74%     114.3     945.5       28.8      3.37      270      167
silesia  xz          -6 -T0                     23.35%      13.6     657.6      104.7      2.22     1080      300
silesia  xz          -9 -T0                     23.01%       2.9     160.0       77.2      1.52     1079      322
silesia  brotli      -q 5                       28.15%      70.6     532.7        3.1      0.40      104       21
silesia  brotli      -q 11 -w 24                23.48%       0.7     489.2      304.0      0.44      289       20
silesia  lz4         -1 -q                      47.62%    1610.3    1925.9        0.5      0.12      139       34
silesia  pigz        -6 -p 20                   32.20%     330.5     313.7       12.2      0.75       16        2
silesia  pigz        -9 -p 20                   31.92%     152.4     328.6       26.2      0.75       16        2
silesia  gzip        -6                         32.19%      39.5     252.6        5.7      0.89        2        2
silesia  bzip2       -9                         25.76%      18.9      52.1       11.7      4.35        8        5
silesia  bzip3       -j 20                      22.29%      59.1      69.9       35.5     27.50     1719     1724
silesia  plzip       -6 -n20                    23.68%      16.8     438.2      126.6      4.48     1265      246
corpus   7zz-modern  -m0=lzma2 -mx1             64.23%     114.4    1690.0       18.7      0.50       99       49
corpus   7zz-modern  -m0=lzma2 -mx5             63.06%      17.1     379.4       12.0      0.31      381      183
corpus   7zz-modern  -m0=lzma2 -mx9             63.02%      17.2     344.1       12.2      0.32     1117      183
corpus   7zz-modern  -m0=zstd -mx3              64.84%    1366.4    3035.8        0.2      0.02      183        8
corpus   7zz-modern  -m0=zstd -mx19             63.32%      14.8    2966.4       22.8      0.03      506       14
corpus   7zz-26.05   -m0=lzma2 -mx1             64.23%     114.1    1495.3       18.6      0.50       98       49
corpus   7zz-26.05   -m0=lzma2 -mx5             63.06%      16.1     332.2       12.4      0.33      381      183
corpus   7zz-26.05   -m0=lzma2 -mx9             63.02%      16.5     348.6       12.6      0.34     1117      183
corpus   zstd        -3 -T0 -q                  64.84%    1655.0    2668.6        0.3      0.04      182        7
corpus   zstd        -19 -T0 -q                 63.32%      14.0    2462.8       22.5      0.04      505       13
corpus   zstd        --ultra -22 -T0 -q         63.29%       5.8    1583.3       20.7      0.07      887      112
corpus   xz          -1 -T0                     64.01%      50.5    1540.8       37.5      0.35      292      101
corpus   xz          -6 -T0                     63.05%      12.9     706.0       32.9      0.31      627      186
corpus   xz          -9 -T0                     63.03%       3.3     438.3       35.8      0.32      852      243
corpus   brotli      -q 5                       64.08%     179.3    1261.4        0.7      0.09       60       20
corpus   brotli      -q 11 -w 24                63.07%       1.0    1157.2      118.2      0.09      219       19
corpus   lz4         -1 -q                      70.02%    1469.1    2286.3        0.2      0.06      108       34
corpus   pigz        -6 -p 20                   66.37%     451.2     600.0        4.6      0.23       17        2
corpus   pigz        -9 -p 20                   66.32%     303.1     617.0        6.6      0.23       17        2
corpus   gzip        -6                         66.37%      53.1     293.5        2.2      0.39        2        2
corpus   bzip2       -9                         64.66%      19.3      34.5        6.1      3.40        8        5
corpus   bzip3       -j 20                      63.78%      30.0      32.4       17.0     15.34     1514     1513
corpus   plzip       -6 -n20                    63.89%      15.2     120.9       40.9      4.30      653      114

load 1m alla fine: 2.43
```

## Lettura

**Dove il fork è già il migliore**

- **Energia a parità di rapporto**: LZMA2 `-mx5` (23,40%) usa 57 s di CPU contro 105 di
  xz `-6` (23,35%) e 108 di zstd `-19` (24,93%, rapporto peggiore).
- **Rapporto alto**: LZMA2 `-mx9` (22,97%) batte xz `-9` (23,01%) ed è 1,8 volte più veloce
  in compressione (5,2 contro 2,9 MB/s) con meno CPU.
- **zstd nel 7z**: velocità e rapporto uguali alla CLI di riferimento.
- Rispetto a 7-Zip 26.05: compressione LZMA2 dall'8 al 10% più veloce (design 0004),
  archivi identici.

**Dove perde**

1. **Decompressione di un file grande compresso con LZMA2**: xz 5.8 decomprime
   `-6` a 658 MB/s, il fork `-mx5` a 169 MB/s (3,9 volte meno). Cause: il decoder C (la
   versione assembly non era attiva) e i blocchi indipendenti di LZMA2, 4 volte il
   dizionario (128 MiB a `-mx5`, dizionario di 32 MiB su 64 bit): su 202 MB solo 2
   blocchi, quindi pochi core in decompressione e in compressione (xz `-6`: blocchi da
   24 MiB, dizionario di 8 MiB). Lo stesso limite rende xz più veloce in tempo reale
   anche in compressione (13,6 contro 10,3 MB/s), pur consumando il doppio della CPU.
2. **Rapporto sui testi**: bzip3 (BWT) arriva al 22,29%, il 3% meglio di LZMA2 `-mx9`, ma
   decomprime a 70 MB/s con 1,7 GB di memoria e 27 s di CPU.

Priorità che ne seguono: politica dei blocchi LZMA2 per file grandi (punto 1), poi una
valutazione di un metodo BWT per i testi (punto 2).

## Blocchi LZMA2: compromesso misurato

`tools/bench/block-sweep.py`, Silesia (202 MiB), 1 ripetizione, build con il decoder
assembly. Su 64 bit `-mx5` usa un dizionario di 32 MiB e `-mx9` di 256 MiB.

| `-mx5` (dizionario 32 MiB) | 128 MiB (4×, upstream) | 96 MiB (3×) | **64 MiB (2×)** | 48 MiB | 32 MiB |
|---|---|---|---|---|---|
| Dimensione | 0 | +0,01% | **+0,06%** | +0,22% | +0,72% |
| Decompressione 20 thread (MiB/s) | 205 | 270 | **364** | 437 | 551 |
| Compressione (MiB/s) | 9,8 | 9,5 | **11,6** | 13,2 | 17,0 |

| `-mx9` (dizionario 256 MiB) | 256 MiB | 192 MiB | 128 MiB | 96 MiB | 64 MiB |
|---|---|---|---|---|---|
| Dimensione | 0 | +0,04% | +0,13% | +0,16% | +0,21% |
| Decompressione 20 thread (MiB/s) | 147 | 156 | 220 | 276 | 347 |
| Compressione (MiB/s) | 5,0 | 5,2 | 8,4 | 8,7 | 10,6 |

**Scelta (2026-11-01): blocchi da 2 volte il dizionario** (`C/Lzma2Enc.h`,
`Lzma2Enc_GetAutoBlockSize()`, unica definizione usata dall'encoder, anche per xz, e dal
handler 7z). A `-mx5`: +0,06% di dimensione, decompressione dei file grandi 1,77 volte e
compressione 1,18 volte più veloci. Il blocco non è mai più piccolo del dizionario: a
`-mx9` resta di 256 MiB, perché blocchi più piccoli taglierebbero i match a lunga
distanza, la ragione di un dizionario grande (su dati con ripetizioni lontane la perdita
può essere grande: design 0001, §1.2). La dimensione dei blocchi solid del 7z non cambia.
Chi vuole più velocità: `-m0=lzma2:c=48m` (o meno).

Correzione: la prima versione di questa sezione indicava per `-mx5` un dizionario di 16
MiB (blocchi da 64 MiB); il valore reale su 64 bit è 32 MiB (`LZMA2:25`).

## Aggiornamento: decoder assembly attivo di default (2026-11-01)

Con `SEVENZIP_ASM` attivo quando c'è un assembler (e `LzFindOpt` in C) la decompressione
LZMA del fork migliora del ~40%, a parità di archivi:

| Input | Profilo | dec MB/s prima | dec MB/s ora | xz 5.8.4 (stesso livello) |
|---|---|---|---|---|
| Silesia | `-mx1` | 651 | **985** | 946 (`-1`) |
| Silesia | `-mx5` | 169 | **239** | 658 (`-6`, blocchi più piccoli) |
| Silesia | `-mx9` | 110 | **156** | 160 (`-9`) |
| Corpus | `-mx5` | 379 | **428** | 706 (`-6`) |

Il divario residuo a `-mx5` dipende dalla dimensione dei blocchi LZMA2 (sezione
precedente, decisione aperta).

## Aggiornamento: blocchi LZMA2 da 2 volte il dizionario (2026-11-01)

| Silesia | Rapporto | comp MB/s | dec MB/s | comp RSS |
|---|---|---|---|---|
| fork `-mx5` prima (C, blocchi 4×) | 23,40% | 10,3 | 169 | 907 MB |
| fork `-mx5` ora (asm, blocchi 2×) | 23,42% | **11,8** | **354** | 1353 MB |
| xz 5.8.4 `-6 -T0` | 23,35% | 13,2 | 630 | 1080 MB |

Il divario in decompressione con xz `-6` è sceso da 3,9 a 1,8 volte; il fork usa ancora
poco più della metà della CPU di xz in compressione (62 contro 106 s).

## Pesi dei modelli AI: metodo FLOAT (2026-11-01)

Qwen3-0.6B da Hugging Face, `model.safetensors` (1503 MB, tutto BF16). Stessa macchina,
20 thread, 2 ripetizioni:

```sh
PATH=~/devtools/bin:$PATH tools/bench/market-bench.py -r 2 \
  --inputs ~/devtools/models/qwen3-0.6b/model.safetensors --only '7zz-modern|zstd|xz|plzip|pigz|lz4|brotli -q 5'
```

| Strumento | Rapporto | Compr. MB/s | Decompr. MB/s | CPU compr. s | CPU decompr. s | RSS compr. MB |
|---|---|---|---|---|---|---|
| **7zz-modern `-m0=FLOAT`** | **66,58%** | **1521** | **2081** | **4,4** | **3,7** | 72 |
| 7zz-modern LZMA2 `-mx1` | 74,16% | 98 | 487 | 304 | 61 | 92 |
| 7zz-modern LZMA2 `-mx5` | 70,24% | 17 | 264 | 1252 | 79 | 4440 |
| 7zz-modern LZMA2 `-mx9` | 70,02% | 7,3 | 162 | 1514 | 51 | 15480 |
| 7zz-modern zstd `-mx3` | 78,12% | 1836 | 946 | 3,2 | 1,7 | 232 |
| 7zz-modern zstd `-mx19` | 75,48% | 14 | 549 | 1882 | 3,0 | 2896 |
| zstd -3 -T0 | 78,12% | 2276 | 1122 | 4,9 | 1,6 | 187 |
| zstd -19 -T0 | 75,48% | 12 | 479 | 1211 | 3,5 | 1523 |
| zstd --ultra -22 -T0 | 74,21% | 2,3 | 354 | 1834 | 4,9 | 4489 |
| xz -1 -T0 | 73,49% | 43 | 449 | 690 | 65 | 298 |
| xz -6 -T0 | 70,65% | 15 | 349 | 1960 | 81 | 2887 |
| xz -9 -T0 | 70,06% | 3,6 | 185 | 1794 | 61 | 6196 |
| plzip -6 | 70,72% | 14 | 260 | 1957 | 106 | 2509 |
| brotli -q 5 | 78,09% | 133 | 249 | 12 | 6,3 | 40 |
| pigz -9 | 79,51% | 153 | 195 | 196 | 9,0 | 16 |
| lz4 -1 | 99,86% | 2023 | 3079 | 1,5 | 0,8 | 209 |

FLOAT è il più piccolo di tutti e tra i più veloci:

- contro il migliore degli altri per rapporto (LZMA2 `-mx9` e xz -9, 70,0%) è più
  piccolo del 5%, comprime 200-400 volte più in fretta con 350-400 volte meno CPU e
  decomprime 11-13 volte più in fretta;
- contro zstd -3, alla stessa velocità, è più piccolo del 15%.

Il confronto con ZipNN 0.5.4 (PyPI), lo strumento di riferimento della letteratura per
questo caso, non è stato possibile. Il suo rapporto (66,2-66,6%) è uguale a FLOAT, ma la
decompressione **non restituisce i dati originali**: su questo modello, su porzioni
allineate e perfino su 1 MiB di byte 0..255, con tutte le combinazioni di
`method`/`byte_reorder`/`reorder_signbit` provate (Python 3.12, x86-64).

Con un solo thread FLOAT comprime a 620 MB/s e decomprime a 750 MB/s (CRC compreso), con
7 MB di memoria. Dettagli: `docs/design/0008-metodo-float.md`.

### FP16 e FP32, scelta automatica (2026-11-01)

Con il modo zstd per i blocchi non float (maschere di attenzione) e la scelta automatica
per `.safetensors`/`.gguf` (`7zz a archivio.7z model.safetensors`, senza opzioni):

| Modello | Tipi | 7zz (automatico) | LZMA2 `-mx5` | xz -9 -T0 | zstd -19 -T0 |
|---|---|---|---|---|---|
| Qwen3-0.6B (1503 MB) | BF16 | **66,28%**, 1,0 s | 70,24%, 88 s | 70,06% | 75,48% |
| pythia-160m (375 MB) | FP16 + maschere U8 | **71,84%**, 0,3 s | 76,13%, 11 s | 75,98% | 78,35% |
| gpt2 (548 MB) | FP32 | **76,60%**, 0,4 s | 83,34%, 14 s | 82,62% | 84,31% |

### Modelli quantizzati GGUF (2026-11-01)

Blocchi quantizzati del metodo FLOAT (design 0008 §3.6), scelti in automatico per i GGUF
Q4_0, Q8_0, Q4_K_M/S, Q5_K_M/S, Q6_K. `7zz a` senza opzioni, 20 thread:

| Modello | 7zz (FLOAT) | LZMA2 `-mx5` | zstd `-19 -T0` | xz `-9 -T0` |
|---|---|---|---|---|
| qwen3-4b Q4_K_M (2,5 GB) | **95,65%**, 2,6 s / 1,4 s | 97,83%, 55 s / 5,9 s | 97,61%, 106 s | 97,88% (`-6`), 91 s |
| DeepHat-V1-7B Q4_K_M (4,7 GB) | **95,48%**, 5,8 s / 2,6 s | 97,81%, 106 s / 10,7 s | – | – |
| Qwen3-0.6B Q4_K_M | **95,26%** | 97,16% | 96,82% | 97,13% |
| Qwen3-0.6B Q5_K_M | **96,27%** | 98,55% | 98,42% | 98,55% |
| Qwen3-0.6B Q6_K | **95,87%** | 98,66% | 98,34% | 98,67% |
| Qwen3-0.6B Q8_0 | **93,74%** | 96,02% | 95,17% | 95,98% |

(tempi: compressione / decompressione). Più piccolo di 1,4-2,8 punti del migliore dei
concorrenti, e più veloce di tutti in entrambe le direzioni.
