# 0002 — Zstandard nel formato `.7z`

| | |
|---|---|
| **Stato** | Implementato |
| **Data** | 2026-10-30 |
| **Fase** | 3: nuovi algoritmi |
| **File** | `CPP/7zip/Compress/ZstdCodecRegister.cpp`, `ZstdLibEncoder.{h,cpp}`, `cmake/SevenZipZstd.cmake`, `third_party/zstd/` |

## 1. Situazione di partenza

- 7-Zip 26.05 ha un **decoder** zstd proprio (`C/ZstdDec.c`, `ZstdDecoder.cpp`) usato solo dal
  formato `.zst`: **non è registrato come metodo `.7z`**, quindi gli archivi `.7z` con zstd
  creati da 7-Zip-zstd (mcmilk) non si aprono.
- Non c'è un encoder: in `7zHandlerOut.cpp` è presente, commentato, il supporto per un
  encoder zstd di Igor Pavlov non ancora rilasciato.

## 2. Decisioni

| Tema | Scelta | Alternative scartate |
|---|---|---|
| ID del metodo | `0x4F71101`, nome `ZSTD`, come 7-Zip-zstd: archivi compatibili in entrambe le direzioni | Nuovo ID o nuovo container: archivi isolati |
| Proprietà del coder | 5 byte `{major, minor, livello, 0, 0}` come 7-Zip-zstd; il decoder le ignora (il frame contiene tutto) | — |
| Decoder | Quello di 7-Zip, registrato per `.7z`: l'estrazione non dipende da librerie esterne ed è presente anche nelle build con i makefile originali | Decoder di libzstd: dipendenza anche per estrarre |
| Encoder | libzstd 1.5.7 (riferimento) inclusa in `third_party/zstd` (solo `common` e `compress`), compilata con `ZSTD_MULTITHREAD` | Pacchetto di sistema: richiedeva `sudo` (utente non disponibile) e lega il binario alla libreria del sistema |
| Nomi | `NCompress::NZstdLib`, `ZstdLib*`, `ZstdCodecRegister`: distinti da `NCompress::NZstd`, così un futuro encoder upstream può convivere | — |
| Frame | Nessun checksum del frame (7z ha già il CRC per file). La dimensione del contenuto è dichiarata (`setPledgedSrcSize`) solo se il chiamante passa `inSize`: nei `.7z` non succede (la dimensione di un folder non è nota prima della compressione), quindi i frame non la contengono e usano la finestra del livello. *Correzione del 2026-10-31: la prima versione di questo documento diceva "sempre dichiarata", ma non era vero.* | Dichiarare la dimensione attesa: se un file cambia durante la compressione, libzstd fallisce (`srcSize_wrong`) |
| API di libzstd | L'encoder usa solo l'API stabile (`ZSTD_cParam_getBounds` invece delle costanti sperimentali). Solo la stima di memoria e dimensione dei job per la compressione parallela (design 0003, `NZstdLib::EstimateEncoder`) usa l'API sperimentale (`ZSTD_getCParams`, `ZSTD_estimateCStreamSize_usingCParams`), che non ha equivalenti stabili; la libreria è inclusa a versione fissa | `ZSTD_STATIC_LINKING_ONLY` per tutto l'encoder |

Opzioni: `-m0=zstd`, livello `-mx1`…`-mx22` (o `-m0=zstd:x=N`), finestra `-md=`, thread `-mmt`.

## 3. Interazione con l'estrazione parallela (design 0001)

`EstimateCoderDecodeMemory` stima 128 MiB per un decoder zstd: è la finestra massima dei
livelli standard. Il decoder di 7-Zip alloca il minimo tra dimensione del contenuto e
finestra. I frame dell'encoder del fork non contengono la dimensione del contenuto (§2) e
usano la finestra del livello, al massimo 128 MiB (livello 22): la stima è rispettata, ma per
un file piccolo compresso a un livello alto il decoder alloca comunque l'intera finestra.
Miglioramento possibile: passare a libzstd la dimensione attesa del folder
(`kExpectedDataSize`) come `ZSTD_c_srcSizeHint`, che adatta finestra e tabelle senza
scrivere la dimensione e senza fallire se cambia. Limite noto: frame di altri programmi
senza dimensione del contenuto e con finestra > 128 MiB (modalità "long") possono usare più
memoria della stima.

## 4. Blocchi solid: default invariato, misurato

Per i metodi diversi da LZMA2 il gestore `.7z` usa un blocco solid unico fino a 4 GiB, e
quindi un solo folder, decompresso da un thread. Silesia (202 MiB):

| zstd | Blocchi | Dimensione | Estrazione `t` | Compressione |
|---|---|---|---|---|
| `-mx3` | unico | — | 0,22 s | 0,23 s |
| `-mx3` | 32 MiB | +0,01% | **0,15 s (1,5×)** | 0,40 s (**1,7× più lenta**) |
| `-mx19` | unico | — | 0,22 s | 22,7 s |
| `-mx19` | 32 MiB | +0,07% | 0,18 s (1,2×) | 62,3 s (**2,8× più lenta**) |

I blocchi piccoli costano quasi nulla in dimensione ma rallentano molto la compressione: 7z
comprime i folder in sequenza e zstd parallelizza solo dentro un frame (job ≈ 4× la
finestra). **Il default resta invariato**; chi vuole estrazioni più veloci usa `-ms=32m`.
Il guadagno in estrazione è limitato dal thread principale (consegna + CRC a ~5 GB/s):
vedi la voce "CRC con PCLMUL / CRC nei worker" della roadmap.

Sviluppo possibile: comprimere in parallelo più folder `.7z` renderebbe vantaggiosi i
blocchi piccoli anche in compressione.

## 5. Verifica

- Suite di equivalenza: profili `zstd-solid`, `zstd-nonsolid` e loro copie corrotte e
  troncate (robustezza del decoder su dati zstd danneggiati); saltati, dichiarandolo, se
  la build non ha l'encoder.
- Roundtrip: `7z-zstd` (`-mx3`), `7z-zstd-nonsolid` (`-mx9`).
- `compare-archives.sh` salta i profili zstd se uno dei binari non ha l'encoder (7-Zip
  upstream).
- Interoperabilità: gli archivi scritti da libzstd sono letti dal decoder indipendente di
  7-Zip. Non è stato possibile provare 7-Zip-zstd (non disponibile su questa macchina).
