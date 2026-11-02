# zstd (vendored)

Sottoinsieme della libreria **Zstandard** usato dall'encoder zstd del fork
(metodo `.7z` `0x4F71101`, compatibile con 7-Zip-zstd). La decompressione usa invece
il decoder di 7-Zip (`C/ZstdDec.c`), quindi qui non c'è `lib/decompress`.

| | |
|---|---|
| Versione | **1.5.7** (tag `v1.5.7`) |
| Fonte | https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz |
| SHA-256 del tarball | `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` (verificato con il `.sha256` della release) |
| Licenza | BSD (`LICENSE`) oppure GPLv2 (`COPYING`), a scelta: si usa la BSD, compatibile con la LGPL di 7-Zip |

Contenuto copiato senza modifiche:

- `lib/zstd.h`, `lib/zstd_errors.h`
- `lib/common/` (tutto)
- `lib/compress/` (tutto)

Escluso: `lib/decompress`, `lib/legacy`, `lib/deprecated`, `lib/dictBuilder`, `lib/dll`,
`programs/`, `tests/`, `contrib/`, `examples/`, `zlibWrapper/`, `doc/`.

## Aggiornamento

1. Scaricare il tarball della nuova release e verificarne lo SHA-256 con il file
   `.sha256` pubblicato.
2. Sostituire i file elencati sopra, aggiornare questa tabella.
3. Ricompilare, eseguire `ctest` e `tools/bench/roundtrip.sh`.

I file non vanno modificati: eventuali adattamenti si fanno nella build
(`cmake/SevenZipZstd.cmake`).
