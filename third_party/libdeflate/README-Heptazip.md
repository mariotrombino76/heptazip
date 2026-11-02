# libdeflate (vendored, compressore)

Il compressore DEFLATE di **libdeflate**, usato dall'encoder Deflate del fork per i
blocchi paralleli di gzip, zip e 7z (`CPP/7zip/Compress/DeflateEncoder.cpp`, design
0012). Il decoder resta quello di 7-Zip (design 0011), e Deflate64 resta all'encoder di
7-Zip (libdeflate non lo supporta).

| | |
|---|---|
| Fonte | https://github.com/ebiggers/libdeflate |
| Versione | ramo master, commit `0758d0a12d34fac281b0fe27c5155fbe753d9511` (dopo la 1.26: compressione più veloce ai livelli 5-12) |
| Licenza | MIT (`COPYING`), compatibile con la LGPL di 7-Zip e con la BSD-2-Clause del fork |
| Build | `cmake/SevenZipLibdeflate.cmake`, opzione `SEVENZIP_LIBDEFLATE` (ON) |

File copiati (solo il compressore):

- `COPYING`, `libdeflate.h`, `common_defs.h`
- `lib/deflate_compress.c`, `lib/deflate_compress.h`, `lib/deflate_constants.h`,
  `lib/lib_common.h`, `lib/utils.c`, `lib/matchfinder_common.h`,
  `lib/hc_matchfinder.h`, `lib/ht_matchfinder.h`, `lib/bt_matchfinder.h`
- `lib/x86/matchfinder_impl.h`, `lib/x86/cpu_features.h`, `lib/arm/matchfinder_impl.h`,
  `lib/arm/cpu_features.h`, `lib/riscv/matchfinder_impl.h`

Il commit `072f3d3` contiene i file originali; le modifiche Heptazip sono nei commit
successivi (`git diff 072f3d3 -- third_party/libdeflate`).

## Modifiche Heptazip

Tutte marcate `Heptazip` nel codice:

1. **`libdeflate_deflate_compress_chunk()`** (nuova, in `libdeflate.h` e
   `deflate_compress.c`): comprime `in_nbytes` byte dopo `dict_nbytes` byte di storia
   (al più gli ultimi 32 KiB). Se `!final`, nessun blocco è finale e l'uscita finisce con
   un blocco stored vuoto su un confine di byte (come `Z_SYNC_FLUSH` di zlib): le uscite
   dei blocchi di uno stream si concatenano.
2. **Campi `hz_dict_nbytes` e `hz_final`** in `struct libdeflate_compressor`, impostati
   solo da `libdeflate_deflate_compress_chunk()` (0 e true altrimenti: le altre funzioni
   si comportano come prima).
3. **Storia nel match finder** in `deflate_compress_fastest()`, `_greedy()`,
   `_lazy_generic()` e `_near_optimal()`: dopo l'inizializzazione i byte di storia
   entrano nel match finder con le funzioni di skip di libdeflate
   (`ht/hc_matchfinder_skip_bytes()`, `bt_matchfinder_skip_byte()`), senza essere codificati.
4. **Blocco finale** solo se `hz_final`; `is_first` del parser quasi ottimo confronta con
   l'inizio dei dati dopo la storia.

## Aggiornamento

1. Copiare i file elencati dalla nuova versione in un commit a sé (file originali).
2. Riapplicare le modifiche (`git diff 072f3d3 <ultimo commit> -- third_party/libdeflate`).
3. Ricompilare, `ctest` (`deflate_mt` decomprime con zlib di Python ogni livello) e
   ripetere le misure del design 0012.
