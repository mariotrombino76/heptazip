# 7-Zip modern fork — build e test

Questo branch (`modern`) affianca ai makefile originali un build system CMake
e un banco di prova per misurare correttezza e prestazioni di ogni modifica.
I makefile upstream (`CPP/7zip/Bundles/*/makefile.gcc`) restano intatti e
servono come riferimento.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/7zz
```

Con le opzioni di default la build è **equivalente a quella upstream**
(`Alone2/makefile.gcc`): stessi sorgenti, stessi flag (`-O2 -Wall -Wextra -Werror -fPIC`),
stesso ordine di link (e quindi stesso ordine di registrazione dei codec)
e archivi prodotti identici byte per byte.

| Opzione | Default | Effetto |
|---|---|---|
| `SEVENZIP_OPT_LEVEL` | `2` | livello `-O` per Release/RelWithDebInfo |
| `SEVENZIP_NATIVE` | `OFF` | `-march=native` — il binario gira solo su CPU compatibili |
| `SEVENZIP_LTO` | `OFF` | link-time optimization |
| `SEVENZIP_ASM` | `OFF` | usa `Asm/x86/*.asm` (CRC, AES, SHA, LzFind, **LzmaDecOpt**); richiede `asmc`, `uasm` o `jwasm` |
| `SEVENZIP_PGO` | `OFF` | `GEN` / `USE`: profile-guided optimization (usare `tools/pgo.sh`) |
| `SEVENZIP_CXX_STANDARD` | `17` | standard C++ (17/20/23) |
| `SEVENZIP_WERROR` | `ON` | warning = errore, come upstream |

Nota: con GCC `-Wmaybe-uninitialized` e `-Wstringop-overflow` non sono trattati come errori, perché con
PGO/LTO/`-O3` producono falsi positivi nel codice upstream.

### Note di progetto

- Le librerie interne sono **OBJECT library**, non statiche: codec e formati si
  registrano con costruttori statici, e una libreria statica li farebbe scartare
  dal linker.
- `cmake/SevenZipSources.cmake` è nell'ordine della linea di link upstream:
  non riordinarlo. Va aggiornato quando upstream cambia `Arc_gcc.mak` /
  `Alone2/makefile.gcc`.

## Test e benchmark (`tools/bench/`)

```sh
tools/bench/make-corpus.sh [--silesia]          # crea bench-data/corpus (~110 MiB)
tools/bench/roundtrip.sh build/7zz              # comprimi → test → estrai → diff, tutti i profili
tools/bench/compare-archives.sh A/7zz B/7zz     # output identico byte per byte?
tools/bench/bench.py up=A/7zz new=B/7zz         # velocità e rapporto di compressione
tools/pgo.sh [opzioni cmake]                    # build PGO in build-pgo/
```

`METHODS_FILTER='<regex>'` limita i profili usati dagli script shell
(es. `METHODS_FILTER=lzma2`).

Regole per ogni modifica:

1. `roundtrip.sh` deve passare sempre.
2. Se la modifica non deve cambiare l'output (refactoring, build, flag),
   `compare-archives.sh` contro il binario upstream deve dare `IDENTICI`.
3. Se la modifica riguarda le prestazioni, allegare l'output di `bench.py`
   (minimo 3 ripetizioni) con il confronto rispetto a upstream.
