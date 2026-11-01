# 0008 — Metodo FLOAT: compressione senza perdita dei pesi dei modelli AI

| | |
|---|---|
| **Stato** | v1 implementata: blocchi split/zstd/stored, multi-thread, scelta automatica per safetensors e GGUF, test, fuzzing, benchmark |
| **Data** | 2026-11-01 |
| **Codice** | `C/FloatPlane.{h,c}` (formato a blocchi), `C/FloatPlaneLz.{h,c}` (zstd per i blocchi non float), `CPP/7zip/Compress/FloatCoder.{h,cpp}`, `FloatRegister.cpp`, analisi in `7zUpdate.cpp` |
| **ID del metodo** | `3F 70 7F 5E 72 8B 00 01` (`0x3F707F5E728B0001`), nome `FLOAT` |
| **Uso** | automatico per `.safetensors` e `.gguf` F16/BF16/F32 (§3.5); esplicito `-m0=FLOAT` (opzionale `:c=4m`, blocchi da 64 KiB a 4 MiB); `-mf=off` disattiva la scelta automatica |

---

## 1. Perché

I modelli pubblicati in BF16/FP16 (safetensors, GGUF F16) pesano gigabyte e vengono
scaricati milioni di volte. Le misure del design 0006 sul modello Qwen3-0.6B in BF16
(1503 MB):

| Metodo | Dimensione | Note |
|---|---|---|
| zstd -3 | 78,1% | |
| zstd -19 | 75,5% | |
| LZMA2 `-mx5` | 70,2% | |
| piani di byte + zstd -19 sull'esponente | 66,6% | prototipo |
| **limite di ordine 0** (esponente; segno e mantissa grezzi) | **66,2%** | |

In un peso BF16 (1 bit di segno, 8 di esponente, 7 di mantissa) l'esponente ha 2,6 bit di
entropia; segno e mantissa sono quasi casuali (7,97 bit su 8, 7,93 dato l'esponente). Un
contesto (esponenti precedenti) non aiuta: 2,62 bit invece di 2,63. Quindi:

- tutto il guadagno possibile viene dall'esponente, e un codificatore di ordine 0 ci arriva;
- LZ (LZMA, zstd) spende tempo a cercare ripetizioni che nei pesi non ci sono.

Un codificatore di Huffman sull'esponente, con la mantissa copiata, arriva al 66,5% a
velocità di centinaia di MB/s: più piccolo di LZMA2 e molto più veloce.

## 2. Alternative valutate

| | Rapporto | Velocità | Lavoro | Decisione |
|---|---|---|---|---|
| A. Filtro di trasposizione (come Delta/BCJ) + LZMA2 | 67,4% | come LZMA2 (lento) | piccolo | no: non risolve la velocità |
| A'. Filtro + zstd -19 | 66,6% | zstd -19 (lento) | piccolo | no |
| **B. Codec dedicato: piani + Huffman per blocco** | **66,5%** | **centinaia di MB/s** | medio | **sì** |
| C. B con codifica aritmetica/rANS (esponente; mantissa dato l'esponente) | ~66,0% | più lento di Huffman | grande | metà del guadagno ottenuta con k (§3.2) |

Huffman sul piano di segno e mantissa non guadagna nulla (7,97 bit: Huffman non sa usare
frazioni di bit), quindi in v1 è copiato.

## 3. Formato

### 3.1 Coder 7z

- ID `3F 70 7F 5E 72 8B 00 01`: un "developer ID" casuale del fork (prefisso `3F`,
  regola 2 di `DOC/Methods.txt`), metodo `0001`. 7-Zip e gli altri fork lo segnalano come
  metodo non supportato, come ogni codec sconosciuto.
- Proprietà: 1 byte, log2 della dimensione del blocco, 16..22 (default 20, 1 MiB).
- Un coder semplice (1 ingresso, 1 uscita): si combina con AES, si usa in archivi solid o
  non solid, con altri metodi in altri folder.

### 3.2 Blocchi

Lo stream è una sequenza di blocchi indipendenti; ognuno decodifica al più la dimensione
del blocco. Interi little-endian.

```
u8  mode        0 = stored, 1 = split, 2 = zstd
u32 outLen      byte decodificati, 1 .. dimensione del blocco
mode 0: outLen byte grezzi
mode 2:
  u32 lzSize    byte del frame, con 9 + lzSize < 5 + outLen (più piccolo del blocco stored)
  lzSize byte   un solo frame zstd (RFC 8878) che decodifica esattamente outLen byte
mode 1:
  u8  type      0 = BF16 (s = 7), 1 = FP16 (s = 8), 2 = FP32 (s = 23); w = 2, 2, 4
  u8  phase     0 .. w-1, byte grezzi prima del primo elemento
  u8  k         0 .. 2, bit di mantissa nel simbolo di Huffman
  phase byte grezzi, poi tail = (outLen - phase) mod w byte grezzi (la coda del blocco)
  (256 << k) / 2 byte  lunghezze dei codici di Huffman di e, 0..12, simbolo 2i nel nibble basso
  u32 huffSize[4]  byte di ognuno dei 4 flussi di Huffman
  4 flussi      flusso j: i codici degli elementi k con k mod 4 = j, canonici, MSB first,
                riempimento a zero fino al byte
  ceil(n * rb / 8) byte  r, rb = 8w - 8 - k bit per elemento, impacchettati little-endian
```

I 4 flussi interlacciati (come huff0 di zstd) sono catene di dipendenze indipendenti: il
decoder ne fa avanzare quattro insieme e la CPU li esegue in parallelo. Con un flusso solo
ogni simbolo aspettava lo shift del precedente (~10 cicli per peso): il decoder del nucleo
passa da 887 a 1900 MB/s su un thread, al costo di 12 byte e del riempimento per blocco
(+19 KB su 1 GB).

Per ogni elemento x (w byte, little-endian) di esponente in posizione s, con SK = s - k:

```
e = (x >> SK) & ((256 << k) - 1)                         esponente e k bit di mantissa:
                                                         simbolo di Huffman di 8 + k bit
r = (x & ((1 << SK) - 1)) | ((x >> (SK + 8 + k)) << SK)  gli altri bit, impacchettati
x = (r & ((1 << SK) - 1)) | (e << SK) | ((r >> SK) << (SK + 8 + k))
```

Una sola formula per i tre tipi e i tre k. **k** sposta nel simbolo i bit alti della
mantissa, che dato l'esponente non sono uniformi (in Qwen il primo vale 0,95 bit, i primi
due 1,94): con k = 2 il blocco BF16 scende dal 66,58% al 66,27%, metà del guadagno teorico
di un codificatore aritmetico (§2, alternativa C), con lo stesso decoder di Huffman. Con
k = 3 si peggiora (tabella più grande, limite di 12 bit). L'encoder sceglie k per blocco
dalla stima di entropia: dati con mantissa uniforme restano a k = 0. Codici canonici come in Deflate: prima i più corti, a
parità di lunghezza per valore del simbolo.

Il decoder rifiuta:
- lunghezze oltre 12;
- un codice sovra-assegnato (somma di Kraft > 1);
- una sequenza di bit che non è un codice;
- per ogni flusso, un `huffSize[j]` fuori dai limiti (almeno 1 bit, al più 12 per codice)
  o diverso dai byte effettivamente usati dai suoi codici;
- un `outLen` oltre il buffer.

Non legge mai fuori dall'input: gli zeri dopo la fine del flusso sono virtuali, e se
vengono consumati è un errore.

Il modo 2 è decodificato dal decoder zstd di 7-Zip (`C/ZstdDec.c`, sempre presente)
nella modalità che scrive nel buffer del chiamante: nulla oltre `outLen` e nessuna
finestra allocata, qualunque cosa dichiari il frame. È accettato solo un frame di dati,
senza frame "skip" e senza byte dopo la fine.

### 3.3 Encoder

Per ogni blocco:

1. Su un campione di 16 KiB prova i 7 candidati (BF16 e FP16 con fase 0-1, FP32 con fase
   0-3) e stima il costo per byte: entropia dell'esponente più i bit copiati. Sceglie il
   minore. Così trova da solo il tipo e l'allineamento anche in un archivio solid con file
   di dimensioni dispari, intestazioni JSON e tensori di tipi diversi: nessuna opzione da
   indicare a mano.
2. Istogramma del blocco intero (uno per flusso) dei simboli a k = 2; per somma quelli a
   k = 1 e 0. k dalla stima di entropia più i bit di r e la tabella; poi
   `Huffman_Generate` di 7-Zip (`C/HuffEnc.c`), lunghezza massima 12, dimensione esatta
   di ogni flusso. Codici e piano r si scrivono in un solo passaggio.
3. zstd (livello 3) su un campione di 16 KiB dall'inizio del blocco: se costa meno della
   stima float, zstd comprime il blocco intero. Vince il più piccolo tra split, zstd e
   stored (+5 byte), misurati esattamente. Il campione costa l'1,6% del tempo
   dell'encoder; senza l'encoder zstd (opzione CMake `SEVENZIP_ZSTD_ENCODER=OFF`) il
   modo 2 non viene usato, ma è sempre decodificato.

Il modo 2 serve perché i file dei modelli non contengono solo float: maschere di
attenzione (0/1, salvate in U8 o in FP32), padding, metadati di testo nei GGUF. In
pythia-160m le maschere U8 sono 48 MB su 375: senza il modo 2 FLOAT le lasciava al 56%,
zstd le porta allo 0,1%.

Su dati FP16 gaussiani l'encoder sceglie spesso la separazione BF16 (bit 7-14:
esponente e 3 bit di mantissa, senza segno), che costa quanto quella FP16. È la scelta per
costo misurato, non un errore.

### 3.4 Decoder

Due fasi per gruppi di 2048 elementi:

1. **simboli**: i 4 flussi di Huffman in un buffer da 4 KB (cache L1). È scalare per
   natura (ogni simbolo dipende dallo shift del precedente nel suo flusso); i 4 flussi
   vanno in parallelo. Da sola: ~2300 MB/s di pesi BF16 su un core.
2. **ricomposizione**: elementi dai simboli e dal piano r impacchettato. Con AVX2 (scelto a
   runtime con `CPU_IsSupported_AVX2()`) 16 elementi per iterazione:
   - un load da 32 byte dei simboli e uno da 16 byte dei 16 campi di r, copiato nelle due
     metà;
   - `vpshufb` porta in ogni lane i 2 byte del suo campo, e lo shift diverso per lane
     (AVX2 non ha `vpsrlvw`) è una moltiplicazione per 2^(8 - sh) seguita da uno shift di 8;
   - JOIN con shift costanti e uno store da 32 byte.

   Senza AVX2 (e per FP32) c'è un ciclo scalare per ogni (tipo, k).

La versione con un solo ciclo per elemento era a ~1360 MB/s con GCC: l'estrazione scalare
dei campi impacchettati pesava il 42% del tempo. Con le due fasi e AVX2 il nucleo
decodifica a ~1980 MB/s, oltre i 1900 del formato senza k. `FloatPlane_SetSimd()` spegne
il SIMD per i test: il test unitario prova entrambi i percorsi, e il fuzzer confronta SIMD
e scalare su ogni input (stesso esito, stessi byte).

### 3.5 Scelta automatica

L'analisi dei file del 7z (che sceglie già BCJ/ARM64 per gli eseguibili e Delta per i
WAV) legge i primi 16 KiB dei file `.safetensors` e `.gguf`:

- **safetensors**: intestazione valida (u64 da 2 B a 100 MiB, poi `{`); FLOAT se nel
  JSON letto i dtype `BF16`, `F16`, `F32` sono più frequenti degli altri (interi, `F8_*`,
  `F64`, `BOOL`): i modelli quantizzati restano a LZMA2;
- **GGUF**: magic, versione 2 o 3, chiave `general.file_type` = 0 (F32), 1 (F16) o
  32 (BF16); i tipi quantizzati (Q4_K, Q8_0...) restano a LZMA2.

I file riconosciuti formano un gruppo, che usa FLOAT al posto del metodo principale se
questo è LZMA o LZMA2 da solo (il default). Le altre scelte esplicite (zstd, PPMd, `-mx0`,
catene con filtri) restano; `-mf=off` disattiva l'analisi. In un aggiornamento, i file
nuovi vanno nel gruppo dei folder FLOAT esistenti. Il gruppo è un marcatore interno
(`k_FLOAT_FilterGroup`), mai scritto nell'archivio.

## 4. Risultati

Tre modelli reali da Hugging Face, un tipo ciascuno:

| Modello | Tipi | FLOAT (automatico) | LZMA2 `-mx5` | xz -9 | zstd -19 |
|---|---|---|---|---|---|
| Qwen3-0.6B (1503 MB) | BF16 | **66,28%** in 1,0 s | 70,24% | 70,06% | 75,48% |
| pythia-160m (375 MB) | FP16 + maschere U8 | **71,84%** in 0,3 s | 76,13% in 11,1 s | 75,98% | 78,35% |
| gpt2 (548 MB) | FP32 (maschere in FP32) | **76,60%** in 0,4 s | 83,34% in 13,6 s | 82,62% | 84,31% |

Su tutti e tre FLOAT è il più piccolo di 4-6 punti e 30-90 volte più veloce di LZMA2
`-mx5`. Senza il modo 2 pythia era al 79,30% e gpt2 all'81,39%. I soli float di pythia:
FLOAT 82,85%, LZMA2 87,93%. Dati sintetici gaussiani (test unitario): BF16 66,2%, FP16
84,3%, FP32 83,1%.

Benchmark a macchina scarica (20 thread, `tools/bench/market-bench.py`, tabella completa
in `docs/benchmark-mercato.md`):

| Metodo | Rapporto | Compr. MB/s | Decompr. MB/s | CPU compr. s | RSS MB |
|---|---|---|---|---|---|
| **FLOAT** | **66,58%** | **1521** | **2081** | **4,4** | 72 |
| LZMA2 `-mx5` | 70,24% | 17 | 264 | 1252 | 4440 |
| xz -9 -T0 | 70,06% | 3,6 | 185 | 1794 | 6196 |
| zstd -19 -T0 | 75,48% | 12 | 479 | 1211 | 1523 |
| zstd -3 -T0 | 78,12% | 2276 | 1122 | 4,9 | 187 |

Velocità nell'archivio (CRC compreso), Qwen BF16:

| | 1 thread | 20 thread |
|---|---|---|
| compressione | 475 MB/s | 1500 MB/s |
| decompressione | 1616 MB/s | 2150 MB/s |

Con 7 MB di memoria a thread singolo; con N thread l'archivio è identico byte per byte.
Storia del decoder a un thread nell'archivio: 750 MB/s con un flusso di Huffman, 1460 con
4 flussi, 1105 con i bit di mantissa nel simbolo (k, −0,30% di dimensione: costava
l'estrazione dei campi impacchettati di r), **1616** con le due fasi e AVX2 (§3.4).
pythia FP16 1785 MB/s, gpt2 FP32 1827 MB/s.

**ZipNN** 0.5.4 (PyPI) arriva allo stesso rapporto (66,2-66,6%), ma qui non è senza
perdita: la decompressione restituisce dati diversi dall'originale con ogni
configurazione provata, anche su 1 MiB di byte 0..255 (Python 3.12, x86-64). Le sue
velocità, misurate a macchina carica (con un thread circa 290 MB/s in compressione e 700
in decompressione), sono solo indicative.

## 5. Verifica

- `tests/unit_float_plane.c` (CTest, 19000 controlli): round-trip di BF16, FP16, FP32,
  costanti e casuali, fasi 0-3, dimensioni da 1 byte a 4 MiB; scelta di tipo e fase; modo
  2 per maschere, costanti e testo, rifiuto di un frame con byte in più, "non supportato"
  senza decoder; ogni byte corrotto e ogni troncamento di blocchi piccoli. Con ASan e
  UBSan.
- `tests/update_equivalence.py` (65 casi): tre profili `FLOAT` espliciti; nei dati di
  tutti i casi un safetensors sintetico (BF16 + maschera U8) e un GGUF F16, quindi la
  scelta automatica; un aggiornamento che aggiunge pesi a un archivio con un folder
  FLOAT. Archivi identici con `-mpfc=off/on`, estrazione corretta.
- `tests/fuzz/fuzz_float.c` (libFuzzer, con zstd reale): round-trip di qualunque input,
  decodifica di qualunque input (2 x 20 minuti, 4,5 milioni di esecuzioni, nessun
  problema).
- Interoperabilità: un archivio con blocchi zstd si estrae con una build senza encoder
  zstd; quella build produce archivi senza modo 2, estratti dalla build normale.
- Multi-thread: archivi identici con 1, 4 e 20 thread; 300 archivi corrotti a caso
  decodificati con `-mmt=on` dalla build ASan (tutti "errore nei dati", nessun crash);
  TSan pulito in compressione e decompressione.

## 6. Prossimi passi

- AVX2 anche per FP32; NEON per ARM64; la fase dei simboli resta il limite (~2300 MB/s
  per core).
- PyTorch `.bin`/`.pt` (archivi zip con i tensori): da valutare.
- v2 con rANS: circa mezzo punto, da valutare contro il costo in velocità.
