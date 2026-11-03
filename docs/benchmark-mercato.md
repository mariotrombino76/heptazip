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

## Deflate e BZip2 nei loro formati (2026-11-02)

`market-bench.py --only 'gzip|bzip2|pigz|libdeflate'`, con il Deflate a blocchi in
parallelo (design 0009). libdeflate e lbzip2 non sono disponibili.

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                33.15%     512.0     305.1        7.5      0.70       80        6
silesia  7zz-modern  -tgzip -mx5                30.99%     104.9     320.3       37.8      0.67       94        6
silesia  7zz-modern  -tgzip -mx9                30.56%      15.3     313.2      263.8      0.68      110        6
silesia  7zz-modern  -tbzip2 -mx5               25.76%      84.8     107.9       45.2      2.93      206       11
silesia  7zz-modern  -tbzip2 -mx9               25.58%      12.8     101.3      310.2      2.91      217       11
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.2     298.1       17.6      0.72        7        6
silesia  7zz-26.05   -tbzip2 -mx5               25.76%      78.9     108.4       49.1      2.91      206       11
silesia  pigz        -6 -p 20                   32.20%     339.8     319.0       12.3      0.76       16        2
silesia  pigz        -9 -p 20                   31.92%     151.8     311.3       25.8      0.78       16        2
silesia  gzip        -6                         32.19%      37.9     245.6        5.7      0.87        2        2
silesia  bzip2       -9                         25.76%      18.4      49.5       11.7      4.35        8        5
corpus   7zz-modern  -tgzip -mx1                66.61%     493.1     825.5        4.1      0.13      100        6
corpus   7zz-modern  -tgzip -mx5                66.09%     205.0     870.3        9.0      0.12      110        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      29.8     742.7        3.9      0.14        7        6
corpus   pigz        -6 -p 20                   66.37%     436.5     550.8        4.6      0.23       17        2
corpus   pigz        -9 -p 20                   66.32%     298.3     537.4        6.7      0.24       17        2
corpus   bzip2       -9                         64.66%      18.8      34.0        6.1      3.39        8        5
```

- **gzip/zip**: Deflate era a un thread (12 MB/s). Ora `-mx5` comprime a 105 MB/s
  (8,6×) con il rapporto migliore della tabella (0,9 punti meglio di pigz -9), `-mx1` a
  512 MB/s (più di pigz -6, rapporto peggiore). Il decoder è come pigz e più veloce di
  gzip. Manca un livello intermedio che batta pigz -6 su entrambi gli assi (design 0009,
  §8; fatto nel design 0010, sezione seguente).
- **bzip2**: già parallelo in 7-Zip. `-mx5` (predefinito, blocchi da 900k come
  `bzip2 -9`, stesso file) è 4,6 volte più veloce in compressione e 2,2 in
  decompressione di bzip2. `-mx9` fa 7 passate: 7 volte la CPU per lo 0,7%.

## Deflate: livelli veloci con lazy matching (2026-11-02)

`market-bench.py --only 'gzip|pigz'`, con `-mx1`..`-mx4` in lazy matching (design 0010).

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                32.98%     529.3     300.2        7.1      0.71       81        6
silesia  7zz-modern  -tgzip -mx3                32.14%     362.3     310.4       10.7      0.69       80        6
silesia  7zz-modern  -tgzip -mx4                31.92%     272.4     314.4       14.2      0.68       80        6
silesia  7zz-modern  -tgzip -mx5                30.99%     108.6     318.7       37.2      0.68       94        6
silesia  7zz-modern  -tgzip -mx9                30.56%      15.4     314.9      262.2      0.68      109        6
silesia  7zz-26.05   -tgzip -mx1                33.15%      74.1     283.1        2.9      0.76        7        6
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.3     299.5       17.7      0.72        7        6
silesia  pigz        -6 -p 20                   32.20%     332.5     317.7       12.3      0.77       16        2
silesia  pigz        -9 -p 20                   31.92%     151.3     315.6       26.1      0.77       16        2
silesia  gzip        -6                         32.19%      37.8     241.9        5.8      0.89        2        2
corpus   7zz-modern  -tgzip -mx1                66.52%     474.4     833.9        4.1      0.13      100        6
corpus   7zz-modern  -tgzip -mx3                66.36%     424.8     865.7        4.6      0.13      100        6
corpus   7zz-modern  -tgzip -mx4                66.32%     370.9     868.7        5.2      0.13      100        6
corpus   7zz-modern  -tgzip -mx5                66.09%     217.5     863.0        9.0      0.13      109        6
corpus   7zz-modern  -tgzip -mx9                66.02%      29.4     857.8       70.1      0.12      123        6
corpus   7zz-26.05   -tgzip -mx1                66.61%      80.3     712.4        1.4      0.15        7        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      30.2     749.8        3.9      0.14        7        6
corpus   pigz        -6 -p 20                   66.37%     447.2     556.6        4.7      0.25       17        2
corpus   pigz        -9 -p 20                   66.32%     297.7     583.3        6.7      0.23       17        2
corpus   gzip        -6                         66.37%      52.4     290.7        2.2      0.40        2        2
```

- **silesia**: `-mx3` batte pigz -6 su entrambi gli assi (32,14% a 362 MB/s contro
  32,20% a 333), `-mx4` ha il rapporto di pigz -9 a 1,8 volte la velocità, `-mx1` è
  veloce come prima con un rapporto migliore (32,98% invece di 33,15%).
- **corpus**: stessi rapporti di pigz, `-mx4` più veloce di pigz -9; `-mx3` il 6% più
  lento di pigz -6 (con meno CPU): il limite qui è la pipeline a blocchi su un input di
  110 MB, non la ricerca dei match.

## Deflate contro libdeflate (2026-11-02)

libdeflate dal ramo master (`0758d0a`, dopo la 1.26), compilato in `~/devtools/bin`;
`libdeflate-gzip` usa un thread e tiene il file in memoria (RSS).

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                32.98%     510.4     306.0        6.8      0.70       80        6
silesia  7zz-modern  -tgzip -mx3                32.14%     361.2     317.0       10.9      0.68       80        6
silesia  7zz-modern  -tgzip -mx4                31.92%     273.2     318.3       14.2      0.67       81        6
silesia  7zz-modern  -tgzip -mx5                30.99%     108.4     317.7       37.2      0.68       94        6
silesia  7zz-modern  -tgzip -mx9                30.56%      15.4     314.1      262.4      0.68      109        6
silesia  pigz        -6 -p 20                   32.20%     323.2     320.6       12.0      0.77       16        2
silesia  pigz        -9 -p 20                   31.92%     149.0     322.5       26.0      0.75       16        2
silesia  gzip        -6                         32.19%      37.6     242.5        5.8      0.89        2        2
silesia  libdeflate  -1                         34.68%     256.1     837.2        0.8      0.25      274      273
silesia  libdeflate  -6                         31.85%     136.8     857.8        1.6      0.25      268      268
silesia  libdeflate  -9                         31.48%      69.0     852.3        3.1      0.24      268      267
silesia  libdeflate  -12                        30.52%       8.1     869.6       26.8      0.24      273      265
corpus   7zz-modern  -tgzip -mx1                66.52%     493.5     853.3        4.1      0.13      100        6
corpus   7zz-modern  -tgzip -mx3                66.36%     423.9     864.7        4.6      0.13      100        6
corpus   7zz-modern  -tgzip -mx4                66.32%     384.5     869.3        5.2      0.13      100        6
corpus   7zz-modern  -tgzip -mx5                66.09%     221.6     870.6        9.0      0.12      109        6
corpus   7zz-modern  -tgzip -mx9                66.02%      29.1     808.6       70.0      0.13      122        6
corpus   pigz        -6 -p 20                   66.37%     462.8     588.7        4.5      0.23       16        2
corpus   pigz        -9 -p 20                   66.32%     304.1     579.9        6.7      0.24       17        2
corpus   gzip        -6                         66.37%      52.0     287.2        2.2      0.40        2        2
corpus   libdeflate  -1                         66.96%     191.0    1567.9        0.6      0.06      182      182
corpus   libdeflate  -6                         66.26%     131.7    1581.8        0.9      0.07      182      181
corpus   libdeflate  -9                         66.20%     106.8    1596.6        1.1      0.07      182      181
corpus   libdeflate  -12                        65.99%      24.7    1528.0        4.7      0.06      187      181
```

- **Compressione**: nel tempo reale vince Heptazip con 20 thread, ma per core libdeflate
  è circa 7-10 volte più efficiente: `-6` dà 31,85% con 1,6 s di CPU, meglio di
  Heptazip `-mx4` (31,92% con 14,2 s); `-12` dà 30,52% con 26,8 s, come Heptazip `-mx9`
  (30,56% con 262 s).
- **Decompressione**: libdeflate 840-870 MB/s su silesia e 1,5-1,6 GB/s sul corpus,
  2,7 e 1,8 volte il decoder Deflate di 7-Zip (306-318 e 860 MB/s).
- Il parallelismo a blocchi (design 0009) e un motore efficiente come libdeflate si
  sommano: è il margine più grande rimasto su gzip/zip.

## Decoder Deflate veloce (2026-11-02)

Con il decoder del design 0011 (decompressione a un thread, colonna `dec MB/s`):

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                32.98%     537.3     780.5        7.1      0.27       80        7
silesia  7zz-modern  -tgzip -mx3                32.14%     346.2     785.6       10.8      0.27       80        7
silesia  7zz-modern  -tgzip -mx5                30.99%     107.0     800.0       37.4      0.26       94        7
silesia  7zz-26.05   -tgzip -mx1                33.15%      73.9     279.6        2.9      0.76        7        6
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.4     298.5       17.9      0.73        7        6
silesia  pigz        -6 -p 20                   32.20%     333.4     325.7       12.2      0.76       16        2
silesia  pigz        -9 -p 20                   31.92%     151.1     328.3       25.8      0.77       16        2
silesia  gzip        -6                         32.19%      37.9     235.7        5.8      0.88        2        2
silesia  libdeflate  -6                         31.85%     135.9     866.3        1.6      0.23      268      268
silesia  libdeflate  -9                         31.48%      70.0     860.1        3.1      0.24      268      267
corpus   7zz-modern  -tgzip -mx1                66.52%     478.7    2096.3        3.9      0.05      100        7
corpus   7zz-modern  -tgzip -mx3                66.36%     443.4    2172.9        4.7      0.04      100        7
corpus   7zz-modern  -tgzip -mx5                66.09%     222.5    2227.7        8.8      0.04      110        7
corpus   7zz-26.05   -tgzip -mx1                66.61%      82.4     729.4        1.5      0.15        7        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      30.0     734.9        3.9      0.15        7        6
corpus   pigz        -6 -p 20                   66.37%     436.8     564.8        4.6      0.23       17        2
corpus   pigz        -9 -p 20                   66.32%     298.9     574.7        6.7      0.23       17        2
corpus   gzip        -6                         66.37%      50.7     282.0        2.2      0.39        2        2
corpus   libdeflate  -6                         66.26%     129.7    1537.7        0.9      0.07      182      181
corpus   libdeflate  -9                         66.20%     106.7    1588.8        1.1      0.06      182      181
```

- **Decompressione gzip**: da 280-300 a 780-800 MB/s su silesia (2,7 volte; libdeflate
  860, pigz 326) e da 730 a 2100-2230 MB/s sul corpus (3 volte, più di libdeflate: 1540-1590).
- Insieme alla compressione parallela (design 0009, 0010), gzip e zip di Heptazip sono
  davanti a pigz su entrambi gli assi a ogni livello confrontabile, e a libdeflate nel
  tempo reale di compressione; libdeflate resta più efficiente per core in compressione.

## Deflate con libdeflate nei blocchi paralleli (2026-11-02)

Design 0012: i blocchi da 1 MiB del Deflate parallelo sono compressi da libdeflate
(modificato per la storia e i blocchi non finali), al livello con il rapporto migliore
dell'encoder di 7-Zip a ogni `-mx`.

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                32.78%    1035.7     796.0        3.3      0.26       66        7
silesia  7zz-modern  -tgzip -mx3                31.85%     819.8     810.3        4.6      0.26       70        7
silesia  7zz-modern  -tgzip -mx4                31.68%     683.1     807.9        5.6      0.26       70        7
silesia  7zz-modern  -tgzip -mx5                30.61%     103.8     803.6       38.1      0.26      204        7
silesia  7zz-modern  -tgzip -mx9                30.53%      53.7     801.1       71.4      0.26      210        8
silesia  7zz-26.05   -tgzip -mx1                33.15%      75.1     284.7        2.9      0.76        7        6
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.3     300.6       17.7      0.72        7        6
silesia  pigz        -6 -p 20                   32.20%     331.9     322.8       12.2      0.75       16        2
silesia  pigz        -9 -p 20                   31.92%     152.1     318.0       26.0      0.78       16        2
silesia  gzip        -6                         32.19%      38.0     245.1        5.8      0.88        2        2
silesia  libdeflate  -1                         34.68%     262.3     841.1        0.8      0.25      274      273
silesia  libdeflate  -6                         31.85%     138.8     806.1        1.6      0.26      268      268
silesia  libdeflate  -9                         31.48%      70.6     880.1        3.1      0.24      268      267
silesia  libdeflate  -12                        30.52%       8.2     864.6       26.7      0.24      273      265
corpus   7zz-modern  -tgzip -mx1                66.47%     895.7    2029.5        2.0      0.05       75        7
corpus   7zz-modern  -tgzip -mx3                66.26%     774.0    2039.1        2.4      0.05       80        7
corpus   7zz-modern  -tgzip -mx4                66.23%     724.9    2028.7        2.6      0.05       79        8
corpus   7zz-modern  -tgzip -mx5                66.01%     225.7    2058.8        8.2      0.05      165        7
corpus   7zz-modern  -tgzip -mx9                66.00%     143.0    2109.7       11.7      0.04      166        7
corpus   7zz-26.05   -tgzip -mx1                66.61%      80.8     670.9        1.4      0.16        7        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      30.2     735.4        3.9      0.15        7        6
corpus   pigz        -6 -p 20                   66.37%     458.0     580.4        4.7      0.24       16        2
corpus   pigz        -9 -p 20                   66.32%     297.6     556.1        6.7      0.24       17        2
corpus   gzip        -6                         66.37%      52.2     285.2        2.2      0.40        2        2
corpus   libdeflate  -1                         66.96%     189.6    1513.5        0.6      0.07      182      182
corpus   libdeflate  -6                         66.26%     131.5    1507.5        0.9      0.06      182      181
corpus   libdeflate  -9                         66.20%     107.1    1517.4        1.1      0.07      182      181
corpus   libdeflate  -12                        65.99%      24.8    1559.6        4.7      0.07      187      181
```

- Ogni livello comprime meglio di prima ed è più veloce o uguale: `-mx1` 1036 MB/s,
  `-mx3` 820 MB/s al 31,85% (pigz -6: 332 MB/s al 32,20%), `-mx5` 30,61% (prima 30,99%),
  `-mx9` 3,6 volte più veloce.
- Decompressione (design 0011): 800 MB/s su silesia, 2 GB/s sul corpus.

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

Blocchi quantizzati del metodo FLOAT (design 0008 §3.6-3.7: modello a contesti con
rANS), scelti in automatico per i GGUF Q2_K, Q3_K_S/M/L, Q4_0, Q8_0, Q4_K_M/S, Q5_K_M/S,
Q6_K. `7zz a` senza opzioni, 20 thread:

| Modello | 7zz (FLOAT) | LZMA2 `-mx5` | zstd `-19 -T0` | xz `-9 -T0` |
|---|---|---|---|---|
| qwen3-4b Q4_K_M (2,5 GB) | **93,69%**, 3,5 s / 1,6 s | 97,83%, 55 s / 5,9 s | 97,61%, 106 s | 97,88% (`-6`), 91 s |
| DeepHat-V1-7B Q4_K_M (4,7 GB) | **93,21%**, 5,95 s / 2,7 s | 97,81%, 106 s / 10,7 s | – | – |
| Qwen3-0.6B Q4_K_M | **93,57%** | 97,16% | 96,82% | 97,13% |
| Qwen3-0.6B Q5_K_M | **94,63%** | 98,55% | 98,42% | 98,55% |
| Qwen3-0.6B Q6_K | **95,19%** | 98,66% | 98,34% | 98,67% |
| Qwen3-0.6B Q8_0 | **93,65%** | 96,02% | 95,17% | 95,98% |
| Qwen3-0.6B Q4_0 | **90,80%** | – | – | – |
| Qwen3-0.6B Q3_K_M | **94,59%** | 97,75% | 97,53% | 97,75% |
| Qwen3-0.6B Q2_K | **93,46%** | 96,68% | 96,39% | 96,68% |

(tempi: compressione / decompressione). Più piccolo di 1,4-4,6 punti del migliore dei
concorrenti, e più veloce di tutti in entrambe le direzioni. Il modello a contesti
(mode 4) vale 1,7-2,3 punti sui Q4_K/Q5_K rispetto a Huffman (mode 3: qwen3-4b era al
95,65%, DeepHat al 95,48%), 0,7 sui Q6_K.

## Stato rispetto al mercato (2026-11-03)

Stessa procedura e stessi strumenti del 2026-10-31 (xz 5.8.4, zstd 1.5.7, brotli 1.2.1,
libdeflate, pigz 2.8, bzip2, bzip3, plzip), più `libdeflate-gzip` e il metodo brotli nel 7z
(design 0014); 7zz del fork alla fine della revisione del codice (develop `9b270fd`), 7-Zip
26.05 compilato da `main`. 2 ripetizioni, minimo.

```
CPU logiche: 20   load 1m: 1.52   ripetizioni: 2
assenti: lbzip2

input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -m0=lzma2 -mx1             27.88%     194.7     858.6       17.9      3.15       87       42
silesia  7zz-modern  -m0=lzma2 -mx5             23.42%      11.8     358.2       60.7      1.59     1354      256
silesia  7zz-modern  -m0=lzma2 -mx9             22.97%       5.0     149.5       62.2      1.40     2089      255
silesia  7zz-modern  -m0=zstd -mx3              31.24%    1112.2    1186.7        1.2      0.17      238        8
silesia  7zz-modern  -m0=zstd -mx19             24.93%      10.2    1241.2      105.9      0.16      824       14
silesia  7zz-modern  -m0=brotli -mx5            28.28%     231.1    1989.5       12.9      1.02      685      308
silesia  7zz-modern  -m0=brotli -mx11           23.66%       2.8    1118.8      400.8      0.85     1554      285
silesia  7zz-modern  -m0=FLOAT                  32.38%    1353.8    4035.2        2.1      0.27      117       70
silesia  7zz-modern  -tgzip -mx1                32.78%    1023.7     774.3        3.4      0.26       66        7
silesia  7zz-modern  -tgzip -mx3                31.85%     812.2     779.1        4.4      0.26       69        7
silesia  7zz-modern  -tgzip -mx4                31.68%     677.7     786.3        5.2      0.26       70        7
silesia  7zz-modern  -tgzip -mx5                30.61%     104.7     785.6       37.5      0.26      204        7
silesia  7zz-modern  -tgzip -mx9                30.53%      53.2     769.2       71.3      0.26      210        7
silesia  7zz-modern  -tbzip2 -mx5               25.76%      82.9     103.3       45.4      3.00      206       11
silesia  7zz-modern  -tbzip2 -mx9               25.58%      12.5      98.9      313.3      2.94      217       11
silesia  7zz-26.05   -m0=lzma2 -mx1             27.88%     188.8     603.7       18.7      4.20       87       41
silesia  7zz-26.05   -m0=lzma2 -mx5             23.40%       9.2     158.6       59.9      2.20      907      256
silesia  7zz-26.05   -m0=lzma2 -mx9             22.97%       4.5     103.9       66.3      2.03     2089      254
silesia  7zz-26.05   -tgzip -mx1                33.15%      73.4     276.9        2.9      0.76        7        6
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.0     293.4       17.6      0.71        7        6
silesia  7zz-26.05   -tbzip2 -mx5               25.76%      77.4     106.1       49.0      2.94      206       11
silesia  zstd        -3 -T0 -q                  31.24%    1133.6    1168.3        1.4      0.18      156        6
silesia  zstd        -19 -T0 -q                 24.93%      10.1    1139.4      105.3      0.19      823       12
silesia  zstd        --ultra -22 -T0 -q         24.67%       2.3     886.6       93.9      0.24      970      133
silesia  xz          -1 -T0                     27.74%     112.0     893.6       28.6      3.40      271      167
silesia  xz          -6 -T0                     23.35%      13.3     594.9      103.4      2.28     1081      299
silesia  xz          -9 -T0                     23.01%       2.9     156.4       76.1      1.50     1079      322
silesia  brotli      -q 5                       28.15%      69.1     518.9        3.1      0.39      103       20
silesia  brotli      -q 11 -w 24                23.48%       0.7     480.9      301.1      0.42      289       20
silesia  lz4         -1 -q                      47.62%    1578.9    1920.3        0.5      0.12      123       34
silesia  pigz        -6 -p 20                   32.20%     333.6     315.6       12.1      0.77       16        2
silesia  pigz        -9 -p 20                   31.92%     149.9     317.5       25.8      0.74       16        2
silesia  gzip        -6                         32.19%      37.2     239.9        5.7      0.87        2        2
silesia  libdeflate  -1                         34.68%     256.4     853.2        0.8      0.23      274      273
silesia  libdeflate  -6                         31.85%     135.6     868.1        1.6      0.23      268      268
silesia  libdeflate  -9                         31.48%      70.7     842.3        3.0      0.24      268      267
silesia  libdeflate  -12                        30.52%       8.1     890.4       26.2      0.23      273      265
silesia  bzip2       -9                         25.76%      18.4      49.4       11.5      4.26        8        5
silesia  bzip3       -j 20                      22.29%      57.7      70.0       35.3     26.80     1719     1725
silesia  plzip       -6 -n20                    23.68%      16.0     419.3      126.3      4.52     1261      245
corpus   7zz-modern  -m0=lzma2 -mx1             64.23%     113.7    2000.5       18.5      0.36       98       49
corpus   7zz-modern  -m0=lzma2 -mx5             63.06%      22.0     649.1       12.3      0.25      834      183
corpus   7zz-modern  -m0=lzma2 -mx9             63.02%      16.6     448.5       12.0      0.24     1117      183
corpus   7zz-modern  -m0=zstd -mx3              64.84%    1390.8    3208.1        0.2      0.02      179        8
corpus   7zz-modern  -m0=zstd -mx19             63.32%      14.0    2878.8       22.6      0.03      506       14
corpus   7zz-modern  -m0=brotli -mx5            64.13%     505.2    2033.5        1.4      0.21      395      219
corpus   7zz-modern  -m0=brotli -mx11           63.06%       2.2    1231.7      116.2      0.20      694      211
corpus   7zz-modern  -m0=FLOAT                  65.15%    1582.2    3116.1        0.3      0.07      123       80
corpus   7zz-modern  -tgzip -mx1                66.47%     897.9    1995.6        1.9      0.05       75        7
corpus   7zz-modern  -tgzip -mx3                66.26%     757.7    2122.0        2.3      0.05       79        7
corpus   7zz-modern  -tgzip -mx4                66.23%     722.3    2120.9        2.3      0.04       79        7
corpus   7zz-modern  -tgzip -mx5                66.01%     226.3    2109.8        8.2      0.04      166        7
corpus   7zz-modern  -tgzip -mx9                66.00%     142.8    2012.2       11.5      0.04      166        7
corpus   7zz-modern  -tbzip2 -mx5               64.67%      95.5      72.9       20.2      2.54      218       11
corpus   7zz-modern  -tbzip2 -mx9               64.48%      10.8      73.8      193.4      2.52      234       11
corpus   7zz-26.05   -m0=lzma2 -mx1             64.23%     112.2    1416.2       18.8      0.51       99       49
corpus   7zz-26.05   -m0=lzma2 -mx5             63.06%      15.6     334.2       12.3      0.33      381      183
corpus   7zz-26.05   -m0=lzma2 -mx9             63.02%      15.6     332.9       12.5      0.33     1117      183
corpus   7zz-26.05   -tgzip -mx1                66.61%      77.8     692.3        1.4      0.15        7        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      29.4     726.2        3.8      0.14        7        6
corpus   7zz-26.05   -tbzip2 -mx5               64.67%      90.8      74.5       20.9      2.47      218       11
corpus   zstd        -3 -T0 -q                  64.84%    1520.5    2533.7        0.3      0.05      181        6
corpus   zstd        -19 -T0 -q                 63.32%      14.4    2641.1       21.9      0.04      505       13
corpus   zstd        --ultra -22 -T0 -q         63.29%       5.6    1473.7       20.4      0.08      887      113
corpus   xz          -1 -T0                     64.01%      49.0    1551.2       38.0      0.34      293      110
corpus   xz          -6 -T0                     63.05%      12.2     720.4       32.6      0.30      627      186
corpus   xz          -9 -T0                     63.03%       3.2     403.6       36.0      0.32      852      243
corpus   brotli      -q 5                       64.08%     162.7    1160.4        0.7      0.09       60       19
corpus   brotli      -q 11 -w 24                63.07%       1.0    1119.6      116.7      0.09      219       19
corpus   lz4         -1 -q                      70.02%    1389.7    2217.6        0.2      0.05      124       34
corpus   pigz        -6 -p 20                   66.37%     443.9     568.5        4.6      0.24       17        2
corpus   pigz        -9 -p 20                   66.32%     289.1     572.3        6.7      0.23       17        2
corpus   gzip        -6                         66.37%      51.1     282.4        2.2      0.39        2        2
corpus   libdeflate  -1                         66.96%     188.3    1591.7        0.6      0.06      182      182
corpus   libdeflate  -6                         66.26%     129.3    1545.4        0.9      0.06      182      181
corpus   libdeflate  -9                         66.20%     105.5    1619.6        1.1      0.06      182      181
corpus   libdeflate  -12                        65.99%      24.3    1471.2        4.7      0.07      187      181
corpus   bzip2       -9                         64.66%      18.8      33.5        6.0      3.37        8        5
corpus   bzip3       -j 20                      63.78%      28.8      31.4       16.8     15.22     1514     1513
corpus   plzip       -6 -n20                    63.89%      14.6     116.8       41.1      4.28      653      114

load 1m alla fine: 3.03
```

### Lettura

**Rispetto a 7-Zip 26.05** (stessi archivi `.7z`, byte per byte): compressione LZMA2 dall'8 al
28% più veloce (`-mx5`: 11,8 contro 9,2 MB/s); decompressione da 1,4 a 2,3 volte più veloce
(`-mx1` 859 contro 604, `-mx5` 358 contro 159, `-mx9` 150 contro 104 MB/s). Formato gzip:
compressione 14 volte più veloce a `-mx1` (1024 contro 73 MB/s) e 8,7 volte a `-mx5`, con
rapporto migliore; decompressione 2,7 volte. BZip2 invariato.

**Dove il fork è il migliore**

- **gzip**: a parità di rapporto è il produttore di gzip più veloce misurato. `-mx1` 32,78% a
  1024 MB/s contro pigz `-6` 32,20% a 334 MB/s; `-mx3` 31,85% a 812 MB/s, lo stesso rapporto di
  libdeflate `-6` a 6 volte la velocità; `-mx5` 30,61% a 105 MB/s batte il rapporto di pigz
  `-9` (31,92%) e si avvicina a libdeflate `-12` (30,52%, 8 MB/s). Decompressione 770-790 MB/s
  su un thread contro 316 di pigz e 240 di gzip; libdeflate resta davanti del 10% (850-890).
- **brotli**: lo stesso rapporto della CLI a 3,3 volte la velocità in compressione e 3,8 in
  decompressione (`-mx5`: 231 e 1990 MB/s contro 69 e 519), grazie ai frame in parallelo;
  `-mx11` 23,66% batte zstd `-19` (24,93%) e decomprime a 1119 MB/s.
- **LZMA2 ad alto rapporto**: `-mx9` 22,97% contro xz `-9` 23,01%, compressione 1,7 volte più
  veloce (5,0 contro 2,9 MB/s), decompressione uguale (150 contro 156), meno CPU (62 contro 76 s).
- **Energia a parità di rapporto**: `-mx5` 23,42% con 61 s di CPU contro 103 di xz `-6`
  (23,35%) e 105 di zstd `-19` (24,93%).
- **zstd nel 7z**: alla pari con la CLI (1112 contro 1134 MB/s, 1187 contro 1168).
- **Modelli AI**: FLOAT non ha concorrenti (vedi la sezione dedicata); sui dati normali è
  quasi una copia (1354 MB/s, 4035 in decompressione).
- **Corpus di file misti**: davanti su tutti gli assi; LZMA2 `-mx5` comprime a 22 MB/s contro
  12 di xz `-6` con lo stesso rapporto.

**Dove perde ancora**

1. **Decompressione LZMA2 di un file grande ai livelli medi**: xz `-6` 595 MB/s contro
   `-mx5` 358 (1,7 volte; era 3,9 volte il 2026-10-31). A `-mx1` e `-mx9` è alla pari. Il
   limite è il numero di blocchi indipendenti (2 volte il dizionario) e il decoder a thread
   singolo per blocco; xz usa blocchi da 24 MiB con dizionario da 8 MiB.
2. **Rapporto massimo sui testi**: bzip3 22,29% contro 22,97%, ma a 70 MB/s in decompressione
   con 1,7 GB di memoria e 27 s di CPU: un metodo BWT resta una valutazione aperta.
3. **Decoder Deflate**: 10% sotto libdeflate (dec 786 contro 868 MB/s).
4. **Memoria in compressione LZMA2 `-mx5`**: 1354 MB contro 907 di 7-Zip (più blocchi in
   parallelo) e 1081 di xz `-6`.
5. **zstd oltre il livello 19**: `--ultra -22` (24,67%) non è esposto nel 7z.

## LZMA2 `-mx5`: fb 64 e blocchi pari al dizionario (2026-11-03, #64)

Il punto 1 della lettura precedente. Lo scaling della decompressione di Silesia a `-mx5`
(4 blocchi da 64 MiB): 1 thread 148 MB/s, 2 → 202, 4 → 340, 8 → 345, 20 → 330: sopra i 4
blocchi non c'è più nulla da parallelizzare, e il sovraccarico fisso (avvio, lettura,
memoria, CRC, ~0,15 s) è simile a quello di xz. xz `-6` ottiene un rapporto migliore
(23,35%) con dizionario da 8 MiB e blocchi da 24 MiB perché usa nice_len 64, dove `-mx5`
usa fb 32. Sweep (`-m0=lzma2`, 20 thread, Silesia):

| profilo | rapporto | comp MB/s | dec MB/s | RSS comp MB |
|---|---|---|---|---|
| d=32m fb=32 c=64m (prima) | 23,42% | 10,2 | 331 | 1353 |
| d=32m fb=64 c=64m | 23,03% | 9,1 | 338 | – |
| d=32m fb=64 c=48m | 23,07% | 10,1 | 438 | – |
| **d=32m fb=64 c=32m** | **23,19%** | **14,0** | **524** | 2329 |
| d=16m fb=64 c=32m | 23,22% | 13,8 | 536 | – |
| d=8m fb=64 c=24m (come xz -6) | 23,33% | 18,3 | 606 | – |
| d=64m fb=64 c=128m (2×) | 23,00% | 7,4 | 207 | 1418 |
| d=64m fb=64 c=64m (1×) | 23,02% | 10,2 | 351 | 2120 |

fb 64 vale 0,39 punti di rapporto per l'8% di velocità di compressione; spesi in blocchi
pari al dizionario, a `-mx5` restano 0,22 punti di guadagno con compressione e
decompressione molto più veloci. Con dizionario da 64 MiB il blocco di un dizionario costa
+0,015%. **Decisione: `fb = 64` dal livello 5 (`LzmaEncProps_Normalize`, anche per LZMA e
xz) e blocco automatico pari al dizionario (`Lzma2Enc_GetAutoBlockSize`, era 2×).** A
`-mx9` (dizionario 256 MiB) il blocco resta 256 MiB. Il dialogo Aggiungi mostra 64 come
dimensione parola automatica dal livello 5.

Suite di mercato dopo la modifica (stessa procedura del 2026-11-03):

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
silesia  7zz-modern  -m0=lzma2 -mx1             27.88%     196.5     933.2       18.1      2.99       88       42
silesia  7zz-modern  -m0=lzma2 -mx5             23.19%      15.5     559.8       95.4      1.94     2329      256
silesia  7zz-modern  -m0=lzma2 -mx9             22.97%       5.0     148.3       62.5      1.42     2089      255
corpus   7zz-modern  -m0=lzma2 -mx1             64.23%     115.1    1924.7       18.1      0.36      100       49
corpus   7zz-modern  -m0=lzma2 -mx5             63.03%      32.5     752.7       14.2      0.26     1299      183
corpus   7zz-modern  -m0=lzma2 -mx9             63.02%      16.5     439.9       12.1      0.25     1117      184
```

`-mx5` su Silesia: rapporto 23,42% → **23,19%**, compressione 11,8 → **15,5 MB/s**,
decompressione 358 → **560 MB/s**; contro xz `-6` (23,35%, 13,3 e 595 MB/s, 103 s di CPU)
ora vince su rapporto e compressione ed è al 94% in decompressione. Corpus: 63,06% →
63,03%, 22,0 → 32,5 MB/s, 649 → 753 MB/s (xz `-6`: 720). Costi: la CPU di compressione su
Silesia sale da 61 a 95 s (fb 64 e più thread su E-core e SMT di questa macchina; xz `-6`
103 s) e la memoria in compressione dei file medi sale per i blocchi in parallelo (Silesia
2,3 GB invece di 1,35; sui file grandi non cambia: il costo per thread è lo stesso).
Gli archivi restano leggibili da ogni 7-Zip.


## Memoria di compressione LZMA2 `-mx5` (2026-11-03, #73 e #75)

Il punto 4 della lettura del 2026-11-03, peggiorato dalla #64: Silesia a `-mx5` passa da
1354 a 2329 MB. Misure (`/usr/bin/time`, RSS massimo, Silesia, binario di develop):

| thread | encoder di blocco in volo | RSS | tempo |
|---|---|---|---|
| `-mmt2` | 1 | 381 MB | 41,1 s |
| `-mmt4` | 2 | 746 MB | 22,4 s |
| `-mmt8` | 4 | 1478 MB | 18,1 s |
| `-mmt20` | 7 (= blocchi: 212 MB / 32 MiB) | 2330 MB | 13,9 s |
| 7-Zip 26.05 `-mmt20` | 2 (blocchi da 128 MiB = 4 × dizionario) | 907 MB | 24,4 s |

La memoria è lineare negli encoder in volo: **~365 MB ciascuno**, di cui 320 sono le tabelle
bt4 del dizionario da 32 MiB (albero binario 8 byte per posizione = 256 MB, hash 2^24 voci =
64 MB) e 32 il blocco in ingresso (il buffer d'uscita, 34 MB allocati, ne tocca ~7). Il
dizionario è lo stesso di 7-Zip (`LZMA2:25` in `l -slt` per entrambi): per encoder il fork
costa meno di upstream (365 contro ~450 MB con blocchi da 128 MiB), ma ne fa girare 7 invece
di 2, ed è questo che compra 15,5 MB/s contro 9,2 e 560 contro 159 MB/s in decompressione.
xz -6 (1081 MB) fa 9 blocchi da 24 MiB con dizionario da 8 MiB: ~120 MB per encoder.

Il limite di memoria di 7-Zip (`-mmemuse`) funziona già sui blocchi LZMA2: `-mmemuse=1g` →
2 encoder, 746 MB, 22,2 s; `-mmemuse=1500m` → 3 encoder, 1118 MB, 18,4 s (stima del handler
~20% sopra il reale). Con `-mmt` esplicito il limite non interviene, come in upstream.
Il valore predefinito era l'80% della RAM; xz usa il 25% per i thread automatici.
**Decisione: metà della RAM per i thread automatici** (`HandlerOut.h`), un compromesso fra
i due che su macchine da 8 GB in su non cambia nulla e su una da 4 GB tiene `-mx5` a 5
encoder (~1,9 GB) invece di 7.

Alternative misurate a 20 thread (stessa sessione, valori relativi: la macchina era più
lenta del benchmark di mercato):

| | Silesia rapporto | comp MB/s | CPU s | RSS comp | dec MB/s | corpus rapporto | comp | RSS comp |
|---|---|---|---|---|---|---|---|---|
| d=32m c=32m (fino a #74) | 23,19% | 14,4 | 95,8 | 2329 | 349 | 63,93% | 28,6 | 1394 |
| d=16m c=16m | 23,37% | 19,5 | 108,1 | 1866 | 421 | 63,94% | 32,6 | 1408 |
| **d=16m c=32m (ora, #75)** | **23,22%** | 14,4 | 99,1 | **1367** | 355 | **63,93%** | 27,6 | **944** |
| d=8m c=24m (come xz -6) | 23,33% | 20,1 | 104,5 | 1025 | 396 | 63,98% | 29,2 | 722 |

Dentro un blocco da 32 MiB la storia disponibile è in media 16 MiB: il dizionario da 16 MiB
perde solo i match a distanza 16-32 MiB nella seconda metà del blocco, 0,03 punti su Silesia
e nulla sul corpus, con la stessa velocità e il 32-41% di memoria in meno.

**Decisione (issue #75, 2026-11-03): al livello 5 i blocchi LZMA2 automatici usano un
dizionario pari a metà del blocco, 16 MiB con blocchi da 32 MiB.** La regola è una sola,
`Lzma2Enc_GetAutoBlockDictSize()` in `C/Lzma2Enc.h`, e vale quando il dizionario non è
dato, i blocchi sono automatici e i dati superano un blocco; la applicano l'encoder 7z
(`Lzma2EncProps_Normalize`), quello xz (`XzProps_Normalize`), le stime di memoria dei
handler (`Get_Lzma2_BlockDicSize`) e il dialogo Aggiungi. Non cambiano: LZMA puro, LZMA2 a
un thread di blocco (`-mmt1`, o dati entro 32 MiB: dizionario 32 MiB o pari ai dati),
`d=` esplicito, gli altri livelli (`-mx1` e `-mx9` identici a 7-Zip, verificati byte per
byte). Verifica con la build finale, 20 thread: Silesia 49 210 571 byte (23,22%), archivio
identico a quello di `-m0=lzma2:d=16m:c=32m` della build precedente, 13,4 s, RSS 1400 MB;
corpus 71 149 090 (63,93%), RSS 967 MB (era 1394); decompressione invariata (0,52 s, RSS
263 MB, dominata dai buffer dei blocchi). `-txz -mx5`: 7 blocchi da 32 MiB con
`dict=16MiB`, a `-mmt1` un blocco con 32 MiB. `-mmemuse=1g` ora tiene 3 encoder (858 MB,
17,0 s) invece di 2, perché la stima del handler usa il dizionario dei blocchi.
