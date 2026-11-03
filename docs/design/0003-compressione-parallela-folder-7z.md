# 0003 — Compressione parallela dei folder `.7z`

| | |
|---|---|
| **Stato** | Implementato, attivo di default (vedi §10, risultati) |
| **Data** | 2026-10-31 |
| **Fase** | 2: parallelizzazione |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`) |
| **File coinvolti** | `CPP/7zip/Archive/7z/7zUpdate.{h,cpp}`, `7zHandler.h`, `7zHandlerOut.cpp`, `7zHeader.h`, nuovi `7zUpdateMt.{h,cpp}`; `CPP/7zip/Compress/ZstdLibEncoder.{h,cpp}` (stima dell'encoder zstd); primitive di threading spostate da `7zExtractMt.{h,cpp}` a `CPP/7zip/Common/MtTaskPool.{h,cpp}` |

---

## 1. Contesto e problema

Quando crea un archivio, `NArchive::N7z::Update()` (`7zUpdate.cpp`) divide i file nuovi in
**folder** (blocchi solid) e li comprime **uno alla volta** nel thread chiamante
(ciclo "Compress files to new solid blocks"). Il parallelismo c'è solo *dentro* l'encoder
di un singolo folder, e dipende dal metodo:

| Metodo | Thread dentro un folder |
|---|---|
| LZMA2 | blocchi indipendenti (`Lzma2Enc` + `MtCoder`), solo se il folder è più grande di un blocco (dizionario × 4, da 1 a 256 MiB); in più 2 thread per il match finder (`bt*` dal livello 5) |
| LZMA | al massimo 2 (match finder multi-thread) |
| zstd (libzstd) | `nbWorkers` job da `jobSize` (alcuni MiB); un folder piccolo dà un solo job |
| BZip2 | blocchi da 100–900 KB in parallelo |
| PPMd, Deflate, Deflate64, Copy | 1 |

Quindi con molti folder piccoli i core restano in gran parte inattivi:

- archivi **non solid** (`-ms=off`): un folder per file;
- **solid a blocchi piccoli** (`-ms=8m`, `-ms=e`, `-qs`);
- metodi senza multi-thread o con poco (**PPMd**, **Deflate**, **LZMA**);
- **zstd con blocchi solid piccoli**: in 0002 il blocco solid di default non è stato ridotto
  proprio perché la compressione diventava 1,7–2,8 volte più lenta.

Le misure sono in §1.1 (base) e §10 (risultati).

### 1.1 Misure di partenza

Vedi §10: la colonna `pfc=off` è il ciclo sequenziale di upstream. Con 20 thread un archivio
non solid di Silesia usa 1,4 core, PPMd e Deflate 1 core.

## 2. Obiettivi e non-obiettivi

**Obiettivi**

- Comprimere in parallelo più folder nuovi, usando i core che l'encoder di un singolo
  folder lascia liberi.
- **Archivi identici byte per byte** a quelli della compressione sequenziale con le stesse
  opzioni (salvo l'IV casuale di AES, che cambia a ogni esecuzione anche oggi).
- Memoria limitata dal limite di `-mmemuse` già usato da 7-Zip.
- Nessuna regressione quando non conviene: un unico folder grande (il caso solid di
  default) segue esattamente il percorso di oggi.

**Non-obiettivi (per ora)**

- Cambiare il formato o la suddivisione in folder (`-ms`, ordinamento, filtri): la
  suddivisione resta quella di oggi, calcolata prima di comprimere.
- Parallelizzare la **ricompressione** dei folder esistenti durante un aggiornamento
  (`Repack old solid block`): resta sequenziale. Si può fare in seguito con lo stesso
  meccanismo.
- Cambiare il numero di thread di un encoder per farlo andare più veloce: cambierebbe
  l'output (vedi §3.4).

## 3. Vincoli emersi dall'analisi del codice

### 3.1 Callback dell'interfaccia utente

I file si leggono tramite `CFolderInStream` (`7zFolderInStream.cpp`), che per ogni file
chiama `IArchiveUpdateCallback::GetStream()` e, a fine file, `SetOperationResult()`. Sono
callback dell'interfaccia (la console stampa il nome del file, la GUI aggiorna la
finestra). Oggi sono chiamate in ordine, un file alla volta, con un solo stream aperto.
Gli encoder multi-thread di upstream (`MtCoder`) già leggono l'input da thread diversi dal
chiamante, ma sempre in ordine e uno alla volta (sotto lock).

Conseguenza: le letture restano **in un solo thread, in ordine**. I worker comprimono dati
già letti in memoria.

### 3.2 Scrittura dell'archivio

I dati compressi dei folder si scrivono uno dopo l'altro in `archive.SeqStream`, nell'ordine
dei folder; le intestazioni (`newDatabase`: `Folders`, `PackSizes`, `CoderUnpackSizes`,
`NumUnpackStreamsVector`, file) si scrivono alla fine. Ogni folder si può comprimere in
memoria e scrivere dopo, **nell'ordine originale**: le posizioni nell'archivio non
dipendono da quando un folder viene compresso.

### 3.3 Encoder

`CEncoder` (`7zEncode.cpp`) crea i coder una volta (`CreateMixerCoder`, al primo
`Encode1`) e li riusa per tutti i folder del gruppo di filtri. Le proprietà si impostano
con `inSizeForReduce`, che è **unico per tutta l'operazione** (somma dei file se solid, file
più grande se non solid): il dizionario ridotto, quindi le proprietà scritte nell'archivio,
non dipendono dal singolo folder. Per ogni folder si passa invece
`kExpectedDataSize` (dimensione attesa del folder).

Un `CEncoder` non si può usare da due thread insieme, ma se ne possono creare più di uno
con le stesse opzioni: ogni worker avrà il suo.

### 3.4 L'output dipende dal numero di thread dell'encoder

- **zstd:** con `nbWorkers = 0` (1 thread) libzstd produce frame diversi da quelli con
  `nbWorkers ≥ 1`.
- **LZMA2:** con più thread di blocco e un folder più grande di un blocco, lo stream è
  diviso in blocchi indipendenti; con un thread no.

Ridurre i thread dell'encoder di un job cambierebbe quindi l'archivio. Per questo ogni job
usa **le stesse opzioni** dell'encoder sequenziale, compreso il numero di thread. Per i
folder piccoli gli encoder usano comunque pochi thread (vedi §5.6), ed è proprio il caso
in cui la parallelizzazione dei folder serve.

### 3.5 Dimensioni dei file dentro il folder e granularità delle letture

`CFolderInStream` implementa anche `ICompressGetSubStreamSize`: l'encoder può chiedere la
dimensione del file *k* del folder. La risposta dipende da quanto è stato letto in quel
momento (file già chiusi: dimensione esatta; file corrente: dimensione dichiarata o
`S_FALSE`; file successivi: `S_FALSE`). Tra gli encoder lo usa solo **BCJ2**, che adatta la
conversione ai confini dei file: la sua uscita dipende quindi dalla sequenza delle risposte.

Inoltre `CFolderInStream::Read` restituisce al massimo 1 MiB per chiamata e mai oltre la
fine del file corrente.

Conseguenze:

- i folder con **BCJ2** si comprimono inline (replicare esattamente la sequenza delle risposte
  sarebbe fragile, e BCJ2 ha già il suo multi-thread tramite il mixer);
- lo stream in memoria dei job restituisce i dati con la **stessa granularità** di
  `CFolderInStream` (al massimo 1 MiB, mai oltre la fine di un file). Gli encoder noti
  riempiono i propri buffer indipendentemente da come arrivano i dati, ma così l'input visto
  dall'encoder è identico anche nella suddivisione delle letture.

### 3.6 AES

L'IV di ogni folder è generato casualmente (`ResetInitVector`), con il generatore globale
protetto da lock. Il confronto byte per byte non vale per gli archivi cifrati, oggi come
domani; vale il roundtrip.

### 3.7 Avanzamento

`CLocalProgress` (`lps`) chiama la callback dell'interfaccia. Con più encoder insieme non
può essere chiamata dai worker: i worker aggiornano contatori atomici e il thread principale
riporta l'avanzamento.

## 4. Requisiti

### 4.1 Equivalenza osservabile

1. Con le stesse opzioni, l'archivio prodotto con `pfc=on` è **identico byte per byte** a
   quello con `pfc=off` (salvo AES).
2. In caso di successo la sequenza di callback `GetStream` / `SetOperationResult` è la stessa
   (stessi file, stesso ordine, stessi esiti); cambia solo il momento in cui avvengono
   rispetto all'avanzamento.
3. In caso di errore il codice d'errore è lo stesso. Differenza ammessa: prima dell'errore
   possono essere già stati letti file di folder successivi (lettura anticipata). L'archivio
   in caso d'errore viene comunque scartato.
4. File che non si aprono (bloccati, permessi): stesso comportamento di oggi (avviso, file
   escluso dal folder).

### 4.2 Prestazioni (criteri di accettazione)

- Silesia non solid, LZMA2 `-mx5`, 20 thread: almeno 2 volte più veloce del sequenziale.
- PPMd e zstd con `-ms=16m`: vicino a `min(numero di folder, thread)` volte.
- Solid di default (un folder grande): nessuna differenza misurabile (stesso percorso).

### 4.3 Memoria

Il totale stimato dei job in corso (buffer di input e output + memoria dell'encoder) resta
entro il limite di memoria della compressione (`-mmemuse`; di default la metà della RAM
per i thread automatici, era l'80% come in 7-Zip: issue #73, `CMultiMethodProps` in
`HandlerOut.h`). Un job che da solo supera il limite non si fa in parallelo.

## 5. Design proposto

### 5.1 Panoramica

```
thread principale                         worker (pool)
-----------------                         -------------
pianifica i folder (come oggi)
per ogni folder, in ordine:
  piccolo? ── sì ─► legge i file in un buffer
                    (CFolderInStream: CRC, dimensioni, tempi)
                    attende posto (thread + memoria)
                    accoda il job ─────────────────►  Encode1(buffer → buffer)
                                                       con il CEncoder del worker
  no ───► attende i job in corso, li scrive,
          comprime il folder inline come oggi
scrive i job completati in ordine ◄────────────────  job pronto
  (dati nell'archivio + newDatabase)
```

Il thread principale fa tre cose, sempre in ordine: legge, scrive, riporta l'avanzamento.
I worker comprimono soltanto.

### 5.2 Quali folder vanno in parallelo

Un folder è **parallelo** se:

- la dimensione attesa (somma di `ui.Size`) è al massimo `kMaxParallelFolderSize` = 64 MiB
  (come in 0001: un folder grande lo comprime meglio l'encoder con i suoi thread, e il
  buffer resta piccolo);
- il gruppo di metodi è **noto** (stima di thread e memoria disponibile, §5.6–5.7) e non
  contiene BCJ2 (§3.5);
- c'è più di un folder nel gruppo e più di un thread;
- l'encoder, da solo, userebbe meno di tutti i thread per quel folder (peso < N, §5.6):
  altrimenti non c'è niente da sovrapporre (per esempio BZip2, che divide il folder in
  blocchi da 900 KB) e la lettura in memoria sarebbe solo un costo (misurato: −3%);
- non è l'ultimo folder del gruppo mentre nessun altro è in compressione: in quel caso non
  c'è niente da sovrapporre, e il percorso inline mantiene lo streaming (per esempio un
  archivio con un solo blocco solid non viene letto in memoria).

Gli altri folder si comprimono **inline**, esattamente come oggi, dopo aver scritto i job
precedenti (l'ordine nell'archivio non cambia).

### 5.3 Lettura anticipata

Il thread principale legge il folder con un `CFolderInStream` normale in un buffer
(capacità iniziale = dimensione attesa). Raccoglie quindi CRC, dimensioni, tempi, attributi
e `Processed` come oggi, e chiama le callback nello stesso ordine. Il worker legge il buffer
con uno stream che rispetta i confini dei file registrati (§3.5).

Se i file sono cresciuti e i dati superano `2 × kMaxParallelFolderSize`, la lettura si
ferma: il folder si comprime inline, con un input che concatena il buffer già letto e il
resto di `CFolderInStream`. Nessun caso porta a buffer senza limite.

### 5.4 Job e worker

Un job contiene il buffer di input, lo stato di `CFolderInStream` (risultati per file) e,
alla fine, i dati compressi in memoria, il `CFolder`, le `PackSizes` e le
`CoderUnpackSizes` (`Encode_Post`).

Ogni worker ha un proprio `CEncoder` per gruppo di filtri, creato la prima volta e riusato
come fa oggi il ciclo sequenziale. Le opzioni sono le stesse (`CCompressionMethodMode` del
gruppo, `inSizeForReduce` globale), quindi l'output è lo stesso.

### 5.5 Scrittura ordinata

Il thread principale scrive i job **nell'ordine dei folder**: i dati nel flusso
dell'archivio, poi le stesse voci di `newDatabase` che scrive oggi il ciclo sequenziale. La
parte comune "dopo `Encode1`" diventa una funzione unica, usata dal percorso sequenziale e
da quello parallelo, per non duplicarla.

### 5.6 Thread

Sia `N` il numero di thread (`-mmt`). Ogni job ha un **peso**: i thread che l'encoder userà
davvero per un folder di quella dimensione. La somma dei pesi dei job in corso resta
`≤ N` (almeno un job alla volta).

| Metodo | Peso per un folder di dimensione `s` |
|---|---|
| LZMA2 | `lzmaThreads × min(blockThreads, ⌈s / blockSize⌉)` (come `Lzma2EncProps_Normalize`) |
| LZMA | `lzmaThreads` (1 o 2) |
| zstd | `min(N, ⌈s / jobSize⌉)` con `nbWorkers > 0`, altrimenti 1; `jobSize = 2^max(20, windowLog + 2)` come in ZSTDMT. Nei `.7z` l'encoder non conosce la dimensione dei dati (design 0002, §2), quindi `windowLog` è quello del livello |
| BZip2 | `min(threads, ⌈s / blockSize⌉)` |
| PPMd, Deflate, Deflate64, Copy | 1 |
| filtri (BCJ, Delta, ARM64…), AES | 0 (costo trascurabile, nello stesso thread) |
| altro / esterno | non parallelo |

Il pool ha `N` worker; il limite effettivo è il peso.

### 5.7 Memoria

Costo di un job = buffer di input (dimensione del folder) + buffer di output (stima:
input + 1/16 + 64 KiB) + memoria dell'encoder, stimata per metodo con le stesse proprietà
(dizionario ridotto da `inSizeForReduce`, thread):

| Metodo | Stima |
|---|---|
| LZMA / LZMA2 | `Get_Lzma_MemUsage()` con il dizionario ridotto come in `LzmaEncProps_Normalize()`. LZMA2 con più thread di blocco (calcolati sul `reduceSize` globale, come in `Lzma2EncProps_Normalize()`) passa da `MtCoder`: per ogni blocco attivo un buffer di input e chunk di output da `blockSize`, con la stessa formula di `CHandler::SetMainMethod()` |
| zstd | `ZSTD_estimateCStreamSize_usingCParams()` (API sperimentale, confinata in `NZstdLib::EstimateEncoder`) + 2 × `jobSize` per worker attivo |
| PPMd | memoria del modello ridotta come in `CEncProps::Normalize()` (regola replicata: `7zUpdate` non deve dipendere dall'encoder PPMd) + 1 MiB |
| BZip2 | 14 MiB per thread (`CThreadInfo::Alloc()` dimensiona per blocchi da 900 KB) |
| Deflate/Deflate64 | costante (alcuni MiB) |

Il budget è `MemoryUsageLimit`, con `CMemoryBudget` di 0001. Un job con costo superiore a
limite/4 non è parallelo.

**Slot di encoder.** Un `CEncoder` tiene la sua memoria (hash del match finder, buffer)
anche dopo aver finito un folder, per riusarla con il successivo, come il ciclo
sequenziale. Contare la memoria dell'encoder solo mentre un job è in corso non basterebbe:
con N encoder inattivi la memoria reale supererebbe il budget (prima versione: 12 encoder
LZMA2 `-mx5` inattivi da ~175 MB). Quindi:

- ci sono N **slot**, ognuno con un `CEncoder`; un job usa uno slot libero (il più piccolo
  con memoria sufficiente, altrimenti il più grande);
- la memoria dell'encoder resta **riservata per lo slot** (il massimo delle stime dei
  folder che ha compresso), non per il job; i buffer di input e di output sono del job;
- se la memoria non basta, prima si liberano gli slot inattivi (un nuovo `CEncoder` ricrea
  i coder al folder successivo), poi si attende la fine dei job in corso.

Verificato: Silesia non solid (`-mx5`, 20 thread): con `-mmemuse=512m` e `1g` tutti i
folder sono "grandi" e vanno inline (RSS uguale al sequenziale, ~390 MB); con `2g` vanno
in parallelo con RSS 1,35 GB.

### 5.8 Avanzamento e annullamento

I worker sommano i byte letti e scritti in contatori atomici; il thread principale chiama
`lps->SetRatioInfo()` mentre attende un posto o un job. Se la callback restituisce
`E_ABORT` (annullamento), il thread principale smette di accodare, attende i job in corso e
restituisce l'errore.

### 5.9 Errori ed eccezioni

Il primo errore (di lettura, di un encoder, di scrittura) ferma la pianificazione; si
attendono i job in corso, si scartano i risultati, si restituisce l'errore. Le eccezioni nei
worker sono catturate da `CMtTaskPool` (come in 0001) e diventano `E_FAIL`.

### 5.10 Configurazione

Proprietà `pfc` (parallel folder compression), **attiva di default** perché l'output non
cambia: `-mpfc=off` torna al ciclo sequenziale di upstream.

### 5.11 Organizzazione del codice

- `CMtTask`, `CMtTaskPool` sono generiche: spostate da `7zExtractMt.{h,cpp}` a
  `CPP/7zip/Common/MtTaskPool.{h,cpp}`, usate da estrazione e compressione.
- `7zUpdateMt.{h,cpp}`: parti verificabili da sole, con test unitari
  (`tests/unit_7zupdate_mt.cpp`): stima di thread e memoria degli encoder
  (`EstimateFolderEncodeCost`) e `CFolderReplayInStream`.
- `7zUpdate.cpp`: `AddNewFolderFiles()` (la parte dopo `Encode1`) ed
  `EncodeNewFolderInline()` (il corpo del ciclo di upstream), comuni ai due percorsi;
  `CFolderEncodeTask` e `CParallelFolderEncoder` (come `CParallelExtract` in
  `7zExtract.cpp`), perché usano le funzioni statiche del modulo.

## 6. Piano di test

1. **Equivalenza byte per byte** (`tests/update_equivalence.py`, CTest): archivi creati con
   `pfc=off` e `pfc=on`, confrontati con `cmp`, e output testuale di `7zz a -bb3` (ordine dei
   file) confrontato. Matrice:
   - metodi: LZMA2 (`-mx1/5/9`), LZMA, zstd (più livelli), PPMd, BZip2, Deflate, Deflate64,
     Copy, BCJ2, filtri (`-mf=Delta:4`, ARM64), `-qs`;
   - suddivisione: `-ms=off`, `-ms=e`, `-ms=1m`, `-ms=16m`, solid di default; file vuoti,
     directory, file più grandi di 64 MiB in mezzo a file piccoli;
   - thread: `-mmt=1`, `2`, `4`, tutti; memoria bassa (`-mmemuse`);
   - file che non si aprono (permessi);
   - aggiornamento di un archivio esistente (aggiunta di file, con e senza ricompressione).
2. **Roundtrip** (`roundtrip.sh`) con i profili che hanno molti folder, anche cifrati.
3. **Sanitizer**: la suite sotto ASan+UBSan e TSan.
4. **Prestazioni** (`tools/bench`): tempi di `a` con `pfc=off/on` per i casi di §1.

## 7. Alternative considerate

| Alternativa | Perché scartata |
|---|---|
| Worker che leggono i file da soli (callback da più thread, sotto lock) | Le callback dei folder si mescolerebbero (ordine dei messaggi, più stream aperti insieme); il comportamento dell'interfaccia cambierebbe. La lettura in un thread non è il collo di bottiglia: si legge molto più velocemente di quanto si comprima. |
| Meno thread per encoder (es. 1) per avere più job | Cambierebbe l'output (§3.4) e rallenterebbe i folder grandi. |
| Output dei job su file temporanei invece che in memoria | Inutile con folder fino a 64 MiB e budget di memoria; più I/O. |
| Cambiare la suddivisione in folder per avere più parallelismo | Cambia gli archivi e il rapporto di compressione: è una scelta dell'utente (`-ms`). |
| Parallelizzare anche la ricompressione dei folder esistenti | Utile, ma con vincoli diversi (decoder + encoder per job, password): in un secondo momento. |

## 8. Rischi

- **Stime di memoria imprecise**: sono per eccesso; in caso di `E_OUTOFMEMORY` di un job si
  restituisce l'errore come oggi.
- **Peso dei thread stimato male**: rallenta o sovraccarica, ma non cambia l'output.
- **Callback con stato** che si aspettano compressione subito dopo la lettura: nessuna in
  7-Zip (la console e la GUI registrano solo i nomi e gli esiti).

## 9. Piano di lavoro

1. Spostare `CMtTask` / `CMtTaskPool` in `CPP/7zip/Common/MtTaskPool.{h,cpp}` (nessun
   cambio di comportamento: suite di 0001).
2. Funzione comune "dopo `Encode1`" in `7zUpdate.cpp` (archivi identici).
3. Stime di thread e memoria (`7zUpdateMt.cpp`) con test unitari.
4. `CParallelFolderEncoder` e proprietà `pfc`.
5. Test di equivalenza, sanitizer, misure; documentazione.

## 10. Risultati

i7-12700H (20 thread logici), WSL2, binario `build` (Release, GCC 13, senza assembly),
`tools/bench/compress-bench.py -r 2`, dati in `bench-data/`, archivi su `/dev/shm`. Tempo
reale (minimo delle ripetizioni), "core" = (user + sys) / reale. In tutti i casi gli archivi
con `pfc=on` sono **identici byte per byte** a quelli con `pfc=off` (gli archivi cifrati
non si confrontano: IV casuali).

| Caso | Folder | `-mmt` | off (s) | on (s) | Speedup | Core off → on | RSS off → on (MB) |
|---|---|---|---|---|---|---|---|
| Silesia non solid (LZMA2 `-mx5`) | 12 | 20 | 40,09 | 19,10 | **2,10×** | 1,4 → 5,5 | 381 → 2100 |
| Silesia non solid | 12 | 4 | 40,24 | 28,73 | 1,40× | 1,4 → 2,2 | 381 → 834 |
| Corpus non solid (1285 file) | 1285 | 20 | 6,91 | 4,13 | **1,67×** | 1,7 → 3,7 | 384 → 654 |
| Corpus `-ms=8m` | 6 | 20 | 6,97 | 4,36 | 1,60× | 1,7 → 3,3 | 384 → 642 |
| Corpus `-ms=e` | 990 | 20 | 6,51 | 5,60 | 1,16× | 1,7 → 2,5 | 384 → 663 |
| Silesia LZMA non solid | 12 | 20 | 40,11 | 19,01 | **2,11×** | 1,4 → 5,5 | 381 → 2099 |
| Silesia PPMd `-ms=16m` | 10 | 20 | 11,44 | 5,05 | **2,26×** | 1,0 → 4,2 | 23 → 384 |
| Silesia PPMd `-ms=16m` | 10 | 4 | 11,45 | 4,77 | 2,40× | 1,0 → 3,2 | 24 → 231 |
| Silesia zstd `-mx3 -ms=16m` | 10 | 20 | 0,45 | 0,26 | **1,75×** | 2,1 → 5,4 | 79 → 270 |
| Silesia zstd `-mx3 -ms=16m` | 10 | 4 | 0,45 | 0,47 | 0,95× | 1,9 → 1,9 | 71 → 93 |
| Silesia BZip2 `-ms=16m` | 10 | 20 | 3,11 | 3,11 | 1,00× | 14,6 → 14,7 | 207 → 208 |
| Silesia Deflate `-ms=16m` | 10 | 20 | 16,90 | 5,07 | **3,34×** | 1,0 → 4,6 | 7 → 247 |
| Silesia AES non solid | 12 | 20 | 40,45 | 19,08 | **2,12×** | 1,4 → 5,5 | 383 → 2118 |
| Silesia solid (default) | 1 | 20 | 25,09 | 25,16 | 1,00× | 2,6 → 2,6 | 907 → 907 |
| Corpus solid (default) | 2 | 20 | 7,19 | 7,20 | 1,00× | 1,7 → 1,7 | 384 → 384 |

Le righe BZip2 e zstd con 4 thread sono misurate dopo la regola "peso < N" di §5.2 (senza:
BZip2 0,97×).

Osservazioni:

- **Limite del folder più grande.** Silesia non solid è limitata da `mozilla` (51 MiB):
  LZMA2 `-mx5` usa blocchi da 64 MiB, quindi il file è un solo blocco compresso da 2 thread
  (circa 19 s) sia in sequenza sia in parallelo. Il resto dei folder si sovrappone a lui.
  Non si supera senza cambiare l'output (blocchi più piccoli: scelta dell'utente, `-m0=lzma2:c=`).
- **Gruppi di filtri in sequenza.** Nel corpus il gruppo degli eseguibili (filtro BCJ:
  `bin/nvdisasm`, 50 MiB, circa 3 s da solo) e quello dei testi erano compressi uno dopo
  l'altro: la coda di un gruppo non si sovrapponeva al successivo. Risolto con la
  pipeline tra gruppi (§10.2).
- **BZip2** divide già il folder in blocchi da 900 KB: nessun guadagno, e grazie alla
  regola di §5.2 nessuna perdita.
- **Solid di default**: stesso tempo e stessa memoria (§5.2, ultimo folder inline).
- **Memoria**: entro il limite (§5.7, slot di encoder). Con il limite di default (80% della
  RAM, ~25 GB qui) i 12 encoder LZMA2 di Silesia tengono ~175 MB ciascuno.

### 10.1 Verifica

- `tests/update_equivalence.py` (CTest `update_equivalence_pfc`): 58 casi equivalenti.
  Non è "vuota": un contatore temporaneo ha confermato il percorso parallelo in tutti i
  profili tranne Copy e BCJ2 (inline per scelta) e il solid di default; due mutazioni
  temporanee sono rilevate (scrittura dei folder fuori ordine: decine di casi; encoder dei
  worker con 1 thread: tutti i casi zstd, come previsto da §3.4, mentre LZMA2, LZMA, BZip2,
  PPMd e Deflate restano identici).
- `tests/unit_7zupdate_mt.cpp`: stime per metodo e granularità del replay (55 controlli).
- ASan+UBSan, TSan (GCC 13 e Clang 20): suite completa pulita. Il nuovo test ha fatto
  emergere una race del codice originale in `C/MtCoder.c` (corretta, commit separato).
- Makefile originali: `Alone7z` e `Alone2` compilano e producono archivi identici.
- File che cambiano dimensione tra la scansione e la lettura: `tests/update_growing.cpp`
  crea l'archivio con l'handler 7z e una callback che dichiara una dimensione e fornisce
  dati di un'altra (cresciuto di poco, cresciuto da 1 a 140 MiB, accorciato).
  `tests/update_growing.py` (CTest) verifica archivi identici con `pfc=off/on`, integrità e
  dimensione reale nell'archivio. Una traccia temporanea ha confermato i due rami: riserva
  della dimensione reale, e lettura anticipata fermata a 128 MiB con il resto inline.

### 10.2 Pipeline tra gruppi di filtri

Un solo `CParallelFolderEncoder` per tutta l'operazione: i folder di un gruppo (per esempio
i testi) si comprimono mentre finiscono quelli del gruppo precedente (gli eseguibili, con
il filtro BCJ). `BeginGroup()` registra una copia del metodo del gruppo; ogni slot di
encoder ricorda il gruppo del suo encoder, e un job preferisce gli slot del proprio gruppo,
altrimenti prende uno slot libero di un altro gruppo e ne ricrea l'encoder (rilasciando la
memoria). I job copiano i propri indici dei file, perché il folder può essere scritto dopo
la fine del ciclo del suo gruppo. Restano gli svuotamenti obbligati: prima dei folder
esistenti di un gruppo (copiati o ricompressi direttamente nell'archivio) e prima di un
folder inline. La regola dell'ultimo folder vale per l'ultimo folder nuovo dell'operazione.

| Caso (20 thread) | Senza pipeline | Con pipeline |
|---|---|---|
| Corpus non solid | 1,67× | **1,90×** |
| Corpus `-ms=8m` | 1,60× | **1,91×** |
| Corpus `-ms=e` | 1,16× | 1,26× |
| Corpus solid di default (2 folder: eseguibili + testi) | 1,00× | **1,37×** |

Con `-ms=e` alcuni gruppi per estensione superano 64 MiB e vanno inline, svuotando la
pipeline. La memoria cresce (corpus non solid: 654 → 1152 MB) restando nel limite.

### 10.3 Possibili sviluppi

- Ricompressione parallela dei folder esistenti durante un aggiornamento.
