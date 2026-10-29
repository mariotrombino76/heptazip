# 0001 — Estrazione parallela dei folder `.7z`

| | |
|---|---|
| **Stato** | Proposta, da rivedere |
| **Data** | 2026-10-29 |
| **Fase** | 2: parallelizzazione della decompressione |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`) |
| **File coinvolti** | `CPP/7zip/Archive/7z/7zExtract.cpp`, `7zHandler.{h,cpp}`, `7zHandlerOut.cpp`, nuovi `7zExtractMt.{h,cpp}` |

---

## 1. Contesto e problema

Un archivio `.7z` è diviso in **folder**: unità di compressione indipendenti, chiamate
"blocchi solid" nella UI. Ogni folder si decomprime senza dipendere dagli altri.
`CHandler::Extract` (`7zExtract.cpp:306`) però li elabora **uno alla volta**, nel thread
chiamante.

Dentro un singolo folder c'è già del parallelismo:

- **LZMA2:** `Lzma2DecMt` decomprime in parallelo i blocchi LZMA2 indipendenti, ma solo
  se lo stream è stato creato a blocchi (vedi §1.2).
- **Catene di filtri:** il mixer MT (`CoderMixer2`) esegue ogni coder della catena
  (es. BCJ2 + LZMA) in un proprio thread, che però resta in larga parte in attesa.

### 1.1 Misure (i7-12700H, 20 thread logici, binario `build-asm`)

| Archivio | Folder | `t -mmt=1` | `t -mmt=20` | CPU con 20 thread |
|---|---|---|---|---|
| Silesia non-solid `-mx5 -ms=off` | 12 | 1,43 s | 1,42 s | 105% |
| Corpus misto `-mx5 -ms=off` | 1285 | 0,25 s | 0,29 s | 105% |
| Corpus misto `-mx5 -ms=e` | 990 | non misurato | non misurato | — |
| Corpus misto `-mx5` (default) | 2 | 0,24 s | 0,25 s | 105% |

Con più folder la CPU non supera mai un core. Per Silesia non-solid il limite teorico
è il tempo del folder più grande (`mozilla`, 48,8 MiB su 202 MiB): **circa 0,35–0,45 s,
cioè da 3 a 4 volte più veloce**.

### 1.2 Perché non agire sul formato

Le alternative "lato encoder" (blocchi LZMA2 più piccoli, più folder) sono state
misurate e **scartate come default**. Ogni blocco o folder riparte con il dizionario
vuoto, quindi la dimensione del blocco limita la distanza massima delle ripetizioni:

| Input `-mx9` | Blocco 64 MiB invece del default | Effetto sulla dimensione |
|---|---|---|
| Silesia (202 MiB) | 64m | +0,21% |
| Silesia ×3 (606 MiB, ripetizioni a 202 MiB) | 64m | **+19,9%** |

Questo intervento invece **non tocca il formato**: accelera anche gli archivi già
esistenti e quelli creati da 7-Zip upstream.

### 1.3 Quando conviene

| Tipo di archivio | Folder | Beneficio atteso |
|---|---|---|
| Non-solid (`-ms=off`) | uno per file | alto |
| Solid per estensione (`-ms=e`), dimensione solid limitata (`-ms=64m`) | molti | alto |
| Archivi da molti GB (oltre `numSolidBytes`: 8–16 GiB con LZMA2) | diversi | medio |
| Default sotto qualche GB | 1–2 (eseguibili con BCJ più il resto) | basso; non deve peggiorare |

---

## 2. Obiettivi e non-obiettivi

**Obiettivi**

1. Decomprimere più folder in parallelo durante `Extract` (estrazione e test).
2. **Equivalenza osservabile** con l'estrazione sequenziale (§4.1): stessa sequenza di
   callback, stessi byte scritti, stessi risultati e codici d'errore.
3. Memoria limitata e prevedibile, entro il limite di decompressione (`-mmemuse`).
4. Nessun peggioramento sugli archivi con un solo folder o pochi folder.
5. Diff piccolo e localizzato nei file upstream, per non complicare i merge futuri.

**Non-obiettivi** (per ora)

- Cambiare il formato o il comportamento dell'encoder.
- Parallelizzare dentro un singolo stream LZMA: impossibile per costruzione.
- Rendere `IArchiveExtractCallback` thread-safe o cambiarne il contratto.
- Altri formati (zip, rar, …). Il design deve però poter essere riusato.

---

## 3. Vincoli emersi dall'analisi del codice

| # | Vincolo | Dove | Conseguenza |
|---|---|---|---|
| V1 | `GetStream`, `PrepareOperation` e `SetOperationResult` **non vengono mai chiamati in contemporanea** da più thread. Le chiamate di avanzamento (`IProgress`) possono invece arrivare da altri thread | `IArchive.h:184-192` | Le callback per gli elementi restano su **un solo thread**, nell'ordine originale |
| V2 | `CFolderOutStream` assume una sequenza ordinata di file per folder (salti, file vuoti, CRC) | `7zExtract.cpp:19-213` | Riutilizzarlo così com'è, "riproducendo" su di esso l'output già decompresso |
| V3 | `CDecoder` mantiene uno stato: tiene in cache mixer e codec tra un folder e l'altro (`_bindInfoPrev`) | `7zDecode.cpp:270` | Un `CDecoder` **per ogni worker** |
| V4 | Con un solo pack stream, `Decode` fa `Seek` e `Read` direttamente su `inStream`, **senza lock** | `7zDecode.cpp:540-544` | I worker non possono condividere `_inStream`. Vedi §5.3 |
| V5 | La password si chiede **dentro** `Decode` (`CryptoGetTextPassword`) | `7zDecode.cpp:435` | Va serializzata e chiesta una volta sola (§5.8) |
| V6 | La cache globale delle chiavi AES è già protetta da una critical section | `7zAes.cpp:155-159` | Nessun intervento necessario |
| V7 | Estrazione parziale: il decoder si ferma dopo l'ultimo file richiesto (`unpackSize = curUnpacked`); i file non richiesti producono `GetStream(kSkip)` | `7zExtract.cpp:319-341` | Il piano di estrazione deve calcolare gli stessi valori |
| V8 | In caso di errore (`S_FALSE`, `E_NOTIMPL`, `dataAfterEnd_Error`) i dati già decompressi sono stati scritti e poi si chiama `FlushCorrupted(resOp)` | `7zExtract.cpp:397-436` | Il worker deve restituire byte prodotti, `HRESULT` e flag, così da riprodurre la stessa sequenza |
| V9 | In `HandlerOut.cpp:88` **ogni proprietà che inizia per `mt`** è interpretata come numero di thread; `mtf` è già usata per il mixer MT | `7zHandler.cpp:775`, `7zHandlerOut.cpp:1042` | La nuova proprietà non deve iniziare per `mt` (§5.10) |
| V10 | Nelle build `Z7_ST` (SFX, single-thread) non ci sono thread | ovunque | Il percorso MT esiste solo con `#ifndef Z7_ST` |
| V11 | Il decoder LZMA alloca il dizionario indicato nelle proprietà del coder, che l'encoder riduce già alla dimensione dei dati | `LzmaDec.c:1320`, `7zHandlerOut.cpp:221` | La memoria di un decoder si stima dalle proprietà (§5.6) |
| V12 | Upstream modifica raramente questi file: 1–15 righe per release, un refactoring ampio nella 23.01 | `git log` | Accettabile un aggancio piccolo; il grosso va in file nuovi |

---

## 4. Requisiti

### 4.1 Equivalenza osservabile

Per ogni archivio, selezione di file, modalità (`x`/`t`) e password, la modalità
parallela deve produrre **esattamente** la stessa sequenza di chiamate verso
`IArchiveExtractCallback` della modalità sequenziale:

- `GetStream(index, askMode)` → `PrepareOperation(askMode)` → `Write(...)`, con gli
  stessi byte nello stesso ordine → `SetOperationResult(opRes)`;
- `ReportExtractResult(kBlockIndex, folderIndex, resOp)`;
- lo stesso `HRESULT` finale.

**Differenze ammesse**, perché non osservabili come risultato:

- frequenza e valori intermedi di `SetCompleted` (avanzamento);
- il momento esatto in cui viene chiesta la password (§5.8);
- lavoro di decompressione "sprecato" su folder successivi quando l'estrazione si
  interrompe per un errore o per annullamento.

### 4.2 Prestazioni (criteri di accettazione)

| Caso | Criterio |
|---|---|
| Silesia non-solid, `t -mmt=20` | ≥ 3× più veloce (da 1,42 s a ≤ 0,47 s) |
| Archivi di default (1–2 folder) | nessun peggioramento oltre il rumore (≤ 2%) |
| Estrazione di un solo file da un archivio con molti folder | nessun peggioramento |
| `-mmt=1` | percorso sequenziale identico a oggi |

### 4.3 Memoria

Il picco di memoria aggiuntiva (buffer più decoder dei folder in volo) non supera il
budget `_memUsage_Decompress`, di default 17/32 della RAM, configurabile con
`-mmemuse`. Se il budget non basta per due folder in parallelo, si usa il percorso
sequenziale.

---

## 5. Design proposto

### 5.1 Panoramica

```
thread chiamante (Extract)                              worker 1..W
───────────────────────────────────                     ────────────────────────────
1. pianifica: lista ordinata di job
   (stessa logica di raggruppamento di oggi)
2. ammette job in ordine finché
   memoria in volo + costo ≤ budget   ── coda job ──►   a. legge i byte compressi del
                                                           folder in un buffer, sotto
                                                           lock dell'archivio (una lettura
                                                           sequenziale per folder)
                                                        b. CDecoder::Decode(CBufInStream →
                                                           buffer di output in memoria)
                                                        c. salva risultato: byte prodotti,
                                                           HRESULT, dataAfterEnd, eccezione
3. attende il job "successivo in ordine"  ◄── esito ──
4. lo RIPRODUCE su CFolderOutStream
   (GetStream/Write/SetOperationResult:
   la stessa sequenza del percorso sequenziale)
5. libera la memoria, ammette altri job, ripete
```

Idea chiave: **decomprimere in anticipo e consegnare in ordine**. I worker decomprimono
in memoria; tutto quello che riguarda le callback avviene nel thread chiamante, nell'ordine
originale, riutilizzando `CFolderOutStream` senza modificarlo. L'equivalenza di §4.1 vale
per costruzione.

### 5.2 Pianificazione

Si estrae dall'attuale ciclo di `Extract` (`7zExtract.cpp:306-351`) una funzione che produce
la sequenza ordinata di job:

```cpp
struct CExtractJob
{
  UInt32 FirstIndex;      // i: posizione nella lista indices/allFiles
  UInt32 NumSolidFiles;   // numero di elementi consegnati da questo job
  UInt32 StartFileIndex;  // primo file del folder (o il file stesso, per gli elementi vuoti)
  CNum   FolderIndex;     // kNumNoIndex per file vuoti, directory, anti-item
  UInt64 UnpackSize;      // curUnpacked: byte necessari (estrazione parziale)
  UInt64 PackSize;        // curPacked
};
```

Il percorso sequenziale usa la stessa funzione, così la logica (delicata) di estrazione
parziale esiste **in un solo punto**. È un piccolo refactoring di `7zExtract.cpp`; lo
verifica il test di equivalenza (§6.1) prima di introdurre qualsiasi parallelismo.

### 5.3 Lettura dei dati compressi

**Problema (V4):** con un solo pack stream `Decode` fa `Seek` e `Read` direttamente su
`_inStream`.

**Alternative considerate:**

| Opzione | Pro | Contro |
|---|---|---|
| a) Un file handle per worker (riapertura dell'archivio) | letture davvero parallele | l'handler riceve un `IInStream`, non un percorso; non funziona con volumi multipli, stream in memoria, archivi annidati |
| b) Stream condiviso con lock per ogni `Read`, come `CLockedSequentialInStreamMT` | riusa codice esistente; memoria minima | letture alternate tra i worker, quindi seek casuali (pessimo su HDD); richiede di cambiare `CDecoder::Decode` per forzare il lock anche con un solo pack stream |
| **c) Il worker legge l'intero folder compresso in un buffer sotto lock, poi decomprime da memoria** (`CBufInStream`) | **nessuna modifica a `CDecoder::Decode`**: riceve un `IInStream` in memoria con `startPos = 0`; letture grandi e sequenziali, buone anche su HDD; il lock è tenuto solo durante la copia | memoria aggiuntiva pari alla dimensione compressa (rientra nel budget) |

**Scelta: (c).** `Decode` calcola la posizione di ogni pack stream come
`startPos + packPositions[j]`, con offset assoluti rispetto all'inizio dei dati.
Per riusarlo senza modifiche si usa un piccolo **adattatore di offset**
(`CBufInStreamAt`): un `IInStream` che espone il buffer come se si trovasse
all'offset `startPos + packPositions[0]` del file, così si passa a `Decode` lo stesso
`startPos` di oggi. Non si "compensa" `startPos` con aritmetica modulo 2^64: funzionerebbe,
ma sarebbe un overflow intenzionale e poco leggibile.
I pack stream di un folder sono consecutivi nel file (indici contigui a partire da
`FoStartPackStreamIndex`), quindi anche i folder con più stream (BCJ2) si leggono con una
sola lettura.

### 5.4 Worker e decompressione in memoria

- Un pool di **W worker**. Ognuno ha il proprio `CDecoder` (V3), riusato tra i job:
  si conserva il vantaggio della cache del mixer.
- L'input è un `CBufInStream` sul buffer compresso.
- L'output è un nuovo `CFolderBufOutStream` sul buffer preallocato di `UnpackSize`
  byte. **Non** si usa `CBufPtrSeqOutStream`: quando il buffer è pieno restituisce
  `E_FAIL`, mentre `CFolderOutStream` restituisce `k_My_HRESULT_WritingWasCut`. Lo stream
  dedicato replica quella semantica, perché il mixer la gestisce in modo specifico.
- Per i parametri `mtMode`, `numThreads` e `memUsage` di ogni job vedi §5.7.
- Il worker cattura **tutte** le eccezioni (`CNewException`, `CSystemException`, `...`)
  e le converte nel risultato del job (§5.9). Nessuna eccezione deve uscire dal thread.

Risultato di un job:

```cpp
struct CExtractJobResult
{
  HRESULT DecodeRes;          // valore restituito da CDecoder::Decode
  bool DataAfterEnd_Error;
  bool ExceptionCaught;       // eccezione C++ nel worker
  HRESULT ExceptionRes;       // E_OUTOFMEMORY / E_FAIL / ...
  size_t OutSize;             // byte effettivamente prodotti (≤ UnpackSize)
};
```

### 5.5 Consegna ordinata (replay)

Il thread chiamante:

1. attende il risultato del **job successivo nell'ordine**, non del primo che finisce;
2. esegue `folderOutStream->Init(...)` esattamente come oggi;
3. scrive i `OutSize` byte prodotti su `CFolderOutStream`, a blocchi. Questo genera
   `GetStream`, `PrepareOperation`, `Write`, `SetOperationResult` e il controllo CRC
   come in sequenziale. I blocchi hanno la stessa granularità che `CFolderOutStream`
   applica già al CRC (`k_Step = 1 MiB`);
4. applica la **stessa** logica di gestione del risultato di `7zExtract.cpp:397-436`,
   che per questo va estratta in una funzione condivisa con il percorso sequenziale:
   `FlushCorrupted`, `ReportExtractResult`, `return result`, `throw` →
   `return ExceptionRes`;
5. libera i buffer e ammette nuovi job.

Il **blocco in testa alla coda** (un folder lento trattiene quelli successivi già pronti) è
il prezzo dell'ordine. Lo limita il controllo d'ammissione sulla memoria (§5.6).

### 5.6 Memoria e ammissione dei job

Costo stimato di un job:

```
C(job) = PackSize + UnpackSize + DecoderMem(folder) + overhead fisso (~1 MiB)
```

`DecoderMem` si ricava dalle proprietà dei coder (V11): dizionario per LZMA/LZMA2,
memoria del modello per PPMd, valori fissi prudenti per BZip2, Deflate e filtri, e un
valore prudente per i coder sconosciuti o esterni. Una funzione
`EstimateFolderDecodeMemory()` nel nuovo modulo, con test unitari dedicati.

Regole:

- **Budget:** `B = _memUsage_Decompress`.
- **Folder "grande":** `C(job) > B / 4`. Non viene decompresso in anticipo: quando arriva
  il suo turno la pipeline si svuota (si consegnano i job precedenti) e il folder si
  elabora con il **percorso sequenziale di oggi**, in streaming verso il disco e con tutti
  i thread a disposizione del decoder (LZMA2 multi-thread). Il valore `B/4` è un primo
  valore da tarare con le misure (§6.4).
- **Ammissione:** i job vengono ammessi **nell'ordine del piano** finché
  `memoria in volo + C(job) ≤ B`. Non si salta avanti, così l'occupazione resta
  prevedibile e il blocco in testa alla coda limitato.
- **Attivazione:** il percorso parallelo si usa solo se:
  - `!Z7_ST`;
  - `pfd` è attivo (§5.10);
  - `_numThreads > 1`;
  - il piano contiene almeno **2 folder non grandi**.

  Altrimenti si usa il ciclo sequenziale attuale.

### 5.7 Ripartizione dei thread

`N = _numThreads`, da `-mmt`, di default pari ai core.

- **Job in parallelo:** il decoder riceve `numThreads = 1`. Il parallelismo è tra folder:
  si evita di avere troppi thread e si risparmia la memoria dei buffer di `Lzma2DecMt`.
- **Numero di worker:** `W = min(N, numero di job ammissibili)`.
- **Folder grandi** (percorso sequenziale): `numThreads = N`, come oggi.
- **Mixer MT:** le catene di filtri (BCJ2: 4 coder) aggiungono thread per ogni job, ma
  quelli dei filtri restano quasi sempre in attesa. Lo si verifica con le misure; se
  serve, nei worker si usa il mixer ST (`_useMultiThreadMixer = false`).

### 5.8 Password

Il callback della password (`ICryptoGetTextPassword`) verrebbe invocato da un worker
(V5), e nella UI console può mostrare una richiesta interattiva. Soluzione:

- un adattatore `CPasswordCache` (implementa `ICryptoGetTextPassword`) passato ai worker;
- la **prima** volta che si ammette un job su un folder cifrato (coder AES nel folder),
  il **thread chiamante** chiede la password al callback reale e la memorizza;
- i worker ricevono la password già pronta, senza mai interagire con la UI;
- la stringa viene cancellata dalla memoria in modo sicuro (`UString_Wipe`,
  `CMyComBSTR_Wipe`), come nel codice esistente.

Differenza rispetto al sequenziale: la richiesta può arrivare prima che sia stato
consegnato il folder cifrato, ma mai prima che sia necessaria (§4.1).

### 5.9 Errori, annullamento, eccezioni

| Evento | Comportamento |
|---|---|
| Errore di decodifica in un job (`S_FALSE`, `E_NOTIMPL`, `dataAfterEnd`) | registrato nel risultato e gestito al replay, identico al sequenziale |
| `HRESULT` di errore "grave" da `Decode` (es. `E_OUTOFMEMORY`) | al replay: `return result`, come oggi; gli altri job vengono interrotti |
| Eccezione nel worker | catturata e convertita; al replay `FlushCorrupted(kDataError)` e poi lo stesso esito del `throw` sequenziale (`E_OUTOFMEMORY` per `CNewException`) |
| Errore da una callback durante il replay (disco pieno, `E_ABORT`) | si interrompe il replay, si segnala l'annullamento ai worker, si attende la loro fine e si restituisce lo stesso `HRESULT` |
| Annullamento dell'utente (`SetCompleted` → `E_ABORT`) | come sopra; i worker si fermano al primo controllo di avanzamento |
| Memoria esaurita in fase di ammissione | si riduce la finestra; nel caso peggiore si torna al percorso sequenziale per i job rimanenti |

Annullamento cooperativo: un flag atomico, controllato dal `ICompressProgressInfo` di
ogni worker, che in quel caso restituisce `E_ABORT`. Il thread chiamante attende
**sempre** la fine di tutti i worker prima di uscire da `Extract`: nessun thread
sopravvive alla chiamata.

### 5.10 Configurazione

- **Nuova proprietà dell'handler 7z: `pfd`** ("parallel folder decoding"), booleana,
  attiva di default dopo la validazione (§7, passo 5). Uso: `7zz x -mpfd=off …`.
  Il nome evita il prefisso `mt` (V9) e non coincide con proprietà esistenti
  (controllato con `grep`). Va gestita sia in `7zHandler.cpp` (build `Z7_EXTRACT_ONLY`)
  sia in `7zHandlerOut.cpp`.
- `-mmt=N` e `-mmemuse` mantengono il loro significato: numero massimo di thread e
  budget di memoria.

### 5.11 Organizzazione del codice

| File | Contenuto | Modifiche upstream |
|---|---|---|
| `7zExtractMt.h/.cpp` (**nuovi**) | pool di worker, coda dei job, ammissione e memoria, `CFolderBufOutStream`, `CPasswordCache`, `EstimateFolderDecodeMemory` | nessuna |
| `7zExtract.cpp` | estrazione della pianificazione (§5.2) e della gestione risultati (§5.5.4) in funzioni condivise; scelta del percorso parallelo | piccole e localizzate |
| `7zHandler.h/.cpp`, `7zHandlerOut.cpp` | campo `_parallelFolderDecoding` e parsing di `pfd` | poche righe |
| `CMakeLists.txt`, `cmake/SevenZipSources.cmake` | nuovo sorgente | solo nel fork |

Per mantenere l'ordine di link (e quindi l'ordine di registrazione dei codec), il nuovo
file va aggiunto a `SevenZipSources.cmake` subito dopo `7zExtract.cpp`. Va aggiunto anche
ai makefile upstream (`Arc_gcc.mak`, `Format7z*`), così anche la build con i makefile
include la funzionalità.

### 5.12 Primitive di threading

| Opzione | Pro | Contro |
|---|---|---|
| **A) Wrapper 7-Zip** (`NWindows::CThread`, `NSynchronization::CCriticalSection`, `CAutoResetEvent`, `CSemaphore`) | coerente con tutto il codice esistente, che non usa mai `std::`; stessa base su Windows e Linux; gestione dei gruppi di processori su Windows | API a eventi più verbosa; condition variable non disponibili come primitiva |
| **B) C++17 standard** (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`) | coda bloccante più semplice e più facile da verificare; RAII nativo; in linea con l'obiettivo di modernizzazione (fase 1) | prima introduzione di `std::` nel codice; da verificare la compatibilità con le eccezioni e il `new` personalizzato di 7-Zip (`NewHandler.h`) |

**Raccomandazione: A.** La regola del progetto (CLAUDE.md, punto 4) chiede compatibilità con
l'architettura esistente, e qui il guadagno di chiarezza di B è modesto. Il modulo nuovo
userà comunque wrapper RAII ed eviterà la gestione manuale delle risorse.
**Da decidere insieme** (§8, D1).

---

## 6. Piano di test

### 6.1 Equivalenza (il test principale)

Un **programma di test** (`tests/extract_equivalence.cpp`) che usa direttamente l'handler
7z (`NArchive::N7z::CHandler`) e un `IArchiveExtractCallback` che **registra** ogni chiamata.
Per ogni `Write` registra l'hash dei byte per file, non i byte stessi. Esegue `Extract` con
`pfd=off` e con `pfd=on` e confronta le due tracce **in modo esatto**.

Matrice di casi:

- tipo di archivio: solid, non-solid, `-ms=e`, `-ms=8m` (molti folder), BCJ2,
  PPMd, BZip2, Deflate, copia, misto, file vuoti, directory, anti-item;
- selezione: tutti, sottoinsiemi (primo, ultimo, file sparsi, file in mezzo a un folder);
- modalità: estrazione e test;
- cifratura: password corretta, password errata, intestazioni cifrate (`-mhe`);
- **archivi corrotti**: byte invertiti nel folder *k* (primo, centrale, ultimo), folder
  troncato, metodo non supportato (ID di coder modificato), dati dopo la fine;
- thread: `-mmt=2`, `-mmt=N`, budget di memoria basso (`-mmemuse`), che forza la finestra
  minima e il ritorno al sequenziale.

Si integra con **CTest** (`ctest`) nella build CMake. Sarà la prima suite di test
automatici del fork.

### 6.2 Test end-to-end

- `tools/bench/roundtrip.sh` si estende con i profili `7z-nonsolid` (già presente),
  `7z-solid-ext` (`-ms=e`), `7z-smallsolid` (`-ms=8m`) e `7z-nonsolid-aes`.
- Confronto tra l'output testuale di `7zz t -bb3` e di `7zz x` con `pfd=off`/`on`.

### 6.3 Sanitizer e stress

- Nuova opzione CMake `SEVENZIP_SANITIZE=address|thread|undefined`.
- Suite di equivalenza e roundtrip eseguite con **ThreadSanitizer** e
  **AddressSanitizer** + **UBSan**.
- Stress: 1000 ripetizioni di estrazione di un archivio da 10.000 folder con annullamento
  a istanti casuali, in un test dedicato: nessun deadlock, nessun leak, nessun thread
  orfano.

### 6.4 Prestazioni

Un nuovo script (`tools/bench/extract-bench.py`, sullo stile di `block-sweep.py`) misura
tempo e RSS di `t` e `x` (su `/dev/shm`) per:

- Silesia non-solid;
- corpus `-ms=e`;
- corpus `-ms=8m`;
- archivio di default;
- estrazione di un solo file da un archivio con molti folder;

con `pfd=off`/`on` e `-mmt=1`/`4`/`20`, riportando il carico della macchina. Gli stessi
dati servono a tarare la soglia "folder grande" (§5.6) e a verificare i criteri di §4.2.

---

## 7. Piano di implementazione (incrementale)

Ogni passo è un commit separato, con i test che passano.

1. **Infrastruttura di test:** CTest, `extract_equivalence` (che inizialmente confronta il
   sequenziale con se stesso, per validare lo strumento), opzione sanitizer, nuovi profili
   di roundtrip. *Nessuna modifica a 7-Zip.*
2. **Refactoring senza cambi di comportamento:** pianificazione (§5.2) e gestione dei
   risultati (§5.5.4) estratte in funzioni. Verifica: test di equivalenza contro il
   **binario upstream**, `compare-archives.sh` e roundtrip.
3. **Modulo `7zExtractMt`:** stima della memoria (con test unitari), coda e ammissione,
   `CFolderBufOutStream`, `CPasswordCache`.
4. **Percorso parallelo dietro `pfd`, disattivato di default.** Equivalenza con tutta la
   matrice, TSan, ASan, stress.
5. **Misure e taratura** (soglia folder grande, mixer ST/MT nei worker), poi **`pfd`
   attivo di default** se i criteri di §4.2 sono soddisfatti.
6. *(Evoluzioni successive, ognuna da misurare)*: sovrapporre lo streaming del folder
   grande alla decompressione in anticipo dei folder piccoli successivi; CRC calcolato
   nei worker; buffer su file temporaneo invece che in RAM per folder medio-grandi.

---

## 8. Decisioni da prendere

| # | Domanda | Proposta |
|---|---|---|
| D1 | Primitive di threading: wrapper 7-Zip o C++17 standard (§5.12) | Wrapper 7-Zip |
| D2 | `pfd` attivo di default dopo la validazione? | Sì, se i criteri di §4.2 sono soddisfatti |
| D3 | Nome della proprietà: `pfd`? | `pfd` |
| D4 | Il test di equivalenza come eseguibile C++ con CTest (più preciso) o solo come script sull'output di `7zz -bb3` (più semplice, meno preciso)? | Eseguibile C++ |
| D5 | Aggiungere il nuovo file anche ai makefile upstream (§5.11)? | Sì, così le due build restano equivalenti |

---

## 9. Rischi

| Rischio | Mitigazione |
|---|---|
| Divergenze sottili dal comportamento sequenziale su archivi corrotti o parziali | Equivalenza per costruzione (replay su `CFolderOutStream` invariato) e matrice di test con corruzioni |
| Deadlock o race condition | Annullamento cooperativo, attesa garantita dei worker, TSan, stress test |
| Memoria oltre le attese (stima del decoder errata) | Stima prudente, test unitari, misura dell'RSS nei benchmark, budget configurabile |
| Peggioramento su archivi di default | Attivazione solo con ≥ 2 folder non grandi; criterio di non regressione in §4.2 |
| Conflitti di merge con upstream | Logica nei file nuovi; in `7zExtract.cpp` solo l'estrazione di due funzioni e l'aggancio |
