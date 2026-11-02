# Tensor7

Tensor7 è un fork di [7-Zip](https://7-zip.org) 26.05 (Igor Pavlov) per Linux e
compilatori moderni: più veloce in compressione e decompressione, parallelo dove 7-Zip
non lo è, e con un metodo nuovo per i modelli di intelligenza artificiale. Il programma
resta `7zz`, con gli stessi comandi e opzioni; gli archivi `.7z` con i metodi standard
si aprono con qualunque 7-Zip.

## Cosa aggiunge

- **Metodo FLOAT** per i pesi dei modelli AI, senza perdita e scelto in automatico per
  `.safetensors` e `.gguf`: BF16/FP16/FP32 con l'esponente in Huffman, GGUF quantizzati
  (Q2_K … Q8_0) con un modello a contesti e rANS. Esempi (`7zz a` senza opzioni):
  Qwen3-0.6B BF16 al 66,3% (LZMA2 70,2%) in 1 s invece di 90; DeepHat-7B Q4_K_M al
  93,2% (LZMA2 97,8%) 18 volte più veloce.
- **zstd** nel formato 7z, per profili "ultra-veloci".
- **Estrazione e compressione parallele** dei folder 7z (`-mpfd`, `-mpfc`), con archivi
  identici byte per byte a quelli a un thread.
- **gzip e zip in parallelo**: Deflate a blocchi su tutti i core, 8,6 volte più veloce
  (`-tgzip -mx5`: 105 MB/s, meglio di pigz -9 come rapporto), file leggibili da
  qualunque gzip/unzip e identici per ogni numero di thread. I blocchi sono compressi
  da libdeflate: `-mx3` comprime più di pigz -6 a 2,5 volte la velocità (820 MB/s),
  `-mx1` supera 1 GB/s. Decompressione Deflate 2,7-3 volte più
  veloce (silesia 800 MB/s, come libdeflate entro l'8%; sul corpus più veloce di libdeflate).
- **Più velocità**: assembly attivo di default (LZMA 1,4x in decompressione), CRC32 con
  PCLMULQDQ, match finder ottimizzato, decoder SIMD (AVX2, NEON).
- **Build moderno** con CMake (GCC/Clang, C++20, LTO/PGO opzionali), test con CTest,
  sanitizer, fuzzing (libFuzzer) e build ARM64.

Gli archivi con FLOAT si estraggono solo con Tensor7: per la compatibilità con 7-Zip
basta `-mf=off`.

## Documentazione

- [BUILD-modern.md](BUILD-modern.md): compilare, testare, usare le opzioni nuove.
- [docs/design](docs/design): una nota per ogni scelta (formati, misure, alternative).
- [docs/benchmark-mercato.md](docs/benchmark-mercato.md): confronto con zstd, xz,
  brotli, pigz e altri.

## Versioni

`26.05-m1`: la base 7-Zip (26.05) e il rilascio di Tensor7 su quella base.

## Branch

| branch | contenuto |
|---|---|
| `develop` | sviluppo: base delle pull request, build e test (GitHub Actions) a ogni push |
| `modern` | linea stabile, aggiornata da `develop` a ogni release |
| `main` | 7-Zip 26.05 originale, come riferimento (non riceve il lavoro di Tensor7) |

Ogni modifica passa da una issue, un branch da `develop` e una pull request.

## Licenza e crediti

Tensor7: Copyright (c) 2026 Mario Trombino. Il codice scritto per Tensor7 è distribuito
con la licenza BSD a 2 clausole: chi lo riusa deve mantenere l'avviso di copyright e il
testo della licenza. Dettagli, file per file: [LICENSE-Tensor7](LICENSE-Tensor7).

7-Zip è di Igor Pavlov: il codice originale mantiene le sue licenze (GNU LGPL, con la
restrizione unRAR per il codice RAR, e le parti BSD; vedi `DOC/License.txt`). zstd è
distribuito con licenza BSD, il compressore di libdeflate con licenza MIT. Tensor7 non è un prodotto ufficiale di 7-Zip.
