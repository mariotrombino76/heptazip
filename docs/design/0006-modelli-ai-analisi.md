# 0006 — Compressione dei modelli AI: analisi

| | |
|---|---|
| **Stato** | Analisi dei modelli quantizzati conclusa (§3); modelli a 16 bit da misurare (§4) |
| **Data** | 2026-11-01 |
| **Strumenti** | `tools/bench/gguf_info.py` (composizione di un GGUF), `tools/bench/gguf_entropy.py` (limite teorico per flusso, richiede numpy) |

---

## 1. Domanda

I modelli AI quantizzati usano una compressione **con perdita** (pesi da 16-32 bit ridotti
a 8, 4 o 2-3 bit, con una scala per gruppi di 32-256 pesi: GGUF `Q4_K`/`Q6_K`, GPTQ, AWQ,
NF4; varianti con codebook come AQLM, QuIP#). Un archiviatore deve restituire i file
identici, quindi la quantizzazione in sé non si applica. Si possono però comprimere meglio
**senza perdita** i file dei modelli, con un filtro che conosce la loro struttura (come BCJ
per gli eseguibili)?

## 2. Due casi diversi

- **Modelli quantizzati** (GGUF `Q4_K_M` e simili): i valori a 4-6 bit sono già vicini alla
  loro entropia; struttura sfruttabile solo nelle scale dei blocchi.
- **Modelli a 16 bit** (BF16/FP16, safetensors su Hugging Face): nei pesi in virgola mobile
  il byte dell'esponente è molto ripetitivo, le mantisse quasi casuali. Separare i byte per
  "piano" e comprimere ogni piano a parte darebbe, secondo la letteratura (ZipNN,
  DFloat11), circa il 20-30% senza perdita: non ancora misurato qui.

## 3. Modelli quantizzati: misure

Due modelli GGUF v3 scaricati dall'utente:

| Modello | Dimensione | Q4_K | Q6_K | F32 |
|---|---|---|---|---|
| DeepHat-V1-7B `Q4_K_M` (qwen2) | 4466 MiB | 73,3% | 26,7% | 0,0% |
| qwen3-4b-cybersecurity `q4_k_m` (qwen3) | 2382 MiB | 70,8% | 29,1% | 0,0% |

Compressione del modello da 2,4 GB (1 file):

| Compressore | Dimensione | Tempo |
|---|---|---|
| 7zz LZMA2 `-mx5` | 97,82% | 61 s |
| 7zz LZMA2 `-mx5` con `lc=0 lp=0 pb=0` | 97,91% | 65 s |
| 7zz LZMA2 `-mx9` | 97,80% | 88 s |
| 7zz zstd `-mx19` / zstd `-19 -T0` | 97,61% | 106 / 99 s |
| xz `-6 -T0` | 97,88% | 91 s |

Limite teorico con i campi dei blocchi separati in flussi e codificati con un modello di
ordine 0 (`gguf_entropy.py`):

| Flusso | MiB | bit/byte |
|---|---|---|
| Q4_K valori a 4 bit | 1496,2 | 7,73 |
| Q6_K valori a 6 bit | 632,6 | 7,79 |
| Q4_K sottoscale (12 byte per blocco) | 140,3 | 7,13 |
| Q6_K scale int8 | 52,7 | 7,25 |
| Q4_K `d`, `dmin` (fp16) | 46,8 | 5,60 |
| Q6_K `d` (fp16) | 6,6 | 4,42 |
| **Totale** | 2375,9 | **95,57%** |

**Conclusione per i quantizzati**: i compressori attuali arrivano al 97,6-97,9%; un filtro
dedicato ai blocchi Q4_K/Q6_K potrebbe arrivare vicino al 95,6% (un modello con contesto,
per esempio sulle scale dei blocchi vicini, forse un punto in più): circa **2 punti** del
file, 50 MB su 2,4 GB. Guadagno modesto per un nuovo filtro nel formato.

## 4. Prossimo passo

Misurare il caso dei modelli a 16 bit, dove il margine atteso è molto più grande: scaricare
un modello pubblico piccolo in BF16 (safetensors), confrontare i compressori attuali con la
separazione per piani di byte (prototipo in Python), e solo con un guadagno confermato
(ordine del 20%) progettare un filtro nativo del 7z.
