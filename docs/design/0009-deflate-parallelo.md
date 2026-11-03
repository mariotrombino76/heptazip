# 0009 — Deflate a blocchi in parallelo

| | |
|---|---|
| **Stato** | Implementato, attivo di default (gzip, zip, 7z con Deflate/Deflate64) |
| **Data** | 2026-11-02 |
| **Fase** | 2: parallelizzazione |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`) |
| **File coinvolti** | `CPP/7zip/Compress/DeflateEncoder.{h,cpp}`, `CPP/7zip/Archive/GzHandler.cpp`, `tests/deflate_mt.py` |

---

## 1. Problema

Il confronto con gli altri strumenti (docs/benchmark-mercato.md) ha mostrato che Deflate
era il punto debole di 7-Zip: l'encoder ignorava `kNumThreads` e usava un solo core. Su
silesia (212 MB), i7-12700H, 20 thread:

| | rapporto | compressione |
|---|---|---|
| 7-Zip 26.05 `-tgzip -mx5` | 30,96% | 12 MB/s |
| gzip -6 | 32,19% | 37 MB/s |
| pigz -6 (20 thread) | 32,20% | 334 MB/s |
| pigz -9 (20 thread) | 31,92% | 150 MB/s |

7-Zip comprime meglio di tutti (parser ottimo), ma 10-25 volte più lentamente di pigz.
gzip e zip sono i formati più usati: è il miglioramento con l'effetto più largo.

BZip2, al contrario, non ha problemi: `-tbzip2` (predefinito `-mx5`, blocchi da 900k come
`bzip2 -9`) è già parallelo e 4,5 volte più veloce di bzip2 allo stesso rapporto; la riga
`-mx9` del primo benchmark era fuorviante (7 passate: 26 volte la CPU per lo 0,7%).

## 2. Soluzione

Lo stesso schema di pigz, dentro l'encoder di 7-Zip:

1. l'ingresso è diviso in **blocchi da 1 MiB**; ogni blocco è codificato da un coder di
   un pool di thread (`CMtTaskPool`, design 0003);
2. ogni coder riceve prima del blocco gli **ultimi 32 KiB** (64 KiB per Deflate64) del
   blocco precedente: vanno nel match finder (`Bt3Zip/Hc3Zip_MatchFinder_Skip`) ma non
   sono codificati, quindi le distanze possono tornare indietro come in un solo stream;
3. ogni blocco tranne l'ultimo finisce con un **blocco stored vuoto** (come
   `Z_SYNC_FLUSH` di zlib), così il successivo inizia su un confine di byte e le uscite si
   concatenano. Il risultato è **un solo stream Deflate standard**: lo legge qualunque
   decoder (zlib, gzip, unzip, Python `zipfile`...);
4. se l'ingresso finisce esattamente con un blocco pieno, uno stream finale vuoto con i
   codici fissi (`03 00`, come zlib) chiude lo stream.

Proprietà:

- **Uscita identica per ogni `-mmt`**: la dimensione dei blocchi non dipende dal numero
  di thread (stesso principio del design 0003). `-mmt1` usa lo stesso percorso.
- **Fino a 1 MiB l'uscita è identica a 7-Zip 26.05**: il primo blocco, se è anche
  l'ultimo, è codificato come prima (verificato con `-mx1`, `-mx5`, `-mx9`).
- **zip con molti file**: zip parallelizza già sui file e dà al coder un thread; non
  cambia nulla tranne il taglio in blocchi dei file sopra 1 MiB (corpus di 1285 file:
  +0,009%, stesso tempo).
- `GzHandler` ora passa il numero di thread al coder, come fa già `Bz2Handler`.

## 3. Rapporto: le misure

Il blocco da solo costa poco, ma il parser ottimo prezza ogni blocco Deflate con i codici
di Huffman del blocco precedente: un blocco di 1 MiB inizia con i codici predefiniti e il
suo primo blocco Deflate è codificato peggio. Correzione: il primo blocco Deflate di ogni
blocco dopo il primo fa **4 passate** (codici ricalcolati dal blocco stesso; richiede la
cache dei match, `m_IsMultiPass`). Non serve in modalità veloce (`-mx1`..`-mx4`, senza
prezzi). Silesia, perdita rispetto allo stream unico di 7-Zip 26.05:

| Blocco | `-mx1` | `-mx5` | `-mx5` + 4 passate | `-mx9` | tempo `-mx5` |
|---|---|---|---|---|---|
| 256 KiB | +0,027% | +0,39% | | +0,20% | 1,97 s |
| 512 KiB | +0,013% | +0,25% | | +0,15% | 1,98 s |
| **1 MiB** | +0,012% | +0,17% | **+0,09%** | +0,11% | 1,96 s |
| 2 MiB | +0,007% | +0,11% | | +0,07% | 1,99 s |
| 4 MiB | +0,009% | +0,07% | | +0,05% | 2,06 s |
| 16 MiB | +0,002% | +0,01% | | +0,02% | 2,76 s |

Senza la storia (blocchi indipendenti) la perdita a 1 MiB era dello 0,41%. Passate
sul primo blocco: 2 → +0,15%, 3 → +0,10%, 4 → +0,09%, 6 → +0,09%.

Scelta: **1 MiB e 4 passate**. Il resto della perdita (circa 300 byte per blocco: ultimo
blocco Deflate corto, tabelle ricominciate) si toglie solo con blocchi più grandi, che
costano memoria e parallelismo sui file medi (un file da 8 MB con blocchi da 4 MiB usa due
thread). Lo 0,09% è un decimo della differenza con pigz (30,99% contro 32,2%).

## 4. Memoria

Un coder per thread (~2,2 MiB: match finder, cache dei match, tabelle) e un anello di
`1,5 × thread + 1` blocchi (ingresso 1 MiB + 32 KiB, uscita). Silesia `-mx5`, 20 thread:
96 MiB (un thread: 7 MiB). Con 2 blocchi per thread: 110 MiB e stessa velocità; con
`thread + 2`: 81 MiB e il 6% più lento.

## 5. Risultati

`tools/bench/market-bench.py` (2026-11-02), 20 thread, compressione su `/dev/shm`, minimo
di 2 ripetizioni. Silesia (212 MB):

| | rapporto | compressione | CPU s | RSS |
|---|---|---|---|---|
| 7-Zip 26.05 `-tgzip -mx5` | 30,96% | 12,2 MB/s | 17,6 | 7 MB |
| **Heptazip `-tgzip -mx1`** | 33,15% | **512 MB/s** | 7,5 | 80 MB |
| **Heptazip `-tgzip -mx5`** | **30,99%** | **105 MB/s** | 37,8 | 94 MB |
| Heptazip `-tgzip -mx9` | 30,56% | 15,3 MB/s | 263,8 | 110 MB |
| pigz -6 | 32,20% | 340 MB/s | 12,3 | 16 MB |
| pigz -9 | 31,92% | 152 MB/s | 25,8 | 16 MB |
| gzip -6 | 32,19% | 38 MB/s | 5,7 | 2 MB |

Corpus (110 MB, sorgenti e documenti): `-mx5` da 29,8 a 205 MB/s (pigz -9: 298 MB/s a
66,32%, Heptazip 66,09%).

`-mx5` è 8,6 volte più veloce di prima e comprime meglio di pigz -9 (0,9 punti) a due
terzi della sua velocità; `-mx1` è più veloce di pigz -6. Nessuno dei due domina pigz su
entrambi gli assi: in 7-Zip `-mx1`..`-mx4` sono lo stesso profilo (modalità veloce), e un
livello intermedio (circa 32% sopra i 340 MB/s) è il passo successivo (§8).

zip e 7z con Deflate e Deflate64: stesso guadagno (`-tzip -mx5` silesia: 17,4 s → 2,0 s).
Il decoder Deflate (non cambiato, un thread) decomprime a 320 MB/s, come pigz e più di
gzip (246 MB/s).

## 6. Test

- `tests/deflate_mt.py` (CTest `deflate_mt`): 10 dimensioni intorno ai multipli di 1 MiB
  (vuoto, 1 MiB ± 1, 4 MiB + 77, casuali, zeri, misti) × gzip `-mx1/-mx5/-mx9`, zip
  Deflate e Deflate64: uscita identica con `-mmt1/-mmt3/-mmt8`, decompressione con zlib
  di Python (CRC compreso). Verificato che fallisce se lo stream finale è sbagliato.
- `update_equivalence_pfc` copre già Deflate e Deflate64 nel formato 7z.
- ASan e TSan: nessun errore; scrittura che fallisce a metà (`/dev/full`): stesso codice
  di uscita e messaggio di 7-Zip 26.05.

## 7. Alternative considerate

- **Blocchi indipendenti senza storia**: più semplice, ma +0,41% a 1 MiB.
- **Blocchi più grandi o dipendenti dalla dimensione dell'ingresso** (es. dimensione/32):
  meno perdita, ma più memoria e la dimensione non è sempre nota (stdin).
- **Numero di blocchi dipendente dai thread**: uscita diversa per ogni `-mmt`; scartato,
  come nel design 0003.
- **Prezzi presi dal blocco precedente** (come in uno stream unico): impossibile in
  parallelo, il blocco precedente non è ancora codificato.

## 8. Come continuare

- ~~Livelli `-mx2`..`-mx4` distinti~~: fatto, lazy matching (design 0010).
- Decoder Deflate più veloce (libdeflate, da misurare quando sarà disponibile).
