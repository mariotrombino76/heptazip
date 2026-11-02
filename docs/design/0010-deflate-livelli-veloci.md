# 0010 — Deflate: livelli veloci con lazy matching

| | |
|---|---|
| **Stato** | Implementato, attivo di default (gzip, zip, 7z con Deflate/Deflate64, `-mx1`..`-mx4`) |
| **Data** | 2026-11-02 |
| **Fase** | 2: prestazioni |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`) |
| **File coinvolti** | `CPP/7zip/Compress/DeflateEncoder.{h,cpp}`, `CPP/7zip/Compress/BitlEncoder.h`, `CPP/7zip/Common/OutBuffer.h`, `C/LzFind.c`, `tests/deflate_mt.py` |

---

## 1. Problema

Dopo il Deflate in parallelo (design 0009) i due livelli di 7-Zip stavano ai due lati di
pigz -6 senza batterlo su entrambi gli assi (silesia, 20 thread):

| | rapporto | compressione |
|---|---|---|
| Tensor7 `-tgzip -mx1` | 33,15% | 512 MB/s |
| pigz -6 | 32,20% | 340 MB/s |
| pigz -9 | 31,92% | 152 MB/s |
| Tensor7 `-tgzip -mx5` | 30,99% | 105 MB/s |

In 7-Zip `-mx1`..`-mx4` sono lo stesso profilo: modalità veloce (`algo 0`), match finder
hash chain con 32 passi, e parsing **greedy** (si codifica sempre il match più lungo).
Lo spazio tra 33,15% e 30,99% era vuoto.

## 2. Soluzione

### 2.1 Lazy matching (`GetOptimalFast`)

La modalità veloce ora fa **lazy matching**, come `deflate_slow()` di zlib: il match
della posizione corrente si codifica solo se la posizione successiva non ne ha uno
migliore; altrimenti il byte corrente è un letterale, e il match successivo è
confrontato allo stesso modo con quello dopo. Tre regole prese da zlib e zstd:

- **migliore** si decide con il guadagno del parser lazy di zstd: 4 per byte di
  lunghezza meno i bit extra della distanza; il match successivo deve avere 2 in più
  (`kNextBias`). Rispetto al solo confronto delle lunghezze (zlib): -0,11 punti a parità
  di tempo;
- tra i match della stessa posizione (più lunghi = più lontani) si sceglie con lo stesso
  guadagno (`kShorterBias = 2`): -0,02 punti;
- un match di 3 byte a più di 4096 byte di distanza non si usa (costa più di 3
  letterali, `TOO_FAR` di zlib); dopo un match di almeno 8 byte la posizione successiva si
  cerca con un quarto della catena (`good_length` di zlib).

Il match della posizione successiva resta in sospeso finché `m_OptimumEndIndex !=
m_OptimumCurrentIndex`: la modalità veloce non usa il parser ottimo, e `TryBlock()` già
non chiude un blocco con una decisione in sospeso.

Con più passate (`-mpass`) la seconda passata di un blocco legge i match dalla cache
(`m_OnePosMatchesMemory`) mentre il match finder è alla fine del blocco: i byte
disponibili si contano dalla posizione corrente (`+ m_AdditionalOffset - 1`), così le
decisioni sono le stesse nelle due passate. Il test `deflate_mt` lo verifica
(`-mx3 -mpass=3`); senza la correzione falliva per CRC.

### 2.2 I livelli

| livello | fb | catena (mc) | prima |
|---|---|---|---|
| `-mx1` | 32 | 10 | greedy, fb 32, mc 32 |
| `-mx2` | 32 | 24 | come `-mx1` |
| `-mx3` | 64 | 48 | come `-mx1` |
| `-mx4` | 128 | 128 | come `-mx1` |
| `-mx5`..`-mx9` | come prima (parser ottimo) | | |

`-mfb` e `-mmc` cambiano ancora i valori. `-mx5` e oltre producono gli stessi byte di
prima (verificato anche zip Deflate64 `-mx7`).

### 2.3 Due accelerazioni, stessa uscita

- **Scrittura dei bit** (`CBitlEncoder`): accumulatore a 64 bit scritto 4 byte alla
  volta, invece di un ciclo per byte. `-mx1` greedy, un thread: da 74 a 86 MB/s.
- **Hash chain** (`Hc_GetMatchesSpec` di `C/LzFind.c`): la lunghezza del match si misura
  8 byte alla volta con `LzFind_MatchEnd()`, come già i binary tree (design 0004).
  Serve anche a LZMA `-mx1`..`-mx4` (hc4/hc5); i file sono identici (verificato LZMA2
  `-mx1` e LZMA `-mmf=hc4`).

## 3. Misure

### 3.1 Scelta dei parametri

Silesia, 20 thread, con `T7_*` variabili di prova (rimosse):

| variante | rapporto | MB/s |
|---|---|---|
| greedy, mc 32 (26.05) | 33,15% | 517 |
| greedy, mc 128 | 32,83% | 397 |
| greedy, BT invece di hash chain | 32,65% | 144 |
| lazy (confronto delle lunghezze), mc 64 | 32,36% | 343 |
| + guadagno con la distanza | 32,25% | 342 |
| + scelta tra i match della stessa posizione | 32,23% | 336 |
| + lazy anche oltre 16 byte, fb 64, mc 48 | **32,15%** | **353** |
| lazy2 (anche la posizione +2, come zstd) | stessa frontiera di una catena più lunga | |
| codici di Huffman fino a 15 bit invece di 12 | +0,001% | |
| blocchi più grandi (valori per blocco) | nessun effetto | |

Il profilo (gprof, `-mx3`): 81% del tempo nella hash chain, 7% nella scrittura dei
blocchi. Il lazy di 7-Zip con catena 128 ha il rapporto di zlib -6 (32,23% contro
32,20%): l'algoritmo è lo stesso, la differenza viene dalle decisioni (§2.1).

### 3.2 Risultati

`tools/bench/market-bench.py --only 'gzip|pigz'` (2026-11-02), 20 thread, minimo di 2
ripetizioni:

| | rapporto | compressione | CPU s | |
|---|---|---|---|---|
| 7-Zip 26.05 `-tgzip -mx1` | 33,15% | 74 MB/s | 2,9 | |
| **Tensor7 `-mx1`** | **32,98%** | **529 MB/s** | 7,1 | prima: 33,15% a 512 |
| **Tensor7 `-mx3`** | **32,14%** | **362 MB/s** | 10,7 | pigz -6: 32,20% a 333 |
| **Tensor7 `-mx4`** | **31,92%** | **272 MB/s** | 14,2 | pigz -9: 31,92% a 151 |
| Tensor7 `-mx5` | 30,99% | 109 MB/s | 37,2 | |
| pigz -6 | 32,20% | 333 MB/s | 12,3 | |
| pigz -9 | 31,92% | 151 MB/s | 26,1 | |

Su silesia `-mx3` batte pigz -6 su entrambi gli assi e `-mx4` ha il rapporto di pigz -9
a 1,8 volte la sua velocità. `-mx1` è veloce come prima con un rapporto migliore.

Corpus (110 MB, sorgenti e documenti, in gran parte già compressi): `-mx3` 66,36% a
435 MB/s, pigz -6 66,37% a 461 MB/s (3 ripetizioni). Qui la ricerca dei match pesa poco
(`-mx1 -mmc=1`: 541 MB/s) e il limite è la pipeline a blocchi da 1 MiB su un input
medio: `-mx3` usa meno CPU di pigz (4,5 s contro 4,7) ma è il 6% più lento. `-mx4`:
66,32% a 371 MB/s, pigz -9 66,32% a 298.

Un thread, silesia: `-mx1` 80 MB/s (26.05: 74), `-mx3` 50 MB/s, `-mx4` 37 MB/s
(gzip -6: 38 MB/s al 32,19%).

## 4. Test

- `tests/deflate_mt.py`: gzip `-mx1`..`-mx5`, `-mx9` e `-mx3 -mpass=3`, zip Deflate e
  Deflate64 `-mx3` e `-mx5`: stessi file con `-mmt1/-mmt3/-mmt8`, decompressi da zlib di
  Python (Deflate64 da 7zz).
- Suite completa (release e ASan per i test Deflate).

## 5. Come continuare

- Pipeline su input medi (corpus): blocchi più piccoli all'inizio e alla fine, o lettura
  dell'ingresso fuori dal thread principale.
- Decoder Deflate più veloce (libdeflate, da misurare quando sarà disponibile).
