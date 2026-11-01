# 0008 — Metodo FLOAT: compressione senza perdita dei pesi dei modelli AI

| | |
|---|---|
| **Stato** | v1 implementata: codifica a blocchi, metodo 7z `FLOAT`, test e fuzzing |
| **Data** | 2026-11-01 |
| **Codice** | `C/FloatPlane.{h,c}` (formato a blocchi), `CPP/7zip/Compress/FloatCoder.{h,cpp}`, `FloatRegister.cpp` |
| **ID del metodo** | `3F 70 7F 5E 72 8B 00 01` (`0x3F707F5E728B0001`), nome `FLOAT` |
| **Uso** | `7zz a -m0=FLOAT modello.7z model.safetensors` (opzionale `-m0=FLOAT:c=4m`, blocchi da 64 KiB a 4 MiB) |

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
| C. B con codifica aritmetica/rANS (esponente; mantissa dato l'esponente) | ~66,0% | più lento di Huffman | grande | in futuro, se mezzo punto vale |

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
u8  mode        0 = stored, 1 = split
u32 outLen      byte decodificati, 1 .. dimensione del blocco
mode 0: outLen byte grezzi
mode 1:
  u8  type      0 = BF16 (s = 7), 1 = FP16 (s = 8), 2 = FP32 (s = 23); w = 2, 2, 4
  u8  phase     0 .. w-1, byte grezzi prima del primo elemento
  phase byte grezzi, poi tail = (outLen - phase) mod w byte grezzi (la coda del blocco)
  128 byte      lunghezze dei codici di Huffman di e, 0..12, simbolo 2k nel nibble basso
  u32 huffSize  byte del flusso di Huffman
  huffSize byte n codici canonici, MSB first, riempimento a zero fino al byte
  n*(w-1) byte  r, little-endian, w-1 byte per elemento
```

Per ogni elemento x (w byte, little-endian) di esponente in posizione s:

```
e = (x >> s) & 0xFF                                    byte codificato con Huffman
r = (x & ((1 << s) - 1)) | ((x >> (s + 8)) << s)       gli altri w*8-8 bit, copiati
x = (r & ((1 << s) - 1)) | (e << s) | ((r >> s) << (s + 8))
```

Una sola formula per i tre tipi. Codici canonici come in Deflate: prima i più corti, a
parità di lunghezza per valore del simbolo.

Il decoder rifiuta:
- lunghezze oltre 12;
- un codice sovra-assegnato (somma di Kraft > 1);
- una sequenza di bit che non è un codice;
- un `huffSize` diverso dai byte effettivamente usati dagli n codici;
- un `outLen` oltre il buffer.

Non legge mai fuori dall'input: gli zeri dopo la fine del flusso sono virtuali, e se
vengono consumati è un errore.

### 3.3 Encoder

Per ogni blocco:

1. Su un campione di 64 KiB prova i 7 candidati (BF16 e FP16 con fase 0-1, FP32 con fase
   0-3) e stima il costo per byte: entropia dell'esponente più i bit copiati. Sceglie il
   minore. Così trova da solo il tipo e l'allineamento anche in un archivio solid con file
   di dimensioni dispari, intestazioni JSON e tensori di tipi diversi: nessuna opzione da
   indicare a mano.
2. Istogramma del blocco intero, `Huffman_Generate` di 7-Zip (`C/HuffEnc.c`) con
   lunghezza massima 12, dimensione esatta del risultato.
3. Se non è più piccolo del blocco grezzo, il blocco è *stored* (+5 byte). Su dati che non
   sono float il metodo costa al massimo 5 byte per blocco.

Su dati FP16 gaussiani l'encoder sceglie spesso la separazione BF16 (bit 7-14:
esponente e 3 bit di mantissa, senza segno), che costa quanto quella FP16. È la scelta per
costo misurato, non un errore.

## 4. Risultati

Qwen3-0.6B BF16 (1503 MB), archivio 7z con `config.json`: **66,58%** (1000,9 MB), file
identici. Dati sintetici gaussiani (test unitario): BF16 66,2%, FP16 84,3%, FP32 83,1%.

Velocità, memoria e confronto con i concorrenti: §6.

## 5. Verifica

- `tests/unit_float_plane.c` (CTest): round-trip di BF16, FP16, FP32, costanti e casuali,
  fasi 0-3, dimensioni da 1 byte a 4 MiB; scelta di tipo e fase; ogni byte corrotto e ogni
  troncamento di blocchi piccoli. Eseguito con ASan e UBSan.
- `tests/update_equivalence.py`: tre profili `FLOAT` (non solid, solid 2 MiB, blocchi
  64 KiB) su dati misti con pesi BF16 e FP32 sfasati di un byte. Archivi identici con
  `-mpfc=off/on`, estrazione corretta.
- `tests/fuzz/fuzz_float.c` (libFuzzer): round-trip di qualunque input, decodifica di
  qualunque input.

## 6. Prossimi passi

- Benchmark completo (macchina scarica): velocità, CPU, memoria, contro LZMA2, xz, zstd,
  e ZipNN (lo strumento di riferimento della letteratura).
- Multi-thread nel coder (i blocchi sono indipendenti) e stima per la compressione
  parallela dei folder (design 0003, `7zUpdateMt`).
- Scelta automatica del metodo per `.safetensors`, `.gguf` (tensori F16/BF16), `.pt`.
- v2 con rANS: circa mezzo punto, da valutare contro il costo in velocità.
