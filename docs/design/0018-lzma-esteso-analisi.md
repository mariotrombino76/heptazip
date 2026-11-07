# 0018 — LZMA "esteso": analisi dei modelli più ricchi (strada 2)

| | |
|---|---|
| **Stato** | Analisi conclusa: strada chiusa (issue #90) |
| **Data** | 2026-11-05 |
| **Fase** | 3: algoritmi (punto 2 del programma: LZMA solo Heptazip con modelli più ricchi) |
| **Codice** | `tools/research/lzmodel/lzmodel.c` (strumento di misura); nessuna modifica ai codec |

---

## 1. Domanda

Il programma "algoritmi" (design 0015 §1) prevedeva, dopo l'encoder LZMA più veloce a
output identico (strada 1, chiusa) e i metodi nuovi per famiglie di dati (strada 3, BWT),
una **strada 2**: un LZMA solo Heptazip che tenga la struttura LZ77 + range coder binario
adattivo ma con modelli di probabilità più ricchi. Soglia per farlo: **almeno 1,5 punti su
Silesia** (22,98% → 21,5%) **a ≥ 100 MB/s di decompressione** per thread (LZMA `-mx9`:
~150 MB/s). Sotto, non vale un formato in più: brotli (23,5% a 445 MB/s) e BWT (22,4% a
25 MB/s per thread, 117 con 20) coprono già le due direzioni.

## 2. Metodo

`lzmodel.c` riproduce il modello di probabilità di LZMA (stati, isMatch/isRep, albero dei
letterali con lo schema "matched", lunghezze, slot e bit delle distanze, allineamento) e
lo applica alle decisioni del parser ottimo di LZMA registrate da `lztrace` (design 0013),
sommando il costo ideale `-log2 p` di ogni bit. Il parsing è quindi fisso: si misura solo
quanto un modello diverso guadagna sugli **stessi** token. Varianti, scelte perché
economiche in decodifica (un accesso in più per bit, o poche tabelle):

- **contatori**: LZMA aggiorna ogni probabilità con shift 5; `dual` = due probabilità
  (shift 4 e 7) mediate; `cnt:N` = velocità 1/(n+2) fino a n = N poi fissa;
- **letterali**: `lc3` = LZMA (3 bit alti del byte precedente); `lc8` = byte precedente
  intero; `mix2` = mixing logistico (pesi per nodo dell'albero) delle predizioni lc3 e
  lc8; `mix3` = mix2 più un contesto di ordine 2 hashed (16 bit);
- **APM** (stima secondaria) sui bit isMatch, contesto = stato e i 4 bit alti dei due byte
  precedenti;
- **distanze**: contesto dello slot esteso con lo slot del match precedente (`-d`).

**Validazione**: la base riproduce la dimensione reale di LZMA: Silesia 48 697 738 byte
contro 48 710 980 del flusso vero (−0,03%, precisione a 16 bit delle probabilità e
arrotondamento del range coder), webster 8 386 001 contro 8 387 246, mozilla 13 341 095
contro 13 341 400.

## 3. Risultati

Token di `-mx9` (dizionario 64 MiB, fb 64). Rapporto in percento dell'originale; in
parentesi la differenza in punti rispetto a LZMA.

| Variante | Silesia (212 MB) | webster (testo) | mozilla (binario) |
|---|---|---|---|
| **LZMA** (shift 5, lc 3) | **22,975** | **20,227** | **26,046** |
| contatori `dual` | 22,908 (−0,07) | 20,189 (−0,04) | 25,972 (−0,07) |
| contatori `cnt:30` | 22,974 (0) | 20,226 (0) | 26,043 (0) |
| contatori `cnt:62` | 22,963 (−0,01) | 20,174 (−0,05) | 26,159 (+0,11) |
| contatori `cnt:126` | 23,031 (+0,06) | 20,174 (−0,05) | 26,383 (+0,34) |
| letterali lc 2 | 23,022 (+0,05) | 20,234 (+0,01) | 26,089 (+0,04) |
| letterali lc 4 | 22,968 (−0,01) | 20,220 (−0,01) | 26,050 (0) |
| letterali lc 8 | 22,933 (−0,04) | 20,182 (−0,05) | 26,015 (−0,03) |
| letterali `mix2` | 22,795 (−0,18) | 20,171 (−0,06) | 25,759 (−0,29) |
| letterali `mix3` | 22,663 (−0,31) | 20,146 (−0,08) | 25,707 (−0,34) |
| APM su isMatch | 22,916 (−0,06) | 20,214 (−0,01) | 25,988 (−0,06) |
| slot con contesto del match precedente | 23,008 (+0,03) | 20,249 (+0,02) | 26,117 (+0,07) |
| `dual` + lc 8 | 22,877 (−0,10) | 20,151 (−0,08) | 25,951 (−0,10) |
| `dual` + APM | 22,853 (−0,12) | 20,178 (−0,05) | 25,916 (−0,13) |
| `dual` + `mix2` | 22,750 (−0,22) | 20,134 (−0,09) | 25,720 (−0,33) |
| **`dual` + `mix3` + APM** | **22,568 (−0,41)** | 20,099 (−0,13) | 25,615 (−0,43) |

Dove sta l'output di LZMA su Silesia (base → variante più ricca):

| flusso | LZMA | `dual`+`mix3`+APM | quota |
|---|---|---|---|
| letterali (20,2 M) | 17 766 KB | 17 096 KB (−3,8%) | 36% |
| flag (match/rep/stato) | 3 659 KB | 3 512 KB (−4,0%) | 7,5% |
| lunghezze | 5 324 KB | 5 306 KB (−0,3%) | 11% |
| distanze (10,3 M match) | 21 949 KB | 21 921 KB (−0,1%) | **45%** |

Costo: lo strumento (costo ideale, senza range coder) passa da 1,8 s a 10,1 s con `mix3`
(+8 s per 20 M letterali: `exp`/`log` in doppia precisione, non ottimizzato). Un decoder
vero con mixer a tabelle (stretch/squash a 12 bit, come lpaq) costa per bit di letterale
2-3 letture di modello, un prodotto scalare e l'aggiornamento di 2-3 pesi: su un decoder
LZMA che fa ~150 MB/s con un accesso per bit, i letterali (36% dei byte, 8 bit ciascuno)
porterebbero la velocità sotto i 70-80 MB/s. I contatori `dual` costano una lettura e una
scrittura in più per bit e raddoppiano le tabelle (cache): stimati −10/−15%.

## 4. Lettura

1. **Il modello di LZMA è vicino al suo ottimo**. Tutto l'arsenale "CM leggero" (mixing
   di tre ordini sui letterali, stima secondaria, contatori a doppia velocità) vale
   **0,41 punti** su Silesia, un quarto della soglia, e già fuori dal vincolo di velocità.
   Le varianti a costo quasi nullo (contatori, lc 8, APM) valgono 0,04-0,12 punti: non
   giustificano un formato incompatibile con 7-Zip.
2. **Il 45% dell'output sono distanze**, in gran parte bit "diretti" (uniformi per
   costruzione) che nessun contesto comprime: la variante con contesto peggiora. Le
   lunghezze (11%) non si muovono. Resta il 36% dei letterali, dove 1,5 punti
   richiederebbero −18%: è il territorio dei CM completi (ordini 1-6, SSE in catena,
   modelli di parola), cioè PAQ/zpaq a 1-5 MB/s di decodifica.
3. Guadagni sulle distanze passano per il **parsing** (più rep, match più lunghi, costi
   diversi), non per il modello: ma il parsing ottimo di LZMA con fb 64 lascia pochissimo
   (design 0004/0005: oracle bt4 −0,08%), e cambiare la struttura dei token (più rep, LZ con
   sequenze) è il prototipo di design 0013, già misurato peggio di brotli.
4. Letteratura coerente: i formati LZ + range coder binario più forti di LZMA di qualche
   punto (Razor, lzham-CM, i modelli LZ di zpaq) decomprimono a 20-40 MB/s per thread;
   a 100 MB/s e oltre nessuno batte LZMA di 1,5 punti sui dati misti.

**Decisione**: la strada 2 è chiusa. Lo strumento resta in `tools/research/lzmodel` per
misurare in futuro idee diverse (per esempio un modello dei letterali scelto per blocco
dall'encoder, o costi statici nel parser), che però partono dallo stesso tetto: 0,4 punti
sui letterali, nulla su distanze e lunghezze. Per il rapporto Heptazip punta sul BWT (testi)
e su LZMA2 `-mx9` (binari); per la velocità su zstd e brotli.

## 5. Come riprodurre

```sh
gcc -O2 -DZ7_LZMA_TOKEN_TRACE -DZ7_ST -o lztrace tools/research/lzstatic/lztrace.c \
    C/LzmaEnc.c C/LzFind.c C/LzFindOpt.c C/CpuArch.c C/Alloc.c
./lztrace bench-data/silesia.tar sil.tok 9 26          # ~1 minuto, stampa la dimensione LZMA
gcc -O2 -o lzmodel tools/research/lzmodel/lzmodel.c -lm
./lzmodel bench-data/silesia.tar sil.tok                # base: deve dare ~48 697 738
./lzmodel bench-data/silesia.tar sil.tok -c dual -l mix3 -a
```
