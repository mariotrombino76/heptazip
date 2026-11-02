# 0012 — Deflate: libdeflate come motore dei blocchi paralleli

| | |
|---|---|
| **Stato** | Implementato, attivo di default (gzip, zip, 7z con Deflate; non Deflate64) |
| **Data** | 2026-11-02 |
| **Fase** | 3: nuovi algoritmi |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`); libdeflate master `0758d0a` |
| **File coinvolti** | `third_party/libdeflate/`, `cmake/SevenZipLibdeflate.cmake`, `CPP/7zip/Compress/DeflateEncoder.{h,cpp}` |

---

## 1. Problema

Dopo i design 0009 (blocchi paralleli) e 0010 (lazy matching) Tensor7 batteva pigz nel
tempo reale, ma per core l'encoder di 7-Zip restava molto meno efficiente di libdeflate
(silesia, un thread, CPU in secondi):

| libdeflate | rapporto | CPU | | 7-Zip (Tensor7 0010) | rapporto | CPU |
|---|---|---|---|---|---|---|
| L4 | 32,78% | 1,2 | | `-mx1` | 32,98% | 2,7 |
| L6 | 31,85% | 1,5 | | `-mx3` | 32,14% | 4,2 |
| L7 | 31,68% | 1,9 | | `-mx4` | 31,92% | 5,7 |
| L10 | 30,57% | 15,1 | | `-mx5` | 30,99% | 16,3 |
| L12 | 30,52% | 26,8 | | `-mx9` | 30,56% | ~150 |

## 2. Soluzione

I blocchi da 1 MiB del design 0009 sono compressi da libdeflate. Lo schema non cambia:
ogni blocco riceve gli ultimi 32 KiB del precedente come storia, i blocchi non finali
finiscono con un blocco stored vuoto, l'uscita è un solo stream Deflate ed è la stessa per
ogni `-mmt`.

libdeflate comprime un buffer intero in uno stream completo: due modifiche (in
`third_party/libdeflate`, marcate `Tensor7`, elenco in `README-Tensor7.md`):

- **storia**: `libdeflate_deflate_compress_chunk()` riceve la storia prima dei dati; le
  quattro strategie (fastest, greedy, lazy, near-optimal) la inseriscono nel match finder
  con le funzioni di skip della libreria, senza codificarla;
- **blocco non finale**: nessun blocco ha BFINAL, e alla fine un blocco stored vuoto.

Il commit `072f3d3` contiene i file originali, così le modifiche sono un diff leggibile
(146 righe).

### 2.1 Livelli

Per ogni `-mx` il livello di libdeflate con un rapporto migliore di quello di prima
(silesia, blocchi da 1 MiB con la storia):

| `-mx` | libdeflate | rapporto | prima |
|---|---|---|---|
| 1 | L4 | 32,78% | 32,98% |
| 2 | L5 | 32,17% | 32,46% |
| 3 | L6 | 31,85% | 32,14% |
| 4 | L7 | 31,68% | 31,92% |
| 5, 6 | L10 | 30,61% | 30,99% |
| 7, 8 | L11 | 30,54% | |
| 9 | L12 | 30,53% | 30,56% |

L1 (34,68%) non è usato: il rapporto sarebbe peggiore di `-mx1` di 7-Zip 26.05 (33,15%).
L8 e L9 (31,5%, 2,7-3,1 s) stanno tra `-mx4` e `-mx5`, ma `-mx5` deve restare sotto il
30,99%.

**Restano all'encoder di 7-Zip**: Deflate64 (libdeflate non lo supporta) e le proprietà
impostate esplicitamente (`-mfb`, `-mmc`, `-mpass`, `-ma`), che sono dell'encoder di 7-Zip.
Senza l'opzione CMake `SEVENZIP_LIBDEFLATE` tutto torna all'encoder di 7-Zip.

### 2.2 Costo dei blocchi

Con la storia, i blocchi da 1 MiB non costano quasi niente a libdeflate: L6 31,852%
contro 31,851% dello stream unico; senza storia 31,928%. L12 1 MiB 30,529%, 4 MiB
30,518%.

## 3. Risultati

`tools/bench/market-bench.py` (2026-11-02), 20 thread, minimo di 2 ripetizioni:

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

| silesia | prima (0010/0011) | ora | pigz |
|---|---|---|---|
| `-mx1` | 32,98% a 537 MB/s | **32,78% a 1036 MB/s** | |
| `-mx3` | 32,14% a 346 MB/s | **31,85% a 820 MB/s** | -6: 32,20% a 332 |
| `-mx4` | 31,92% a 273 MB/s | **31,68% a 683 MB/s** | -9: 31,92% a 152 |
| `-mx5` | 30,99% a 107 MB/s | **30,61% a 104 MB/s** | |
| `-mx9` | 30,56% a 15 MB/s | **30,53% a 54 MB/s** | |

- `-mx3` comprime meglio di pigz -6 a 2,5 volte la velocità, con circa un terzo della
  CPU (4,6 s contro 12,2); `-mx4` meglio di pigz -9 a 4,5 volte la velocità.
- `-mx5` (predefinito di gzip e zip): 0,38 punti meglio alla stessa velocità.
- `-mx9`: rapporto migliore, 3,6 volte più veloce e un quarto della CPU.
- Corpus: `-mx3` 66,26% a 774 MB/s (pigz -6: 66,37% a 458).
- **Memoria**: i compressori di libdeflate ai livelli 10-12 (parser quasi ottimo) usano
  circa 6 MiB per thread: `-mx5` con 20 thread 204 MB di RSS invece di 94 (`-mx1`..`-mx4`:
  66-70 MB, meno di prima).

## 4. Test

- `tests/deflate_mt.py`: ogni livello, uscita identica con `-mmt1/3/8`, decompressa da
  zlib di Python; zip Deflate64 (encoder di 7-Zip) decompresso da 7zz.
- Prova a parte del solo compressore (`libdeflate_deflate_compress_chunk()` su silesia,
  tutti i livelli da 1 a 12, blocchi da 1 MiB): stream decompresso da zlib.
- Suite completa in release, ASan, TSan (deflate_mt), ARM64 (qemu, match finder NEON).
