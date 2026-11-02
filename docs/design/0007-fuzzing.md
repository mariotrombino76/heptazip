# 0007 — Fuzzing dei gestori di archivio

| | |
|---|---|
| **Stato** | Attivo: 2 harness (archivi, codec), semi, 6 campagne (2,8 miliardi di esecuzioni), test di regressione; 15 bug corretti dal fuzzing, 6 dall'analisi statica |
| **Data** | 2026-11-02 |
| **Codice** | `tests/fuzz/fuzz_archive.cpp`, `tests/fuzz/fuzz_codec.cpp`, `tools/fuzz/make-seeds*.{sh,py}`, `tools/fuzz/run-campaign.sh`, `tests/fuzz/regress/`, `tests/fuzz/regress-codec/` |

---

## 1. Obiettivo

Un archiviatore apre file che arrivano da chiunque: ogni gestore di formato e ogni decoder
è superficie d'attacco. Il fork legge 61 formati; il fuzzing ne copre 59 (esclusi Split,
che richiede più volumi, e Hash). Il fuzzing guidato dalla copertura (libFuzzer) con ASan
e UBSan cerca automaticamente gli input che causano accessi fuori dai limiti,
comportamento indefinito, leak, allocazioni sproporzionate o cicli infiniti.

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

### 2.1 Harness dei codec

`fuzz_codec` manda l'input direttamente a un decoder (`FUZZ_CODEC`, nome come per `-m`:
LZMA, LZMA2, PPMD, Deflate, Deflate64, BZip2, ZSTD, BCJ2, Rar1..Rar5, FLOAT, i filtri).
Attraverso un archivio il fuzzer arriva al decoder solo dopo aver costruito un'intestazione
valida; qui le proprietà, la dimensione d'ingresso e di uscita e la finish mode vengono dal
primo byte dell'input (formato descritto in testa a `fuzz_codec.cpp`). Uscita limitata a
64 MiB. Le proprietà RAR5 con dizionario oltre 1 GiB si scartano: il decoder alloca il
dizionario dichiarato (fino a 64 GiB), come LZMA, e nell'uso reale il gestore lo confronta
prima con il limite di memoria. `fuzz_codec_replay` riesegue `tests/fuzz/regress-codec/`
(test `fuzz_codec_regress`).

## 3. Semi e campagne

`make-seeds.sh` crea 31 archivi validi e piccoli (12-58 KB) per i formati che il fork sa
scrivere: 7z (LZMA, LZMA2, PPMd, BZip2, Deflate, Deflate64, Copy, BCJ2, Delta, ARM64,
zstd, AES, AES con intestazioni cifrate, intestazioni non compresse), zip (Deflate,
Deflate64, BZip2, LZMA, PPMd, Store, AES, ZipCrypto), tar, wim, gz, bz2, xz, zstd, lzip.
Gli altri formati partono da un corpus vuoto: libFuzzer ne costruisce l'intestazione dai
confronti con le firme (dizionario automatico).

`make-seeds-images.py` crea immagini valide per i formati di sola lettura che 7-Zip non sa
scrivere, con strumenti che non richiedono root: ISO 9660 (semplice, Joliet, Rock Ridge,
livello 4, El Torito) e UDF 2.60 con pycdlib, FAT12/16/32 con pyfatfs, ext2/3/4 con
`mke2fs -d`, squashfs con `mksquashfs` (gzip, lzo, xz, zstd, lzma). Con questi semi la
campagna 4 ha trovato 3 bug in formati che in 3 campagne da zero non avevano dato nulla.
`make-seeds-codec.py` crea gli stream grezzi per `fuzz_codec`.

`run-campaign.sh` lancia un processo per formato, N in parallelo; il corpus cresce di
campagna in campagna.

| Campagna | Durata per formato | Esecuzioni (tot.) | Artefatti | Bug |
|---|---|---|---|---|
| 1 | 5 min | 219 M | 12 | 7 (+5 OOM attesi) |
| 2 | 15 min | 631 M | 4 | 2 (+2 timeout: bombe di decompressione) |
| 3 | 30 min | 1406 M | 0 | 0 (harness con estrazione e limite di 256 MiB) |
| 4 | 45 min, 18 formati | 337 M | 3 | 3 (semi reali per iso/udf/fat/ext/squashfs) |
| 4b | 45 min: ext, compound, squashfs | 72 M | 0 | 0 (verifica delle correzioni) |
| codec 1 | 30 min, 25 codec | 116 M | 2 | 2 (Rar3, Rar5) |

Rar1 e Rar3 girano a ~380 esecuzioni al secondo (contro migliaia degli altri): il decoder
alloca a ogni input la finestra e la memoria PPMd. Servono campagne più lunghe.

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
| sparse | heap overflow | `AddInReserved()` oltre la riserva limitata della correzione precedente (65537 chunk) | `Add()` |
| squashfs | ciclo infinito | blocco zstd più grande di 8 KB: il decoder non avanza e il ciclo non esce (immagine di 4 KB; bug upstream) | uscita quando il decoder non fa progressi |
| ext | allocazione di GB | directory più grande dell'immagine (file di 47 KB) | controllo sulla dimensione dell'immagine |
| compound | allocazione > 3 GB | con settori da 4 KiB la FAT dichiarata era allocata prima di ogni controllo (518 B di file) | limite dalla dimensione del file |
| Rar3 (codec) | UB | `progress` NULL dereferenziato (gli altri decoder lo controllano) | controllo |
| Rar5 (codec) | UB | shift di 64 bit quando si leggono 0 bit (distanze allineate, `numBits == 4`) | due shift, senza ramo |

L'analisi statica (clang-tidy con clang-analyzer e parte di bugprone, cppcheck; ogni avviso
verificato a mano) ha dato altri 6 bug: `&v[0]` su vettori vuoti in 7zIn.cpp (riferimento
a NULL, input in `regress/7z/`), tabella Huffman senza pesi in ZstdDec (lettura dello stack
non inizializzato), elemento nuovo senza proprietà in 7zHandlerOut (`Files[-1]`), e
`std::bad_alloc` che usciva da metodi `throw()` in FloatCoder, Udf `GetStream` e
`OutMemStream::Write` (`std::terminate` invece di `E_OUTOFMEMORY`).

Gli helper sono generici, in `CPP/7zip/Common`:

- `ReadToBuffer_Growing(buf, size, read)` (StreamUtils.h) legge `size` byte facendo
  crescere il buffer (1 MiB, poi raddoppio) solo mentre i dati ci sono;
- `CGrowingBufSeqOutStream` (StreamObjects.h) fa lo stesso per l'uscita di un decoder,
  senza copia finale quando i dati arrivano tutti.

Le copie in più costano meno di un passaggio sui dati.

Ogni input è in `tests/fuzz/regress/<formato>/`. Il test `fuzz_regress` li riesegue: con
ASan le allocazioni oltre 1 GiB sono errori. Verificato che, con il codice precedente,
ogni input fa fallire il test. Gli input dei codec sono in `tests/fuzz/regress-codec/`.

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
- Semi reali per gli altri formati di sola lettura (rar, dmg, msi, hfs, apfs, ntfs...):
  partire da zero raggiunge l'intestazione, ma poco del decoder (vedi la campagna 4).
- Campagne più lunghe di `fuzz_codec`, soprattutto Rar1/Rar3 (lenti) e LZMA, PPMD,
  Deflate64, BZip2 (sotto il milione di esecuzioni).
- Restano in codice upstream altri metodi `throw()` che possono allocare senza
  `COM_TRY_BEGIN`: un `std::bad_alloc` lì termina il processo.
- Funzioni non supportate viste con i semi: ext4 `inline_data`, squashfs LZ4.
