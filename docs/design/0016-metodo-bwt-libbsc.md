# 0016 — Metodo BWT nei `.7z` con libbsc

| | |
|---|---|
| **Stato** | Implementato (issue #81); misure in §4 |
| **Data** | 2026-11-04 |
| **Fase** | 3: nuovi algoritmi (strada 3 del programma "algoritmi": metodi nuovi per famiglie di dati) |
| **Codice** | `CPP/7zip/Compress/BscCoder.{h,cpp}`, `BscRegister.cpp`, `third_party/libbsc` (3.4.0, Apache-2.0), `cmake/SevenZipBsc.cmake`, makefile GCC e nmake, `tests/bwt_codec.py` |

---

## 1. Perché

Il benchmark di mercato (`docs/benchmark-mercato.md`) lasciava un punto scoperto: sui
testi un compressore Burrows-Wheeler batte LZMA. bzip3 su Silesia fa 22,29% a 59 MB/s
con 20 thread, contro 22,97% di LZMA2 `-mx9` a 8 MB/s (dopo #78); sul corpus binario non
guadagna (63,78% contro 63,93%). La strada 1 (LZMA a output identico) è chiusa (0015):
il guadagno che resta sta in un metodo diverso per i dati dove LZ perde.

## 2. Libreria: libbsc

| | libbsc 3.4 (Grebnov) | bzip3 1.5 |
|---|---|---|
| licenza | Apache-2.0 (compatibile con LGPL e BSD-2) | LGPL-3 |
| algoritmo | LZP + BWT (libsais) + QLFC (statico, adattivo, veloce) | LZP + BWT (libsais) + CM |
| blocchi | fino a 2 GiB, 5 × blocco di memoria | fino a 511 MiB |
| API | `bsc_compress` / `bsc_block_info` / `bsc_decompress`, header di 28 byte con dimensioni e checksum (Adler-32) | stream |

Stesso autore di libsais (la trasformata), codice maturo, licenza permissiva: scelto
libbsc. Vendored in `third_party/libbsc` senza la parte CUDA, compilato senza OpenMP:
il parallelismo è per blocchi nel codec (come brotli, design 0014). La policy di
sicurezza di questa sessione non permetteva di compilare il sorgente scaricato da solo:
la valutazione è stata fatta con la libreria già dentro il progetto.

## 3. Formato e codec

- **ID** `0x3F707F5E728B0002`, nome `BWT` (`-m0=bwt`): metodo solo di Heptazip, come
  FLOAT (`0x…0001`); gli archivi che lo usano si aprono solo con Heptazip.
- **Proprietà del coder**: 2 byte, versione del formato (1) e livello (informativo).
- **Flusso**: una sequenza di blocchi libbsc indipendenti; ogni blocco inizia con
  l'header di 28 byte di libbsc (dimensione compressa e originale, parametri, checksum):
  il decoder legge l'header, impara le dimensioni e legge il resto. Nessun framing
  aggiuntivo. Un blocco incomprimibile è memorizzato (`bsc_store`).
- **Parallelismo**: blocchi in parallelo con `CSlotPipeline` (encoder e decoder); la
  dimensione del blocco dipende solo dal livello (o da `c=`), quindi l'uscita è la stessa
  con qualunque numero di thread. Memoria ~7 × blocco per thread (5 × in libbsc, buffer
  di entrata e uscita), limitata a metà della RAM.
- **Opzioni**: `x` livello 1-9; `c=` dimensione del blocco (64 KiB … 1 GiB); `a=` coder
  (0 QLFC veloce, 1 statico, 2 adattivo); `mt`.

| livello | blocco | coder | Silesia, 20 thread |
|---|---|---|---|
| 1 | 2 MiB | QLFC veloce | 24,16%, 193 MB/s, dec 379 MB/s |
| 2 | 4 MiB | QLFC veloce | 23,74%, 138 MB/s, dec 321 MB/s |
| 3 | 8 MiB | QLFC statico | 22,85%, 107 MB/s, dec 233 MB/s |
| 4 | 16 MiB | QLFC statico | 22,68%, ~80 MB/s, dec 188 MB/s |
| **5** (default) | 16 MiB | QLFC adattivo | **22,41%, 72 MB/s, dec 117 MB/s** |
| 6 | 32 MiB | QLFC adattivo | 22,42%, 67 MB/s, dec 91 MB/s |
| 7, 8, 9 | 64 MiB | QLFC adattivo | 22,41%, 46 MB/s, dec 55 MB/s |

LZP sempre attivo con i valori predefiniti di bsc (hash 2^15, match minimo 72 byte).
Il rapporto si ferma a 16 MiB di blocco (8 MiB statico 22,85%, 16 MiB 22,68%, 32 e 64
MiB 22,68%; con l'adattivo 22,41% da 16 MiB in su) e guadagna 0,27 punti con il coder
adattivo; blocchi più piccoli danno più blocchi in parallelo in decompressione e meno
memoria. 128 e 256 MiB sono andati peggio su Silesia (23,00% a 256 MiB: i file sono più
piccoli del blocco e la trasformata mescola contesti di file diversi): i livelli alti
restano a 64 MiB, utili solo su grandi testi omogenei.

## 4. Misure

i7-12700H, 20 thread, `bwt_eval.sh` (una esecuzione; archivio creato da zero, estrazione
in `/dev/shm`):

| Silesia (212 MB) | rapporto | compressione | decompressione | RSS comp |
|---|---|---|---|---|
| **`-m0=bwt -mx5`** | **22,41%** | **72 MB/s** | 117 MB/s | 1,1 GB |
| `-m0=bwt -mx3` | 22,85% | 107 MB/s | 233 MB/s | 0,9 GB |
| `-m0=bwt -mx1` | 24,16% | 193 MB/s | 379 MB/s | 0,3 GB |
| LZMA2 `-mx5` | 23,22% | 16,6 MB/s | 385 MB/s | 1,4 GB |
| LZMA2 `-mx9` | 22,97% | 7,6 MB/s | 101 MB/s | 2,1 GB |
| bzip3 `-j 20` (benchmark di mercato) | 22,29% | 59 MB/s | 70 MB/s | 1,7 GB |
| zstd -19 (benchmark di mercato) | 24,93% | | ~1170 MB/s | |

A un thread: `-m0=bwt` (statico, 32 MiB) 25 MB/s contro 3,2 di LZMA2 `-mx5`, con
rapporto migliore (22,69% contro 22,99%).

Per file di Silesia, `-m0=bwt` (statico, 32 MiB) contro LZMA2 `-mx9`:

| file | BWT | LZMA2 `-mx9` | |
|---|---|---|---|
| webster (dizionario) | **16,04%** | 20,23% | testo |
| dickens (romanzo) | **22,32%** | 27,78% | testo |
| nci (database chimico) | **3,65%** | 5,19% | testo strutturato |
| xml | **7,47%** | 8,51% | |
| x-ray (immagine medica) | **44,92%** | 52,87% | |
| samba (sorgenti + binari) | 18,37% | **17,40%** | |
| mozilla (eseguibili) | 31,64% | **26,05%** | binario |
| sao (catalogo stellare) | 65,15% | **60,87%** | binario |

Corpus binario (110 MB): BWT 64,81% a 22 MB/s e decompressione 33 MB/s, LZMA2 `-mx5`
63,93% a 30 MB/s e 445 MB/s: **il BWT è un metodo per testi e dati strutturati**, non per
binari, e la decompressione resta 3-4 volte più lenta di LZMA2 (inversione della
trasformata, accessi casuali).

**Rispetto al mercato**: `-m0=bwt -mx5` batte LZMA2 `-mx9` di 0,56 punti a 9,5 volte la
velocità, e bzip3 in velocità (72 contro 59 MB/s, 117 contro 70 in decompressione) ma non
in rapporto (22,41% contro 22,29%). Il coder CM di bzip3 è più forte del QLFC adattivo;
il margine è 0,12 punti.

Decisioni: livello 5 di default a 16 MiB adattivo; niente scelta automatica per
estensione (come FLOAT per `.safetensors`/`.gguf`): gli archivi `BWT` si aprono solo con
Heptazip, e per i testi la scelta spetta all'utente (`-m0=bwt`). Da valutare in seguito
un'opzione del dialogo Aggiungi e della guida ("archivi di testi: BWT").

## 5. Verifiche

`tests/bwt_codec.py` (CTest `bwt_codec`): dimensioni vuoto, 1 byte, 5000 byte, ai
confini dei blocchi da 2 MiB, dati casuali, zeri; livelli 1, 3, 5 e `a=2:c=1m`; flusso
identico con `-mmt1` e `-mmt4`; estrazione parallela; un byte corrotto dà errore.
`tests/roundtrip_smoke.py`: `-m0=bwt -mx5 -mmt4` (anche su Windows, MinGW e MSVC).

**Fuzzing del decoder** (`fuzz_codec`, `FUZZ_CODEC=BWT`, semi = flussi di piccoli dati ai
livelli 1 e 5 e con blocchi da 64 KiB): in pochi secondi due casi di comportamento
indefinito in libbsc, `rangecoder.h`: con dati corrotti (checksum Adler-32 ricostruiti dal
fuzzer) il puntatore al flusso a 16 bit del range coder finisce a un indirizzo impari.
Corretto nella copia vendored con puntatori a byte e `memcpy` di 2 byte (stesso codice
macchina; modifica descritta in `third_party/libbsc/README-Heptazip.md`); i due casi sono
in `tests/fuzz/regress-codec/BWT/` e vengono riprodotti da `fuzz_codec_regress`. Dopo la
correzione 150 s di fuzzing senza altri casi. Il resto della robustezza viene da libbsc:
controlli dei parametri nell'header, Adler-32 dell'header, dei dati compressi e dei dati
decompressi, dimensioni dichiarate verificate dal codec (blocco ≤ 1 GiB).
