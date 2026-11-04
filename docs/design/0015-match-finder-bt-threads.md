# 0015 — Match finder bt su più thread per bucket di hash

| | |
|---|---|
| **Stato** | Implementato (issue #77); misure in §6 |
| **Data** | 2026-11-04 |
| **Fase** | 2: ottimizzazione delle prestazioni (parallelizzazione) |
| **Prerequisiti** | design 0004 (bt4 al suo ottimo a output identico), 0005 (bt4 quasi esatto) |
| **File** | `C/LzFindMt.c`, `C/LzFindMt.h`, `C/LzFindOpt.c` (`GetMatchesSpecN_2`), `C/LzFind.h`/`.c` (`sonExtra`), `C/LzmaEnc.c`, `C/Lzma2Enc.c` |

---

## 1. Il problema

Dopo #75 (dizionario 16 MiB al livello 5) il tempo di `-mx5` su Silesia a 20 thread
(13 s) è dettato dal blocco più pesante: il blocco 5 da solo costa 9,0 s con i 2 thread
di LZMA (match finder + parser) e 13,9 s in seriale, quindi **match finder ~9 s, parser
~5 s**: il parser aspetta il thread bt per il 45% del tempo. Gli altri blocchi stanno in
4,6-5,6 s. A `-mx9` (dizionario e blocco da 256 MiB) Silesia è un blocco solo: 19 thread
inutilizzati.

Il bt4 a output identico è al suo ottimo pratico (0004: la latenza della memoria, ~30 ns
per passo dell'albero); le manopole compatibili ma non identiche costano rapporto per
poca velocità (Silesia `-mx5`: bt5 −12% tempo +0,055 punti, mc=24 −5% +0,04, blocchi da
16 MiB −23% +0,15). Quel che resta è il parallelismo *dentro* il match finder.

## 2. L'idea

In `LzFindMt.c` il lavoro è diviso in un thread hash (calcola l'hash di 4 byte e dà, per
ogni posizione, la distanza dall'occorrenza precedente con lo stesso hash) e un thread
bt (inserisce la posizione nell'albero binario del suo bucket e raccoglie i match), più
il thread dell'encoder (parser e range coder; gli hash di 2 e 3 byte li fa lui).

**Gli alberi di bucket di hash diversi sono indipendenti**: un nodo punta solo a
posizioni dello stesso bucket. Quindi N thread bt possono dividersi i bucket:
`owner = hash & (N − 1)`, deciso dal thread hash e scritto accanto alle distanze (un
byte per posizione). Ogni thread bt scorre tutte le posizioni di un blocco hash ma
inserisce e cerca solo le proprie; per le altre scrive la parola `kMtNotOwned` nel suo
flusso. L'encoder legge gli N flussi in passo: per ogni posizione uno solo ha i match.
Ogni albero vede le sue posizioni nello stesso ordine di prima: **stessi match, archivi
identici** a quelli di un thread bt e del match finder a thread singolo.

## 3. I punti delicati

1. **Nodi condivisi per posizione ciclica.** `son[]` è indicizzato per posizione
   ciclica, non per bucket: il thread A, avanti, inserendo la posizione `p` sovrascrive
   il nodo che conteneva `p − cyclicBufferSize`, che il thread B, indietro, potrebbe
   ancora percorrere (per B è ancora nella finestra). Rimedio: `son` ha `sonExtra`
   nodi in più (ciclo di `sonSize = cbs + sonExtra`), mentre il taglio della finestra
   resta a `cbs`: un nodo sopravvive `sonExtra` posizioni dopo l'uscita dalla
   finestra. Lo sfasamento fra thread bt è limitato dall'anello dei blocchi hash (2
   slot, uno slot si riusa solo quando *tutti* i thread lo hanno lasciato): meno di 2
   blocchi hash = 2 × 104 848 posizioni < `sonExtra` = 262 144 (2 MiB di nodi).
2. **Normalizzazione di `son`** (ogni 4 Gi posizioni, quando `pos` sfiora 2³²): non
   può essere un'operazione sull'intero array fatta da un thread solo (gli altri hanno
   una base diversa e leggono i loro nodi), e una barriera fra i thread bt può bloccarsi
   (un thread in attesa alla barriera mentre un altro aspetta che l'encoder consumi il
   suo anello d'uscita, e l'encoder aspetta il primo). Soluzione senza sincronizzazione:
   ogni thread normalizza **solo i propri nodi dentro la propria finestra**,
   `(pos − cbs, pos)`, riconoscendoli dal proprietario ricalcolato dai dati (i byte
   della posizione sono ancora nel buffer: `keepSizeBefore` copre finestra più il
   vantaggio del thread hash). I nodi più vecchi non vanno toccati: il thread non li
   legge più (taglio della finestra) e li sovrascrive quando inserisce; e un thread
   più avanti può già avervi messo una posizione sua. La prima versione normalizzava
   tutti i `sonSize` nodi propri e produceva archivi diversi e non deterministici con
   la normalizzazione forzata ogni 8 Mi posizioni (`-DkMtMaxValForNormalize=0x800000`):
   trovato così, corretto restringendo alla finestra.
3. **Proprietario costante per bucket.** `owner` deve essere funzione del valore di
   hash. Per gli hash crc (3, 4, 4b, 5, 5b) i 2 bit bassi del valore sono
   `(crc[p[0]] ^ p[1]) & 3` (gli altri termini sono spostati a sinistra di ≥ 2 bit o
   sono byte sopra il bit 8, e `hashMask` copre sempre i 16 bit bassi); per bt2 e "3b"
   sono `p[0] & 3`. Lo stesso calcolo nel thread hash (`OWNER_CRC`/`OWNER_BYTE0`) e
   nella normalizzazione (`MtOwner`).
4. **Match lunghi.** `GetMatchesSpecN_2` ha una scorciatoia per le sequenze ripetute
   (copia il nodo della posizione `pos + 1 − delta` in quello di `pos + 1` senza
   cercare): continua solo finché il proprietario non cambia. Il risultato è lo stesso
   della ricerca completa (il primo candidato ha già la lunghezza massima).
5. **Sincronizzazione.** Anello hash a N consumatori (un semaforo `filled` per thread
   bt, conteggio dei riferimenti per liberare lo slot), N anelli d'uscita (un `CMtSync`
   ciascuno), lock del buffer: uno per il thread encoder (`lzCs`), uno per thread bt;
   il thread hash li prende tutti per spostare il buffer. L'encoder non tiene mai un
   lock mentre aspetta un blocco. Un blocco hash contiene più posizioni (104 848) di
   quante ne possa contenere un blocco bt (65 536 parole, almeno una per posizione):
   così l'encoder non aspetta mai il thread che è un blocco hash avanti all'altro, e i
   tre anelli non si bloccano a vicenda. Stop: l'encoder manda lo stop a tutti i thread
   bt prima di attenderne uno (un thread bt fermo sull'anello hash viene liberato
   dall'uscita degli altri), poi ferma il thread hash.

6. **False sharing.** I nodi di 8 posizioni consecutive stanno nella stessa linea di
   cache e appartengono a thread diversi: due thread che inseriscono le stesse posizioni
   nello stesso momento si passano ogni linea avanti e indietro. Misurato sul blocco 5:
   +45% di CPU con 2 thread e +85% con 4, con un guadagno in tempo dimezzato. Rimedio:
   il thread k inizia un blocco hash solo dopo che il thread k−1 glielo ha concesso
   (semaforo `prevDone`), e k−1 lo concede quando lo ha finito, quando ne ha superato la
   metà (52K posizioni, 420 KiB di nodi: la linea si sposta una volta sola) o prima di
   bloccarsi sul proprio anello d'uscita. L'ultima condizione è necessaria: la prima
   versione concedeva solo a blocco finito, e con 4 thread a `-mx9` si è bloccata
   (thread 0 fermo sull'anello d'uscita a metà blocco, perché l'encoder consuma al passo
   del thread più indietro, che aspettava il via dal thread 2, che aspettava il thread 1,
   che aspettava il thread 0). Con la concessione prima di ogni possibile attesa nessun
   thread aspetta un thread bloccato. L'anello hash ha 4 slot (erano 2) perché lo
   sfasamento non fermi il thread hash; `sonExtra` = 4 blocchi hash (4 MiB di nodi).
   Dopo il rimedio il sovraccarico di CPU resta del 29% con 2 thread e del 54% con 4
   (scansione delle posizioni altrui, scritture casuali sui cammini in linee dell'altro
   thread, contesa di cache): il prezzo del parallelismo su un array condiviso.

## 4. Thread

`numThreads` di LZMA = 1 + thread bt: 2 come prima (un thread bt), 3 e 4 → due, da 5 →
quattro (`LzmaEnc.c`). Il thread hash non si conta: è quasi sempre in attesa. LZMA puro
riceve dal handler 7z tutti i thread (`-mmt`): a 20 thread usa 4 thread bt.

`Lzma2EncProps_Normalize`: i thread avanzati vanno al match finder **solo con un blocco
in volo** (dati entro un blocco, `-mx9`, flusso solid), se i thread di LZMA non erano
dati per metodo, per dati di almeno 8 MiB (o di dimensione ignota) e dizionario di
almeno 4 MiB: da 3 thread totali 2 thread bt, da 5 ne ha 4. Con più blocchi in volo i
blocchi tengono i core: i thread bt in più costano CPU e lì guadagnano poco (corpus,
2 blocchi per folder: stesso tempo per 1,6 volte la CPU; file da 150 MB, 5 blocchi su 20
thread: −8% di tempo per +38% di CPU). Con dizionario da 64 KiB il parser è il collo di
bottiglia e 2 thread bt costano il 9%. Gli archivi non dipendono dal numero di thread.

L'encoder parallelo dei folder (`7zUpdate.cpp`) passa al coder LZMA/LZMA2 di ogni slot i
thread di LZMA che il budget ha contato (`EstimateFolderEncodeCost`: 2 con il match
finder bt) come proprietà esplicita `kLzmaThreads` (nuova, `ICoder.h`, non è un'opzione
utente): con i thread totali un folder di un blocco avrebbe diviso il match finder su 4
thread mentre il budget ne contava 2, e dieci folder in parallelo avrebbero fatto girare
60 thread per lo stesso tempo (misurato sul corpus: 1,6 volte la CPU). Il totale
(`kNumThreads`) resta quello dato, perché decide i blocchi LZMA2 e quindi l'archivio: il
primo tentativo, che riduceva il totale al budget, cambiava la scelta blocchi/solid di un
folder piccolo e faceva fallire `tests/update_equivalence.py` (`-mpfc=on` diverso da
`-mpfc=off` in 1 caso su 66).

## 5. Memoria

Per encoder LZMA con più thread bt: `sonExtra` 4 MiB di nodi, un anello d'uscita da
4 MiB per thread bt (prima 4 MiB), i byte dei proprietari dentro i blocchi hash (ora
4 slot, 2 MiB invece di 1). Con 2 thread bt +9 MiB, con 4 thread +17 MiB. Con un thread
bt (il caso di `-mx5` con più blocchi) +1 MiB.

## 6. Misure

Minimo di 3 esecuzioni alternate (riferimento develop a d05005e), i7-12700H 20 thread,
`exp3.sh`:

| | prima | dopo | CPU prima → dopo |
|---|---|---|---|
| blocco 5 di Silesia (32 MiB) `-mx5`, 1 thread bt (`-mmt2`) | 8,97 s | 8,83 s | 12,9 → 13,0 s |
| idem, 2 thread bt (`-mmt3`) | 8,97 s | **6,36 s** | 12,9 → 16,7 s |
| idem, 4 thread bt (`-mmt5` o `-mmt20`) | 8,97 s | **4,91 s** | 12,9 → 19,9 s |
| idem, `d=64k` (parser-bound), `-mmt2` | 3,10 s | 3,06 s | |
| Silesia `-mx9` (un blocco da 256 MiB), 20 thread | 42,5 s | **26,1 s** | 61,8 → 89,3 s |
| LZMA puro `-m0=lzma -mx5`, 60 MB, 20 thread | 10,9 s | **7,4 s** | 18,1 → 23,7 s |
| file da 12 MB `-mx5`, 20 thread | 2,92 s | **1,68 s** | 4,3 → 6,3 s |
| Silesia `-mx5`, 20 thread (7 blocchi: non divide) | 12,5 s | 12,7 s | 93,0 → 93,9 s |
| Silesia `-mx5 -mmt6` (3 blocchi in volo: non divide) | 16,6 s | 16,6 s | 65,5 → 65,8 s |
| corpus `-mx5`, 20 thread (2 folder da 2 blocchi) | 3,7 s | 3,7 s | 14,6 → 14,9 s |
| file da 6 MB (sotto la soglia) | 1,36 s | 1,40 s | |

Il quarto thread bt aggiunge meno del secondo (4,9 contro 6,4 s): non per il parser, come
sembrava, ma per il costo per passo dell'albero che cresce con i thread (§7).

## 7. Profilo del thread encoder e tentativo con array privati (issue #79)

`gprof` su una build `-pg` (blocco 5, `-mx5`):

| | CPU |
|---|---|
| `LzmaEnc_CodeOneBlock` (parser e range coder, `GetOptimum` inlined) | 2,3 s |
| interfaccia col match finder nel thread encoder (`MatchFinderMt_GetMatches`, `MixMatches3`, `ReadMatchDistances`, …) | 0,5 s |
| `GetMatchesSpecN_2`, 1 thread bt (`-mmt2`) | 10,1 s |
| `GetMatchesSpecN_2`, 2 thread bt (`-mmt3`), somma | 11,8 s (5,9 ciascuno) |
| `GetMatchesSpecN_2`, 4 thread bt (`-mmt5`), somma | 18,4 s (4,6 ciascuno) |

Il parser costa ~2,3 s, non ~5 come stimato dalla differenza fra `-mmt1` e `-mmt2` (che
include le attese della pipeline). Il limite con 2 e con 4 thread bt restano i thread bt:
il costo **per passo dell'albero** cresce con i thread, 37 ns con uno, 44 con due, 68 con
quattro, identico se i thread sono fissati con `taskset` a P-core, hyper-thread o E-core.

Ipotesi "false sharing residuo" verificata con un prototipo con un array `son` **privato per
thread** (niente linee condivise, niente sfasamento, niente nodi extra, memoria ×N):
CPU 17,9 s con 2 thread e 23,7 con 4, tempi 6,8-8,2 e 5,5 s, cioè **peggio** della versione
condivisa (16,7 / 19,9 s; 6,4 / 4,9 s). Quindi non è il false sharing: è la **capacità di
cache**. La velocità di bt4 viene dalla località dei nodi recenti (le ultime posizioni,
densi in memoria): dividendo le posizioni per bucket, i nodi recenti di ogni thread o si
sparpagliano su N volte le linee (array privati, 1/N di nodi utili per linea) o stanno
nelle stesse linee e migrano fra i core (array condiviso). Un array denso per thread
richiederebbe nodi da 16 byte (posizione e indice compatto del figlio) e grande quanto la
finestra per ogni thread, perché nel caso degenere (tutte le posizioni in un bucket) un
thread riceve tutto: 2N volte la memoria. Scartato.

**Conclusione**: la versione di #78 è la migliore fra quelle provate; la strada "LZMA più
veloce a output identico" si ferma qui. Il parser non è il limite finché i thread bt sono
al massimo 4.

## 8. Verifica

Archivi identici al riferimento (develop a d05005e, un thread bt) con: bt2, bt3, bt4,
bt5, `fb=273`, `fb=5`, 1-20 thread, LZMA e LZMA2, xz, stdin, file da 1 byte a 60 MB,
corpus, Silesia `-mx5` e `-mx9`; con la build di prova che normalizza ogni 8 Mi
posizioni: dizionari da 64 KiB a 3 MiB, tutte le famiglie di hash, 2-5 thread, tre
esecuzioni identiche fra loro.
