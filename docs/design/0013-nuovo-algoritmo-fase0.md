# 0013 — Nuovo algoritmo LZ: fattibilità (Fase 0)

| | |
|---|---|
| **Stato** | Ricerca chiusa: Fase 0 e Fase 1 (decoder) senza raggiungere le soglie (issue #16) |
| **Data** | 2026-11-02 |
| **Fase** | 3: nuovi algoritmi |
| **Codice** | `tools/research/lzstatic/` (`lztrace.c`, `lzstatic.py`, `adaptive.py`, `lzseq.py`, `lzproto.c`); aggancio `Z7_LZMA_TOKEN_TRACE` in `C/LzmaEnc.c` (solo per la ricerca) |

---

## 1. Obiettivo

Il rapporto di LZMA con una decompressione 6-10 volte più veloce: lo spazio occupato
nel mondo proprietario da Oodle Leviathan, vuoto nell'open source. Silesia, un core:
LZMA2 `-mx9` 22,96% a ~150 MB/s, zstd -19 24,93% a ~1170 MB/s.

Idea: il modello di LZMA (parsing ottimo, 4 offset ripetuti, "matched literal") con
entropy coding statico per blocco (rANS), flussi separati e decodificabili in blocco,
al posto del range coder adattivo bit per bit di LZMA.

Soglie della Fase 0: perdita rispetto a LZMA < 2%, decompressione >= 600 MB/s per core.

## 2. Metodo

`lztrace.c` comprime con LZMA (un thread, `-mx9`) e registra ogni decisione del parser
ottimo (aggancio in `LzmaEnc.c`, compilato solo con `Z7_LZMA_TOKEN_TRACE`). Sullo stesso
parsing:

- `lzstatic.py`: dimensione con tabelle statiche per blocco (entropia + costo delle
  tabelle), con varie scelte di contesti e blocchi;
- `adaptive.py`: lo stesso con modelli adattivi a simboli interi, per flusso;
- `lzseq.py`: i token come sequenze (serie di letterali + match, come zstd);
- `lzproto.c`: prototipo vero (rANS a 32 bit con rinormalizzazione a 16 bit, tabelle
  per blocco da 256 KiB, letterali decodificati in blocco con 4 stati intercalati,
  sequenze), con decoder che ricostruisce il file (verificato byte per byte).

## 3. Risultati

### 3.1 Rapporto (primi 20 MB di silesia, LZMA 36,85%)

| variante | rispetto a LZMA |
|---|---|
| token di LZMA, tabelle statiche, blocchi 256 KiB | +2,75% |
| + contesti di posizione (pb = 2) | +2,03% |
| + letterali senza contesto (meno tabelle) | +1,71% |
| sequenze (come zstd) invece di una decisione per letterale | +1,70% (3,4 volte meno decisioni) |
| più contesto sui letterali (lc 4, 5, 8) | peggio (costo delle tabelle) |
| contesto dal byte del match per i letterali dopo un match | nessun guadagno |

Adattività: un modello adattivo a simboli interi fa come le tabelle statiche (+0,36%);
l'adattamento veloce di LZMA vale solo sui flussi piccoli di decisioni (tipo di token
+8,7%, lunghezza dei rep +18%), mentre sui flussi grandi le tabelle statiche sono
migliori (letterali -10%).

**Silesia intero (212 MB)**: il prototipo arriva al **23,78%** (LZMA 22,96%):
**+3,6%**. Sui dati binari il modello adattivo bit per bit di LZMA guadagna di più.

### 3.2 Velocità (un core)

| | rapporto | decompressione |
|---|---|---|
| LZMA `-mx9` | 22,96% | 150 MB/s |
| prototipo | 23,78% | 396 MB/s |
| brotli -q 11 -w 24 | 23,48% | ~480 MB/s |
| zstd -19 | 24,93% | 1173 MB/s |

Sui primi 20 MB (testo): prototipo 183 MB/s, LZMA 60, zstd -19 348; con una finestra di
1 MB il prototipo va a 380 MB/s: buona parte del tempo è l'attesa della memoria per i
match lontani (rimedio noto: sequenze decodificate in blocco e prefetch, come la
modalità "long" di zstd).

## 4. Conclusione

Soglie **non raggiunte**: +3,6% su silesia (soglia 2%) e 396 MB/s (soglia 600). Il
prototipo è 2,6 volte più veloce di LZMA, ma brotli -11 è già meglio su entrambi gli
assi (anche se comprime a 0,7 MB/s).

Per arrivare all'obiettivo servirebbero insieme:

- un modello migliore per i dati binari (letterali per differenza dal byte del match,
  contesti di allineamento, tabelle multiple per blocco scelte dall'encoder come brotli);
- un parser ottimo che usi i costi statici veri invece di quelli di LZMA;
- decoder in due fasi con prefetch, codici combinati (lunghezza + tipo, slot + bit bassi
  in un solo simbolo), SIMD.

Ognuno è incerto e il totale è un progetto di ricerca lungo, con il rischio di arrivare
al livello di brotli -11. Gli strumenti restano in `tools/research/lzstatic/` per
riprendere da qui.

## 5. Fase 1: il decoder (2026-11-02)

Dopo l'aggiunta di brotli nei `.7z` (design 0014) si è provato a migliorare il decoder:

- **due fasi con prefetch** (`lzproto.c`): le sequenze del blocco decodificate in array
  senza toccare l'uscita, poi letterali e copie con l'origine del match di 8 sequenze
  dopo chiesta in anticipo alla memoria: primi 20 MB 183 -> 252 MB/s, **silesia 396 ->
  433 MB/s** (23,78%).
- Il limite rimasto è la decodifica delle sequenze (silesia: 268 ms su 490): ogni
  simbolo sceglie la tabella in base a un simbolo di un altro flusso (posizione dopo la
  serie di letterali per la lunghezza, lunghezza per lo slot della distanza), quindi le
  4-5 decodifiche di una sequenza sono in fila. Toglierle costa rapporto (primi 20 MB,
  rispetto a LZMA):

| variante | rispetto a LZMA |
|---|---|
| sequenze con i contesti incrociati | +1,70% |
| senza contesti incrociati (flussi indipendenti, come zstd) | +3,15% |
| lunghezza e slot in un solo simbolo | +2,22% |
| posizione dell'inizio della serie di letterali | +1,96% |

**Conclusione**: il prototipo (23,78% a 433 MB/s su un core) resta dietro a brotli
`-mx11` nel `.7z` (23,66%, 445 MB/s su un core e 1,2 GB/s con 20 thread, design 0014) su
entrambi gli assi, e ogni accelerazione del decoder costa rapporto. Con brotli
disponibile, il valore aggiunto di un formato nuovo è basso: la ricerca si chiude qui.
Ripartire avrebbe senso solo con un'idea di modello diversa (per esempio tabelle
multiple per blocco scelte dall'encoder, o un parser che ottimizzi per i costi statici),
non con altre ottimizzazioni dello stesso schema.

## 6. Come riprodurre

```sh
gcc -O2 -DZ7_LZMA_TOKEN_TRACE -DZ7_ST -o lztrace tools/research/lzstatic/lztrace.c \
    C/LzmaEnc.c C/LzFind.c C/LzFindOpt.c C/CpuArch.c C/Alloc.c
./lztrace silesia.tar sil.tok 9 26          # 3 minuti: la dimensione LZMA e i token
LZS_PB=2 LZS_LC=0 python3 tools/research/lzstatic/lzstatic.py silesia.tar sil.tok <lzma>
python3 tools/research/lzstatic/lzseq.py silesia.tar sil.tok <lzma>
gcc -O2 -march=x86-64-v2 -o lzproto tools/research/lzstatic/lzproto.c
taskset -c 2 ./lzproto silesia.tar sil.tok
```
