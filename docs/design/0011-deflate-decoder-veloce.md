# 0011 — Decoder Deflate veloce

| | |
|---|---|
| **Stato** | Implementato, attivo di default (gzip, zip, 7z, cab, nsis, qcow: tutto il Deflate e il Deflate64) |
| **Data** | 2026-11-02 |
| **Fase** | 2: prestazioni |
| **Riferimento upstream** | 7-Zip 26.05 (`b88c6bc`) |
| **File coinvolti** | `CPP/7zip/Compress/DeflateDecoder.{h,cpp}`, `CPP/7zip/Compress/BitlDecoder.h`, `CPP/7zip/Compress/LzOutWindow.h`, `CPP/7zip/Common/InBuffer.h`, `tests/deflate_mt.py` |

---

## 1. Problema

Il confronto con libdeflate (docs/benchmark-mercato.md) ha mostrato che il decoder Deflate
di 7-Zip era 2,7 volte più lento su silesia (318 contro 858 MB/s) e 1,8 volte sul corpus.
Il decoder è usato da ogni gzip e zip, cioè dalle operazioni più frequenti.

Il decoder di 7-Zip legge i bit un byte alla volta (`NBitl::CDecoder::Normalize()`, con i
bit invertiti per tabelle MSB-first), decodifica un simbolo per volta con controlli di
fine input a ogni simbolo, copia i match un byte alla volta e scrive in una finestra di
32 KiB (64 KiB per Deflate64) svuotata nello stream d'uscita a ogni giro.

libdeflate non si può usare come libreria: vuole tutto l'ingresso e tutta l'uscita in
memoria, mentre 7-Zip decomprime a flusso (stdin, file grandi, ripresa con `CodeResume`,
lettura dal coder con `ISequentialInStream::Read`, modalità NSIS). Le sue tecniche sì.

## 2. Soluzione

`CCoder::DecodeFast()` decodifica i simboli di un blocco Huffman come libdeflate:

- **bit buffer a 64 bit** ricaricato 8 byte alla volta senza salti
  (`bb |= GetUi64(in) << n; in += (63 - n) >> 3; n |= 56`);
- **tabelle LSB-first** (`_fastLit`: 11 bit + sottotabelle; `_fastDist`: 8 bit +
  sottotabelle) costruite in `ReadTables()` dalle stesse lunghezze dei codici accettate da
  `NHuffman::CDecoder::Build()`. Una voce contiene il letterale, o la base della lunghezza
  o della distanza con il numero di bit extra: codice e bit extra si consumano con uno
  shift;
- **fino a 3 letterali** per ricarica, e la voce del simbolo successivo letta prima della
  ricarica;
- **copie dei match a 8 byte**: 32 byte senza ciclo (la maggior parte dei match), poi a
  blocchi di 8; distanza 1 come riempimento; distanze 2..7 byte per byte;
- **finestra di 1 MiB** invece di 32/64 KiB: spazio per le copie, e scritture più grandi
  nello stream d'uscita.

`DecodeFast()` legge direttamente il buffer di `CInBuffer` e scrive in quello di
`CLzOutWindow` (accessi `Fast_*` aggiunti), quindi gira solo lontano dai loro limiti
(almeno 64 byte d'ingresso e 512 di uscita). **Tutto il resto torna al codice di 7-Zip
prima del simbolo**: i limiti dei buffer, la fine dell'input, i simboli 286/287, le
distanze oltre la storia o non valide, le copie che passano l'inizio della finestra senza
storia, i match più lunghi dello spazio rimasto (Deflate64). Il codice di 7-Zip decodifica
quel simbolo e poi `DecodeFast()` riprende. All'uscita i bit non usati tornano nel
`CInBuffer` (si riavvolge il puntatore ai byte non usati, `SetLastBits()` per i bit del
byte a metà), e lo stato del bit decoder è quello che avrebbe avuto il codice di 7-Zip.

### 2.1 Stesso risultato per ogni input

Il requisito è che per **qualunque** input, valido o no, l'uscita, il codice d'errore e i
byte scritti prima dell'errore siano quelli di 7-Zip 26.05. Due punti delicati:

- **La fine dell'input** si riconosce dal numero di byte letti dopo la fine
  (`ExtraBitsWereRead_Fast()`: 0xFF virtuali), che dipende da quanti byte il bit decoder
  ha letto in anticipo. Il primo test differenziale ha trovato 54 file troncati su 2268 con
  un byte in meno prima dell'errore: `DecodeFast()` normalizzava il bit decoder prima di
  controllare il margine d'ingresso, e vicino alla fine cambiava la lettura in anticipo.
  Ora il margine si controlla prima: vicino alla fine non tocca lo stato.
- **La finestra più grande** accettava distanze oltre 32 KiB, che prima `CopyBlock()`
  rifiutava (simboli di distanza 30 e 31 nei blocchi a codici fissi): il codice di 7-Zip
  ora le rifiuta esplicitamente.

## 3. Risultati

Un thread (core fisso), `7zz e -so` su `/dev/shm`, minimo di 7 ripetizioni; libdeflate
`libdeflate-gzip -d -c` (master dopo la 1.26):

| file | 7-Zip 26.05 | Heptazip | libdeflate |
|---|---|---|---|
| silesia, gzip -6 | 316 MB/s | **~820 MB/s** | 895 MB/s |
| silesia, Heptazip `-mx5` | 318 MB/s | **~825 MB/s** | 910 MB/s |
| silesia, zip Deflate64 | 334 MB/s | **~840 MB/s** | (non lo legge) |
| corpus, gzip -6 | 870 MB/s | **~2150 MB/s** | 1675 MB/s |

Da 2,5 a 2,6 volte il decoder di prima; sul corpus più veloce di libdeflate (qui conta la
finestra grande: i blocchi stored e i match lunghi vanno a memcpy). Su silesia resta un
8-10% sotto libdeflate, che decodifica tutto in memoria e non ha i controlli di flusso.

Passi misurati (silesia gzip -6, MB/s): finestra 1 MiB + percorso veloce 649; + 3
letterali per ricarica 713; + voce letta prima della ricarica 776; + copia di 16 byte
senza ciclo 810; + 32 byte 830. Senza effetto misurabile: dimensioni delle tabelle
(10..12 / 8..10 bit), flag nei bit alti, clang al posto di GCC.

## 4. Test

- **Differenziale** (scratchpad `deflate_diff.py`, non in CTest perché serve il binario di
  riferimento): 2268 + 3 × 4920 file (gzip di zlib con livelli 0-9, ogni strategia,
  finestre di 512 byte, flush; zip di zlib e di 7zz, Deflate64; uscite di 7zz a ogni
  livello) e le loro copie corrotte (bit invertiti, tagli, byte sovrascritti, vicino alla
  fine e ai confini di 1 MiB): uscita, codice d'uscita e messaggi identici a 7-Zip 26.05.
- `tests/deflate_mt.py`: ora anche la decodifica di gzip di zlib con ogni strategia,
  finestra di 512 byte e flush, interi e tagliati. Verificato che fallisce con un errore
  introdotto nella copia oltre l'inizio della finestra.
- Fuzzing: `fuzz_codec` Deflate e Deflate64, `fuzz_archive` gzip e zip (§5).
- Suite completa in release e ASan; ARM64 (qemu).

## 5. Fuzzing

Campagna di 20 minuti per bersaglio (ASan + UBSan), corpus delle campagne precedenti:

| bersaglio | esecuzioni | corpus | artefatti |
|---|---|---|---|
| `fuzz_codec` Deflate | 1,74 M | 1058 | 0 |
| `fuzz_codec` Deflate64 | 0,86 M | 817 | 0 |
| `fuzz_archive` gzip | 3,88 M | 4006 | 0 |
| `fuzz_archive` zip | 1,23 M | 11534 | 0 |

## 5.1 Benchmark di mercato

`market-bench.py` (2026-11-02), decompressione a un thread:

```
input    strumento   profilo                  rapporto comp MB/s  dec MB/s comp CPU s dec CPU s comp RSS  dec RSS
-----------------------------------------------------------------------------------------------------------------
silesia  7zz-modern  -tgzip -mx1                32.98%     537.3     780.5        7.1      0.27       80        7
silesia  7zz-modern  -tgzip -mx3                32.14%     346.2     785.6       10.8      0.27       80        7
silesia  7zz-modern  -tgzip -mx5                30.99%     107.0     800.0       37.4      0.26       94        7
silesia  7zz-26.05   -tgzip -mx1                33.15%      73.9     279.6        2.9      0.76        7        6
silesia  7zz-26.05   -tgzip -mx5                30.96%      12.4     298.5       17.9      0.73        7        6
silesia  pigz        -6 -p 20                   32.20%     333.4     325.7       12.2      0.76       16        2
silesia  pigz        -9 -p 20                   31.92%     151.1     328.3       25.8      0.77       16        2
silesia  gzip        -6                         32.19%      37.9     235.7        5.8      0.88        2        2
silesia  libdeflate  -6                         31.85%     135.9     866.3        1.6      0.23      268      268
silesia  libdeflate  -9                         31.48%      70.0     860.1        3.1      0.24      268      267
corpus   7zz-modern  -tgzip -mx1                66.52%     478.7    2096.3        3.9      0.05      100        7
corpus   7zz-modern  -tgzip -mx3                66.36%     443.4    2172.9        4.7      0.04      100        7
corpus   7zz-modern  -tgzip -mx5                66.09%     222.5    2227.7        8.8      0.04      110        7
corpus   7zz-26.05   -tgzip -mx1                66.61%      82.4     729.4        1.5      0.15        7        6
corpus   7zz-26.05   -tgzip -mx5                66.07%      30.0     734.9        3.9      0.15        7        6
corpus   pigz        -6 -p 20                   66.37%     436.8     564.8        4.6      0.23       17        2
corpus   pigz        -9 -p 20                   66.32%     298.9     574.7        6.7      0.23       17        2
corpus   gzip        -6                         66.37%      50.7     282.0        2.2      0.39        2        2
corpus   libdeflate  -6                         66.26%     129.7    1537.7        0.9      0.07      182      181
corpus   libdeflate  -9                         66.20%     106.7    1588.8        1.1      0.06      182      181
```

Decompressione: silesia 780-800 MB/s (7-Zip 26.05: 280-300, pigz 326, libdeflate 860),
corpus 2100-2230 MB/s (26.05: 730, libdeflate 1540-1590). Con meno della metà della CPU di
26.05 (0,27 s contro 0,76 su silesia).

## 6. Come continuare

- Distanze 2..7: copie a parole con passo (libdeflate), per dati con molte ripetizioni corte.
- Decoder in parallelo per un solo gzip grande: non possibile in generale (i blocchi di
  Heptazip usano la storia del blocco precedente, design 0009).
- Estrazione zip in parallelo sui file (oggi un file alla volta), come per i folder 7z
  (design 0001): da misurare.
