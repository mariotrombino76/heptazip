# 0005 — Un match finder nuovo per LZMA: piano di ricerca

| | |
|---|---|
| **Stato** | Piano, in attesa di conferma della direzione |
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
