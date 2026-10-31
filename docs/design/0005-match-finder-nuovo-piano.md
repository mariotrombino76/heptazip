# 0005 — Un match finder nuovo per LZMA: piano di ricerca

| | |
|---|---|
| **Stato** | Banco di prova e oracolo fatti (§6): il candidato A non supera i criteri |
| **Data** | 2026-10-31 |
| **Fase** | 2: ottimizzazione delle prestazioni |
| **Prerequisito** | design 0004 (bt4 al suo ottimo pratico con gli stessi match) |

---

## 1. Requisiti di un match finder per LZMA

Interfaccia `IMatchFinder2` (`C/LzFind.h`), usata da `LzmaEnc.c`:

- `GetMatches()`: per la posizione corrente, coppie *(lunghezza, distanza − 1)* con
  lunghezze **crescenti**, ognuna con la distanza **più vicina** trovata per quella
  lunghezza, fino a `lenLimit` (`fb`, da 5 a 273). Il parser ottimo prova tutte le
  lunghezze: un match finder che dà solo il match più lungo richiederebbe un parser
  diverso.
- `Skip(n)`: avanza di n posizioni (dentro un match scelto) aggiornando le strutture.
- Streaming su una finestra scorrevole (dizionario da 64 KiB a 1,5 GiB), dati letti a
  blocchi; memoria proporzionale al dizionario.
- Multi-thread: oggi un thread hash e un thread bt lavorano in pipeline con l'encoder
  (`LzFindMt.c`); LZMA2 divide inoltre in blocchi indipendenti.
- Il formato non cambia: qualunque match valido dà uno stream LZMA standard. Cambiano i
  match trovati, quindi gli archivi non sono identici a quelli di bt4.

## 2. Il riferimento da battere

bt4 a `-mx5`: ~3,5 MB/s per thread su Silesia, 8-9 passi per posizione, ~25 ns per
passo, al suo ottimo pratico (0004). Criterio di successo per un candidato, sugli stessi
livelli e corpus:

- **almeno 1,3 volte più veloce** con archivi al massimo **+0,5%** più grandi, oppure
- **stessa velocità** con archivi almeno **1% più piccoli**.

## 3. Candidati

| | Idea | Pro | Contro e rischi |
|---|---|---|---|
| **A. Suffix array per blocchi** | Per ogni blocco (es. 1-8 MiB) si ordina il testo finestra + blocco (SA-IS, O(n), accessi sequenziali), si calcola l'LCP, e per ogni posizione si ricavano dai vicini nell'ordinamento le occorrenze precedenti con lunghezza decrescente, tenendo la più vicina per ogni lunghezza | Match **esatti** nella finestra (bt4 approssima, con il limite di passi): rapporto atteso uguale o migliore. Costruzione cache-friendly, blocchi indipendenti (parallelizzabile) | La parte difficile è l'elenco "per ogni lunghezza la distanza più vicina" senza scandire molti vicini. Riordinare la finestra a ogni blocco costa W/B volte. Memoria 4-9 byte per posizione. Lavoro di ricerca di più giorni, esito incerto |
| **B. Righe di hash con tag SIMD** (idea delle righe di zstd ≥ 1.5) | Ogni hash indica una riga di 16-32 posizioni recenti con 1 byte di tag ciascuna; un confronto SIMD dei tag filtra i candidati in una riga di cache | Pochi accessi casuali per posizione; adatto ai livelli veloci (`-mx1`…`-mx4`, oggi hash chain) | Profondità limitata alle posizioni recenti: perde i match lunghi lontani, rapporto più basso ai livelli alti. Utile soprattutto dove zstd, già nel fork, è comunque l'alternativa veloce |
| **C. Radix match finder nostro** (idea di fast-lzma2, riscritta) | Ordinamento radix delle posizioni di un blocco per contesto, catene di posizioni simili | Veloce e parallelizzabile | Dà essenzialmente il match più lungo: serve anche un parser adattato. Il progetto più grande |

## 4. Metodo

1. **Banco di prova**: un match finder alternativo selezionabile con un nome nuovo
   (`-m0=lzma2:mf=…`), mai di default finché non supera i criteri; misure separate di
   match finder ed encoder, rapporto e memoria su Silesia, sorgenti, binari, testi ripetitivi.
2. **Oracolo**: un match finder esatto e lento (stesse regole, nessun limite di passi)
   per misurare *quanto rapporto si guadagna con match migliori* rispetto a bt4. Se il
   margine è piccolo, il candidato A vale solo per la velocità; se è grande, anche per il
   rapporto. Decide la priorità prima di scrivere codice veloce.
3. **Prototipo** del candidato scelto, con correttezza verificata dal roundtrip (ogni
   match deve essere valido) e confronto con l'oracolo.
4. **Integrazione** (streaming, memoria, multi-thread) solo se il prototipo supera i
   criteri di §2.

## 5. Proposta

Partire da 4.1 e 4.2 (banco di prova + oracolo, poco codice e decisivi), poi il
candidato A per i livelli predefiniti. B in un secondo momento, solo se servono livelli
LZMA veloci. C non prima di aver misurato A.

## 6. Risultati del banco di prova e dell'oracolo (2026-10-31)

### 6.1 Oracolo: quanto rapporto si guadagna con match esatti

L'oracolo non richiede codice: bt4 con `mc=1000000` (`-m0=lzma2:mc=1000000`) esplora
l'albero senza limite di passi e trova, per ogni lunghezza fino a `fb`, l'occorrenza più
vicina nella finestra: esattamente ciò che chiede il parser. Silesia file per file, 1 thread:

| | bt4 standard (byte) | Oracolo (byte) | Guadagno |
|---|---|---|---|
| Totale `-mx5` | 49 608 648 | 49 571 281 | **0,075%** |
| Totale `-mx9` | 48 757 480 | 48 738 438 | **0,039%** |
| File migliore | samba `-mx5` 0,29%, xml `-mx9` 0,31% | | |

`mf_bench` sugli stessi dati (20 MB di Silesia): bt4 standard 2,837 coppie per posizione
e match più lungo medio 7,835; oracolo 2,853 e 7,854, solo il 6-9% più lento. Il limite di
passi di bt4 scatta raramente: **bt4 è già quasi esatto**. Un match finder nuovo non può
migliorare il rapporto in modo apprezzabile: può vincere solo sulla velocità.

### 6.2 Costo minimo del candidato A

`tools/mfbench/sais_probe.c`: SA-IS diretto (verificato sull'ordinamento) costa 64 ns per
byte su 4 MB e 76 ns per byte su 20 MB. Per trovare match in tutto il dizionario ogni
blocco va ordinato con la finestra precedente (con blocchi grandi quanto il dizionario,
ogni byte è ordinato circa due volte: ~150 ns per byte), più l'LCP (accessi casuali,
10-20 ns per byte) e l'estrazione "per ogni lunghezza la distanza più vicina" (decine di ns,
più sui dati ripetitivi). Stima: **220-300 ns per posizione, contro i 240 di bt4**.
Un SA-IS molto ottimizzato (2-3 volte più veloce) porterebbe forse a 1,3-1,6 volte il
match finder, cioè ~1,2-1,4 volte la compressione (il match finder è il 75% del tempo),
con 3-4 volte la memoria di bt4 e un progetto di più settimane.

### 6.3 Conclusione

Con i criteri di §2 il candidato A **non conviene**: niente margine sul rapporto, e un
guadagno di velocità incerto e piccolo rispetto al costo. Il banco (`mf_bench`) e l'oracolo
restano disponibili per valutare altre idee. Le leve migliori restano il parallelismo e,
per la velocità a parità di rapporto, la scelta del metodo (zstd per i livelli veloci).
