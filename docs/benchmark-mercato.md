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
   `-6` a 658 MB/s, il fork `-mx5` a 169 MB/s (3,9 volte meno). Causa: i blocchi
   indipendenti di LZMA2 sono 4 volte il dizionario (64 MiB a `-mx5`): su 202 MB solo 4
   blocchi, quindi quasi un solo core in decompressione e pochi in compressione (xz `-6`:
   blocchi da 24 MiB). Lo stesso limite rende xz più veloce in tempo reale anche in
   compressione (13,6 contro 10,3 MB/s), pur consumando il doppio della CPU.
2. **Rapporto sui testi**: bzip3 (BWT) arriva al 22,29%, il 3% meglio di LZMA2 `-mx9`, ma
   decomprime a 70 MB/s con 1,7 GB di memoria e 27 s di CPU.

Priorità che ne seguono: politica dei blocchi LZMA2 per file grandi (punto 1), poi una
valutazione di un metodo BWT per i testi (punto 2).

## Blocchi LZMA2: compromesso misurato (decisione aperta)

`tools/bench/block-sweep.py`, Silesia (202 MiB), 1 ripetizione:

| `-mx5` (dizionario 16 MiB) | auto (64 MiB) | 48 MiB | 32 MiB | 24 MiB | 16 MiB |
|---|---|---|---|---|---|
| Dimensione | 0 | +0,22% | +0,72% | +0,93% | +1,43% |
| Decompressione 20 thread (MiB/s) | 160 | 316 | 380 | 435 | 486 |
| Compressione (MiB/s) | 9,6 | 12,6 | 16,8 | 19,9 | 22,4 |

| `-mx9` (dizionario 64 MiB) | auto (256 MiB) | 48 MiB | 32 MiB | 24 MiB | 16 MiB |
|---|---|---|---|---|---|
| Dimensione | 0 | +0,41% | +0,97% | +1,22% | +1,73% |
| Decompressione 20 thread (MiB/s) | 101 | 313 | 385 | 402 | 511 |
| Compressione (MiB/s) | 4,9 | 11,4 | 15,0 | 18,3 | 19,9 |

Ogni blocco riparte con il dizionario vuoto: blocchi più piccoli danno più parallelismo
(compressione e decompressione di un file grande) a scapito del rapporto. Il default non
è cambiato: CLAUDE.md chiede di mantenere o migliorare il rapporto. Opzione già
disponibile per l'utente: `-m0=lzma2:c=48m`. Proposta da decidere: blocchi da 3 volte il
dizionario (la regola di xz) invece di 4, cioè +0,22% a `-mx5` per una decompressione 2
volte più veloce sui file grandi.
