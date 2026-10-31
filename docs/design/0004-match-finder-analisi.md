# 0004 — Match finder LZMA: analisi per l'ottimizzazione SIMD

| | |
|---|---|
| **Stato** | Opzione 1 implementata (§7); opzione 2 in esplorazione |
| **Data** | 2026-10-31 |
| **Fase** | 2: ottimizzazione delle prestazioni (match finder vettorializzati, CLAUDE.md) |
| **File analizzati** | `C/LzFind.c` (`GetMatchesSpec1`, `Hc_GetMatchesSpec`, `SkipMatchesSpec`), `C/LzFindOpt.c` (`GetMatchesSpecN_2`, percorso multi-thread), `C/LzFindMt.c`, `C/Alloc.c` |

---

## 1. Domanda

CLAUDE.md prevede match finder vettorializzati (AVX2/AVX-512) per accelerare la
compressione. Prima di progettarli: dove va il tempo, e quale parte si può davvero
vettorializzare?

## 2. Dove va il tempo

`gprof` su una build `-pg`, LZMA2 con `-mmt=1` (match finder ed encoder nello stesso
thread), primi 60 MB di `silesia.tar`:

| Livello | Match finder | Encoder (parser, range coder) | Tempo |
|---|---|---|---|
| `-mx1` (hc5, parser veloce) | ~65% (`Hc5_MatchFinder_GetMatches` 55%, `Skip` 10%) | ~33% | 2,4 s |
| `-mx5` (bt4, parser ottimo) | ~76% (`Bt4_MatchFinder_GetMatches` 69%, `SkipMatchesSpec` 6%) | ~22% | 20,8 s |
| `-mx9` (bt4) | ~75% | ~24% | 22,8 s |

Il match finder è il punto giusto. In multi-thread (il default) il match finder gira nel
suo thread e l'encoder in un altro: la velocità è quella del più lento, il match finder.

## 3. Che cosa fa il match finder bt4

`GetMatchesSpec1` (e `GetMatchesSpecN_2` in multi-thread) percorre un albero binario: a
ogni passo legge un nodo di `son[]` e i dati alla posizione candidata, estende il match
**un byte alla volta** da `min(len0, len1)`, e scende a sinistra o a destra.

Contatori temporanei (`-mx5` / `-mx9`, stessi dati):

| | `-mx5` | `-mx9` |
|---|---|---|
| Passi per ricerca | 8,4 | 9,3 |
| Byte estesi per passo (media) | 2,4 | 2,6 |
| Passi con estensione 0 / 1-3 / 4-7 / 8-15 / 16+ | 49% / 18% / 25% / 7% / 1% | 49% / 19% / 23% / 7% / 2% |

Con ~250 ns per ricerca, cioè ~30 ns per passo, il costo è quello di un accesso alla
memoria che manca nella cache: i nodi di `son[]` (128 MiB con dizionario da 16 MiB) e i
dati del candidato sono sparsi. L'estensione, la sola parte vettorializzabile, conta
pochi nanosecondi per passo. Il passo successivo dipende dal confronto: i caricamenti
non si possono sovrapporre.

**Pagine da 2 MiB già attive.** `BigAlloc` di upstream su Linux allinea a 2 MiB e chiama
`madvise(MADV_HUGEPAGE)`: durante la compressione 150 MiB su 160 di memoria residente sono
in huge page (`AnonHugePages`). La leva dei TLB è già usata.

## 4. Prototipi (stessi match: archivi identici)

Misure alternate, `mfbench` (mediana di 3-4 esecuzioni), `-mmt=1`. Il rumore tra
ripetizioni è circa ±1-2%.

| Prototipo | Silesia 60 MB `-mx5` | `-mx9` | Sorgenti 10 MB `-mx5` | `-mx9` |
|---|---|---|---|---|
| A: estensione a parole (XOR a 64 bit + `ctz`, 8 byte per volta finché restano 8 byte prima di `lenLimit`) | −1,6% | −2,3% | −1,9% | −3,0% |
| B: prefetch del nodo e dei dati di entrambi i figli | +1,8% | | | |
| B1: prefetch dei soli nodi | −2,2% / +0,8% (non riproducibile) | +0,3% | | |
| B2: prefetch dei soli dati | +4,8% | | | |

Tutti producono archivi identici byte per byte. Con SSE2/AVX2 (16-32 byte per volta) A
non migliorerebbe: il 92% dei passi estende meno di 8 byte.

## 5. Conclusioni

- Per il match finder bt4 (i livelli predefiniti, `-mx5` e oltre) la vettorializzazione
  della ricerca dà al massimo il **2-3%**: il limite è la latenza della memoria, non il
  calcolo. Un "match finder AVX2/AVX-512" che dia gli stessi match non può fare molto
  di più.
- Guadagni maggiori richiedono un **algoritmo diverso** (meno accessi casuali per
  posizione), che cambia i match trovati: gli archivi restano LZMA validi, leggibili da
  qualunque 7-Zip, ma non identici, e il rapporto di compressione va misurato.

## 6. Opzioni

1. **Estensione a parole in tutti i cicli di confronto** (bt4 single e multi-thread,
   hash chain, skip): 2-3%, archivi identici, rischio basso, modifiche piccole.
2. **Esplorare un match finder diverso** per i livelli predefiniti, accettando archivi
   diversi a parità o miglioramento del rapporto (requisito di CLAUDE.md). Progetto di
   ricerca: va prima valutato con prototipi e misure (velocità e rapporto) su più corpus.
3. **Nessuna delle due**: il parallelismo (blocchi LZMA2, folder in parallelo) resta la
   leva principale per la velocità su macchine con molti core.

## 7. Opzione 1: estensione a parole (implementata)

`C/LzFindMatchLen.h` (header interno): `LzFind_MatchEnd(p, lim, diff)` restituisce la
prima posizione in `[p, lim)` dove `p[i] != p[i + diff]`, confrontando 8 byte per volta
(XOR di due load a 64 bit, `ctz` / 8) finché restano 8 byte prima di `lim`, poi byte per
byte. Stesso risultato del ciclo di upstream; non legge mai a `lim` od oltre (a differenza
del ciclo a byte può leggere fino a 7 byte dopo la prima differenza, sempre prima di `lim`,
nella zona già garantita valida dal match finder). Su CPU non little endian o compilatori senza `ctz` resta il ciclo a byte.

Sostituisce i cicli di estensione in `GetMatchesSpec1` (bt, single-thread),
`SkipMatchesSpec`, `Hc_GetMatchesSpec` (hash chain), `UPDATE_maxLen` (hash 2/3) e
`GetMatchesSpecN_1/_2` (`LzFindOpt.c`, thread del match finder multi-thread).

Verifica: archivi identici byte per byte alla build precedente con tutti i match finder
(bt2, bt3, bt4, bt5, hc4, hc5, con 1 e 2 thread), LZMA2 `-mx9` a 4 thread, `fb=273`,
Deflate e Deflate64 in zip, e i profili di `compare-archives.sh`.

Misure alternate (Silesia, 60 MB o intera; mediana di 3-6 esecuzioni):

| Modalità | Variazione del tempo |
|---|---|
| LZMA2 `-mx1` (hc5), 1 thread | −2,3% |
| LZMA2 `-mx3`, 1 thread | +0,9% (rumore ±5% in quella serie) |
| LZMA2 `-mx5`, 1 thread | −2,6% |
| LZMA2 `-mx9`, 1 thread | −4,0% |
| LZMA `-mx5 -mmt=2` (match finder MT) | −5,0% |
| LZMA2 `-mx5 -mmt=20` | **−7,7%** |

Il percorso multi-thread (`GetMatchesSpecN_2`, il default con più thread) guadagna di più
del single-thread: nel thread del match finder il ciclo a byte pesava di più.
