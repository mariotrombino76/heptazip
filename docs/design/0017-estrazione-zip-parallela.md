# 0017 — Estrazione parallela dei file degli archivi zip

| | |
|---|---|
| **Stato** | Implementato, attivo di default (issue #86); misure in §7 |
| **Data** | 2026-11-05 |
| **Fase** | 2: parallelizzazione della decompressione |
| **Codice** | `CPP/7zip/Archive/Zip/ZipExtractMt.{h,cpp}` (nuovi), `ZipDecoder.h` (dichiarazioni estratte da `ZipHandler.cpp`), `ZipHandler.{h,cpp}`, `ZipHandlerOut.cpp` (proprietà `pfd`), `ZipIn.h`; riusa `7z/7zExtractMt.h` e `Common/MtTaskPool`; test `tests/zip_extract_equivalence.py`, `tests/extract_trace.cpp -z` |

---

## 1. Contesto e problema

`CHandler::Extract` di `ZipHandler.cpp` decodifica gli elementi di uno zip **uno alla
volta nel thread chiamante**: per ogni elemento legge l'header locale, chiede lo stream di
uscita, decodifica, riferisce il risultato. Uno zip non ha blocchi solid: ogni file è un
flusso compresso indipendente, con il proprio metodo e la propria cifratura, quindi la
decodifica si può distribuire sui core esattamente come i folder dei `.7z` (design 0001).

Misure di partenza (i7-12700H, 20 thread logici, archivi in `/dev/shm`, `-tzip -mx5`):

| Archivio | `t -mmt=1` | `t -mmt=20` | CPU |
|---|---|---|---|
| silesia (12 file, 203 MB) | 0,25 s | 0,25 s | un core |
| corpus AES-256 (1285 file, 110 MB) | 0,45 s | 0,45 s | un core |

## 2. Obiettivi e non-obiettivi

- **Equivalenza osservabile**: chi chiama `Extract` (riga di comando, GUI, gestori di
  archivi, `extract_trace`) deve vedere le stesse callback nello stesso ordine con gli
  stessi argomenti e risultati dell'estrazione sequenziale: stessi stream richiesti,
  stesso `PrepareOperation`, stesse scritture (dati identici), stesso
  `SetOperationResult` per elemento, stesso esito finale, anche su archivi corrotti,
  con selezioni parziali, in modalità test e con password sbagliata.
- **Nessuna modifica al formato**: accelera gli zip esistenti, creati da chiunque.
- **Memoria limitata** dal budget di decompressione (`-mmemuse`), come il 7z.
- Non-obiettivo: parallelizzare la decodifica *di un singolo* file (un Deflate o uno
  ZipCrypto sono flussi seriali); non-obiettivo: cambiare l'ordine di scrittura dei file.

## 3. Design

### 3.1 Schema

Lo stesso del design 0001, con i suoi mattoni (`N7z::CMemoryBudget`, `CSharedInStream`,
`CBufInStreamAt`, `CCachedPassword`, `CMtTaskPool`):

1. **Pianificazione** (`CParallelZipExtract::Plan`), nel thread chiamante: copia
   dell'elemento, lettura dell'header locale (`Read_LocalItem_After_CdItem`) e controlli
   di offset come nel ciclo sequenziale, decisione se l'elemento è decodificabile da un
   worker (§3.3), costo in memoria = pack + unpack + memoria del decoder.
2. **Worker** (`CItemDecodeTask::Decode`): legge i byte compressi dell'elemento in un
   buffer con `CSharedInStream::ReadFullAt` (una lettura sotto lock dello stream
   dell'archivio), li decodifica in un buffer di uscita di dimensione dichiarata con il
   `CZipDecoder` del worker, registra HRESULT, codice dell'operazione e byte prodotti.
   `CZipDecoder::Decode` ha un parametro in più, lo stream compresso già pronto
   (`packStreamIn`): senza, legge l'archivio come prima.
3. **Riproduzione ordinata** (`ReplayItem`), nel thread chiamante: `GetStream`, i casi
   speciali del ciclo originale (header non disponibile, directory, elementi non
   leggibili), `PrepareOperation`, scrittura del buffer a blocchi da 1 MiB,
   `SetOperationResult` con la stessa correzione per `headersError`.

### 3.2 Lo stream dell'archivio non è condivisibile

`CInArchive` legge con la posizione dello stream (seek + read): le letture degli header
locali in pianificazione e la decodifica inline (§3.3) non possono avvenire mentre un
worker legge. Perciò gli elementi sono pianificati **a lotti** (la finestra: fino a
512 × thread + 512 elementi, poche centinaia di byte ciascuno) solo quando **nessun
worker è in corsa** (`WaitWorkersIdle`: attende `NumRunning == 0`), e un elemento inline
attende allo stesso modo prima di toccare l'archivio. I worker leggono invece con
`ReadFullAt` (seek + read sotto il lock condiviso): fra loro non collidono.

### 3.3 Che cosa resta sequenziale (inline)

Decodificato con il codice di prima e gli stream reali, nel thread chiamante:

- l'intero archivio se multivolume, o se lo stream non è un `IInStream`;
- elementi con header locale non disponibile o non leggibile, directory;
- metodi la cui memoria non si ricava dagli header: LZMA, PPMd, zstd, xz, LZFSE (i loro
  parametri sono nei dati; il decoder di un file grande può anche essere multithread);
- elementi oltre 64 MiB (`CMemoryBudget::kMaxParallelFolderUnpackSize`) o con
  dimensioni oltre 4 GiB, cifratura "strong" di PKWARE, elementi su un disco diverso
  dall'ultimo in un archivio a volume singolo;
- **elementi grandi (> 1 MiB) non cifrati memorizzati o compressi meno del 5%**: si
  decodificano alla velocità di una copia (gli encoder Deflate emettono blocchi stored
  per i dati incomprimibili); un worker non guadagna nulla e paga due buffer e due copie
  della loro dimensione (misurato: 50 MB al 98,7%, 14 ms sequenziali contro 42 ms in un
  worker). Un elemento cifrato va sempre a un worker: la decrittazione costa centinaia di
  MB/s qualunque sia il metodo;
- **ripiego esatto**: se il worker ha prodotto più byte del dichiarato (file corrotto o
  dimensione sbagliata negli header: nel ciclo sequenziale il seguito dipende dallo
  stream reale) o ha sollevato un'eccezione, l'elemento è decodificato di nuovo inline
  al suo turno. Il risultato osservato è quello della decodifica inline.

Con AES (WinZip) il metodo di compressione è nel campo extra, non in `Method` (99, lo
stesso id di LZFSE): il pianificatore lo risolve da lì, altrimenti ogni zip AES finiva
inline (prima versione: nessun guadagno; corretto, §7).

### 3.4 Ammissione e attesa

`AdmitAhead` sottomette al pool gli elementi della finestra in ordine finché il budget di
memoria e il limite di elementi in volo (64 per worker) lo permettono; alloca i buffer
all'ammissione (se l'allocazione fallisce l'elemento passa inline e blocca l'ammissione
oltre sé finché non è processato). È chiamata prima di processare la testa della
finestra **e a ogni risveglio mentre il thread chiamante aspetta la testa**: un worker
che finisce un elemento qualsiasi (`Wakeup`) fa ammettere i successivi, così i worker
non restano a secco dietro a un elemento lungo. Con il limite di 4 per worker del 7z i
worker si fermavano dietro al file da 50 MB del corpus (1285 file: 0,39 s contro 0,21).

Un primo tentativo decodificava i file piccoli (≤ 256 KiB) nel thread chiamante, in
anticipo mentre aspettava: una volta corretti ammissione e finestra non dava più nessun
vantaggio misurabile (1200 file piccoli: 0,022 s in ogni variante contro 0,010 s con
tutti i file ai worker), ed è stato rimosso.

### 3.5 Password

La password è chiesta **una volta, dal thread chiamante**, prima di ammettere il primo
elemento cifrato (`CCachedPassword::Fetch`); i worker la leggono senza chiamare la UI.
I conteggi dei riferimenti COM di 7-Zip non sono atomici: il provider è assegnato ai
decoder dei worker prima di avviare il pool (un `SetPasswordProvider` dai worker dava
un use-after-free in `~CZipDecoder`, trovato con ASan).

### 3.6 Avanzamento, annullamento, errori

Il thread chiamante riferisce l'avanzamento della testa (`CItemDecodeProgress` aggiorna
i contatori atomici del task e sveglia il thread solo se il task è la testa); un errore
della callback di avanzamento o un `E_ABORT` fermano i worker (`Abort`, poi
`~CParallelZipExtract` ferma il pool prima di liberare i task). Gli errori per elemento
seguono il codice sequenziale: `opRes` e HRESULT del decoder sono quelli di
`CZipDecoder::Decode`, riprodotti al turno dell'elemento.

### 3.7 Configurazione

Proprietà `pfd` dell'handler zip (`-mpfd=off` la disattiva), attiva di default; richiede
`-mmt` ≥ 2, almeno 2 elementi richiesti, di cui almeno 2 parallelizzabili nel primo
lotto (altrimenti il ciclo sequenziale originale, senza alcun costo).

## 4. Equivalenza: verifica

`tests/extract_trace.cpp -z` registra ogni callback dell'estrazione (stream richiesti,
operazioni, dimensioni e CRC dei dati scritti, risultati, esito) di un handler zip
creato direttamente; `tests/zip_extract_equivalence.py` crea zip con Deflate ai livelli
1/5/9, Store, Deflate64, BZip2, LZMA, PPMd, ZipCrypto, AES-256, da stdin (descrittori di
dati) e con 150 file piccoli, ne deriva copie corrotte (8 byte invertiti in tre punti,
due tagli, un byte casuale) e confronta la traccia di `-mmt=8` con quella di `-mmt=1`,
anche con `-t`, con selezione parziale e con password sbagliata: **272 casi identici**.
`-mpfd=off -mmt=8` è confrontato solo sugli archivi integri: il decoder BZip2 sequenziale
usa più thread e su dati corrotti la quantità di uscita prima dell'errore differisce da
`-mmt=1` (comportamento di 7-Zip, non di questo lavoro); il percorso parallelo decodifica
ogni elemento con un thread e coincide sempre.

ASan (ripetizioni con 3, 8, 20 thread, archivi cifrati, password sbagliata, estrazione
confrontata byte per byte con i dati originali) e TSan puliti; 225 esecuzioni senza
stalli (un'esecuzione isolata a 2,1 s non si è più ripetuta: rumore della macchina).

## 5. Memoria

Costo di un elemento ammesso = pack + unpack + memoria del decoder (Deflate 2 MiB, BZip2
8 MiB, Shrink/Implode 1 MiB) + 64 KiB, nel budget di decompressione (17/32 della RAM, o
`-mmemuse`). Elementi oltre 64 MiB non entrano (inline). I buffer sono allocati
all'ammissione e liberati alla riproduzione.

## 6. Limiti e lavoro futuro

- **ZipCrypto** è un cifrario seriale a byte (~156 MB/s nel decoder attuale): il file più
  grande resta il limite (corpus: 50 MB → 0,32 s). Il decoder si può ottimizzare (tabelle,
  meno dipendenze per byte), lavoro separato.
- **Buffer freschi per elemento**: i buffer grandi passano da `mmap` e pagano i page
  fault (≈ 10 ms per 50 MB); un pool di buffer riusati ridurrebbe il costo fisso dei
  worker sugli elementi grandi.
- **Lotti**: fra un lotto e l'altro la pipeline si svuota (`WaitWorkersIdle`); con la
  finestra di 512 × thread + 512 elementi succede ogni poche migliaia di file. Una vista
  con posizione propria sullo stream condiviso, usata anche da `CInArchive`, eliminerebbe
  i lotti.

## 7. Risultati (i7-12700H, archivi in `/dev/shm`, minimo di 4 prove)

| Archivio (`-tzip -mx5`) | `-mmt=1` | `-mmt=8` | `-mmt=20` |
|---|---|---|---|
| silesia (12 file, 203 MB) test | 0,248 s | **0,126 s** | 0,126 s |
| silesia estrazione | 0,285 s | **0,171 s** | 0,167 s |
| corpus AES-256 (1285 file, 110 MB) test | 0,445 s | **0,133 s** | 0,111 s |
| corpus AES-256 estrazione | 0,492 s | **0,161 s** | 0,146 s |
| corpus ZipCrypto test | 0,530 s | 0,384 s | 0,385 s |
| corpus (1285 file) test | 0,060 s | 0,058 s | 0,054 s |
| corpus estrazione | 0,096 s | 0,093 s | 0,084 s |
| 1200 file sorgente (27 MB) test | 0,021 s | **0,010 s** | 0,013 s |
| 2 file quasi incomprimibili (66 MB) test | 0,015 s | 0,014 s | 0,014 s |

Il corpus in chiaro non guadagna perché 66 dei suoi 110 MB sono i due file quasi
incomprimibili che restano sequenziali (14 ms) e il resto si decodifica in 20 ms; prima
della regola §3.3 il percorso parallelo lo **rallentava** (0,060 → 0,079 s). Su silesia il
limite è il file più grande (`mozilla`, 51 MB: ~0,12 s da solo); su AES il
guadagno è 3,3-4 volte perché la decrittazione (AES-NI + HMAC-SHA1) domina ed è
distribuita.
