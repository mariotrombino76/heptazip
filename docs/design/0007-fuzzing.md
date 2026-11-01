# 0007 — Fuzzing dei gestori di archivio

| | |
|---|---|
| **Stato** | Attivo: harness, semi, campagne, test di regressione; 9 bug corretti |
| **Data** | 2026-11-01 |
| **Codice** | `tests/fuzz/fuzz_archive.cpp`, `tools/fuzz/make-seeds.sh`, `tools/fuzz/run-campaign.sh`, `tests/fuzz/regress/` |

---

## 1. Obiettivo

Un archiviatore apre file che arrivano da chiunque: ogni gestore di formato e ogni decoder
è superficie d'attacco. Il fork ha 61 formati in lettura. Il fuzzing guidato dalla
copertura (libFuzzer) con ASan e UBSan cerca automaticamente gli input che causano accessi
fuori dai limiti, comportamento indefinito, leak, allocazioni sproporzionate o cicli
infiniti.

## 2. Harness

`fuzz_archive` (un solo binario per tutti i formati; il formato si sceglie con
`FUZZ_FORMAT`, come `-t`):

1. `CCodecs::Load()` una volta; `CreateInArchive()` per ogni input.
2. `Open()` su un `CBufInStream` con i dati, `maxCheckStartPosition` 64 KiB, password
   `fuzz` (così i semi cifrati arrivano ai decoder).
3. `GetProperty()` di 13 proprietà per i primi 4096 elementi.
4. `Extract()` di tutti gli elementi verso uno stream che scarta i dati e fallisce oltre
   **256 MiB per input**; `IArchiveRequestMemoryUseCallback` limita i decoder che la
   usano a 512 MiB.

Scelte:

- **Estrazione, non test**: in modalità test molti gestori non chiedono lo stream di uscita
  e generano comunque tutti i dati; con lo stream il limite di 256 MiB si applica ovunque.
  La copertura dei corpus è la stessa (7z −3%, zip/qcow/sparse uguale o più alta).
- **Limite sull'uscita**: le immagini disco (sparse, qcow, vhd...) possono descrivere in
  45 byte un disco di 994 PB fatto di zeri. È lecito (un'immagine sparse reale da 1 TB
  quasi vuota esiste) e 7-Zip lo estrae correttamente: senza limite sarebbe un timeout, non
  un bug.
- **`-malloc_limit_mb=4200`** nelle campagne: LZMA e zstd allocano il dizionario o la
  finestra dichiarati nell'intestazione (fino a 4 GiB, come xz e zstd senza limite di
  memoria). Sono casi attesi; `-rss_limit_mb=2560` cattura invece i consumi reali.

Build: Clang 20, `-fsanitize=address,undefined,fuzzer-no-link`, senza assembly (le
routine asm non sono strumentate). Senza libFuzzer lo stesso sorgente diventa
`fuzz_replay` (macro `FUZZ_REPLAY`), che riesegue gli input di `tests/fuzz/regress/`.

## 3. Semi e campagne

`make-seeds.sh` crea 31 archivi validi e piccoli (12-58 KB) per i formati che il fork sa
scrivere: 7z (LZMA, LZMA2, PPMd, BZip2, Deflate, Deflate64, Copy, BCJ2, Delta, ARM64,
zstd, AES, AES con intestazioni cifrate, intestazioni non compresse), zip (Deflate,
Deflate64, BZip2, LZMA, PPMd, Store, AES, ZipCrypto), tar, wim, gz, bz2, xz, zstd, lzip.
Gli altri 40 formati partono da un corpus vuoto: libFuzzer ne costruisce l'intestazione dai
confronti con le firme (dizionario automatico).

`run-campaign.sh` lancia un processo per formato, N in parallelo; il corpus cresce di
campagna in campagna.

| Campagna | Durata per formato | Esecuzioni (tot.) | Artefatti | Bug |
|---|---|---|---|---|
| 1 | 5 min | 219 M | 12 | 7 (+5 OOM attesi) |
| 2 | 15 min | 631 M | 4 | 2 (+2 timeout: bombe di decompressione) |
| 3 | 30 min | | | |

## 4. Bug trovati e corretti

| Formato | Tipo | Causa | Correzione |
|---|---|---|---|
| Z | UB | load a 32 bit disallineato con puntatore convertito | `Z7_LoadNative32` (memcpy) |
| zip (Shrink) | UB | stesso schema | `Z7_LoadNative32` |
| cab (Quantum) | leak | la finestra di `SetParams()` non veniva mai liberata | distruttore |
| wim | allocazione 2⁵⁶ B | dimensione di una risorsa solida (da `Solids[]`) allocata senza il controllo del limite; `OpenXml` controllava un'altra dimensione | controllo in `UnpackData` sulla dimensione reale |
| wim | allocazione 8,5 GB | risorsa entro il limite (31/32 della RAM) ma dichiarata da un file di pochi KB | `CGrowingBufSeqOutStream`: il buffer cresce con i dati decodificati |
| rpm | allocazione 4,3 GB | dimensione dell'intestazione allocata prima di leggerla (374 B di file) | `ReadToBuffer_Growing()` |
| nsis | allocazione 3,7 GB | stesso schema (61 B di file) | `ReadToBuffer_Growing()` |
| vdi | allocazione 1,6 GB | tabella dei blocchi non confrontata con il file | controllo sulla dimensione del file |
| sparse | allocazione 3,5 GB | `Reserve(NumChunks)` dall'intestazione | riserva limitata, il vettore cresce |

Gli helper sono generici, in `CPP/7zip/Common`:

- `ReadToBuffer_Growing(buf, size, read)` (StreamUtils.h) legge `size` byte facendo
  crescere il buffer (1 MiB, poi raddoppio) solo mentre i dati ci sono;
- `CGrowingBufSeqOutStream` (StreamObjects.h) fa lo stesso per l'uscita di un decoder,
  senza copia finale quando i dati arrivano tutti.

Le copie in più costano meno di un passaggio sui dati.

Ogni input è in `tests/fuzz/regress/<formato>/`. Il test `fuzz_regress` li riesegue: con
ASan le allocazioni oltre 1 GiB sono errori. Verificato che, con il codice precedente,
ognuno dei 9 input fa fallire il test.

## 5. Casi attesi, non corretti

- **Dizionario LZMA / finestra zstd dichiarati** (lzma, lzma86, swfc, zip-LZMA, zstd): un
  file di 15 byte può chiedere 4 GiB. Una possibile miglioria è limitare il dizionario LZMA
  alla dimensione di uscita quando è nota (un file da 10 KB compresso con `-mx9` non
  richiederebbe 256 MiB). Non è banale: se l'uscita supera il buffer ridotto, una distanza
  oltre `dicBufSize` leggerebbe fuori dal buffer circolare; il decoder deve garantire di
  fermarsi all'`outSize`. Da progettare a parte.
- **Bombe di decompressione delle immagini disco**: comportamento corretto (vedi §2).
- Restano nel codice altri load con puntatore convertito (LzxDecoder, ZstdDec, Sha3,
  Blake2s, ComHandler, LpHandler, VhdHandler) su buffer allineati per costruzione o in
  rami protetti. Il fuzzing con UBSan segnala ogni caso disallineato reale.

## 6. Come continuare

- Campagne più lunghe sui formati con molto codice: 7z, zip, rar, rar5, wim, cab, chm, iso,
  udf, ntfs, hfs, apfs, dmg.
- Semi reali per i formati di sola lettura (rar, iso, dmg, msi...) da archivi pubblici:
  partire da zero raggiunge l'intestazione, ma poco del decoder.
- Harness dedicati ai codec senza gestore di archivio (LZMA2 diretto, PPMd, BCJ2), con i
  parametri presi dall'input.
