# brotli (vendored)

La libreria **brotli** di Google (compressore e decompressore), usata dal metodo
`BROTLI` nei `.7z` (`CPP/7zip/Compress/BrotliCoder.cpp`, design 0014), compatibile con
7-Zip-zstd.

| | |
|---|---|
| Versione | **1.2.1** (tag `v1.2.1`, 2026-10-21) |
| Fonte | https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz |
| SHA-256 del tarball | `98d35d11a78dc453aebb7d10c3748f3f73ff78c8267c0dc697b8631f67bf7e8f` |
| Licenza | MIT (`LICENSE`), compatibile con la LGPL di 7-Zip e la BSD-2-Clause del fork |
| Build | `cmake/SevenZipBrotli.cmake`, opzione `SEVENZIP_BROTLI` (ON); makefile GCC e nmake |

Contenuto copiato senza modifiche: `LICENSE`, `c/common/`, `c/dec/`, `c/enc/`,
`c/include/`. Escluso: `c/tools/`, `docs/`, `go/`, `python/`, `java/`, `tests/`,
`scripts/`.

## Aggiornamento

1. Scaricare il tarball della nuova release e annotarne lo SHA-256.
2. Sostituire le cartelle elencate sopra (i file non vanno modificati) e aggiornare
   questa tabella e gli elenchi di oggetti dei makefile se cambiano i file `.c`.
3. Ricompilare ed eseguire `ctest` (`brotli_codec`, `roundtrip_smoke`).
