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
| `SEVENZIP_CXX_STANDARD` | `20` | standard C++ (17/20/23); upstream compila in gnu++17, il fork in gnu++20 (verificato: nessun warning con GCC 13 e Clang 20, archivi identici) |
| `SEVENZIP_ZSTD_ENCODER` | `ON` | encoder zstd per `.7z` con `third_party/zstd` (il decoder è sempre presente) |
| `SEVENZIP_WERROR` | `ON` | warning = errore, come upstream |

Nota: con GCC `-Wmaybe-uninitialized` e `-Wstringop-overflow` non sono trattati come errori, perché con
PGO/LTO/`-O3` producono falsi positivi nel codice upstream.

### Compilatori

Il compilatore si sceglie con le variabili standard di CMake, una directory di build
per ciascuno:

```sh
cmake -S . -B build-gcc16   -DCMAKE_C_COMPILER=gcc-16   -DCMAKE_CXX_COMPILER=g++-16
cmake -S . -B build-clang20 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
```

| Compilatore | Fonte | Stato |
|---|---|---|
| GCC 13.3 | Ubuntu 24.04 | riferimento (stesso di upstream `makefile.gcc`) |
| GCC 16 | PPA `ubuntu-toolchain-r/test` | **snapshot di sviluppo** `16.0.1 20260315`, precedente alla 16.1: da usare per diagnostica e compatibilità, non per binari di produzione o misure di prestazioni |
| Clang 20.1 | Ubuntu 24.04 | zero warning con `-Wall -Wextra` |

Con GCC ≥ 16 `-Warray-bounds` non è trattato come errore: segnala falsi positivi
dovuti alla devirtualizzazione speculativa (vedi `CMakeLists.txt`).

### Assembly x86-64 (consigliato)

`SEVENZIP_ASM=ON` usa le routine assembly upstream: **+22% in decompressione LZMA**
(`LzmaDecOpt.asm`) e qualche punto percentuale in compressione, con output identico.
Serve un assembler MASM-compatibile. UASM **v2.57r** compila su Linux, mentre
`master` non compila (include `<direct.h>`):

```sh
git clone https://github.com/Terraspace/UASM.git ~/.local/src/UASM
cd ~/.local/src/UASM && git checkout v2.57r
make -f Makefile-Linux-GCC-64.mak -j"$(nproc)" CFLAGS=-fcommon
cp GccUnixR/uasm ~/.local/bin/

cmake -S . -B build-asm -DCMAKE_BUILD_TYPE=Release -DSEVENZIP_ASM=ON \
      -DSEVENZIP_ASSEMBLER="$HOME/.local/bin/uasm"
```

### Note di progetto

- Le librerie interne sono **OBJECT library**, non statiche: codec e formati si
  registrano con costruttori statici, e una libreria statica li farebbe scartare
  dal linker.
- `cmake/SevenZipSources.cmake` è nell'ordine della linea di link upstream:
  non riordinarlo. Va aggiornato quando upstream cambia `Arc_gcc.mak` /
  `Alone2/makefile.gcc`.

## Estrazione parallela dei folder `.7z` (`-mpfd`)

```sh
7zz x archivio.7z              # attiva di default: folder diversi decompressi in parallelo
7zz x -mpfd=off archivio.7z    # estrazione sequenziale (comportamento upstream)
```

Accelera gli archivi con molti folder (non-solid `-ms=off`, `-ms=e`, solid limitati
`-ms=<dim>`, archivi molto grandi) anche se creati da 7-Zip upstream; non cambia il formato.
L'output e le callback sono identici all'estrazione sequenziale (vedi
`docs/design/0001-estrazione-parallela-folder-7z.md`). Usa al massimo `-mmt` thread e il
budget di memoria di decompressione (`-mmemuse`). I folder che producono più di 64 MiB
sono decompressi in linea con tutti i thread (il decoder LZMA2 a blocchi è più veloce
così). Risultati: da 1,15× a 2,9× sugli archivi con più folder (design 0001, §10). È un'opzione dell'handler 7z: come
`-mtf`, non è accettata da altri formati (zip, xz…).

## Compressione parallela dei folder `.7z` (`-mpfc`)

```sh
7zz a -ms=off archivio.7z dati/      # attiva di default: folder diversi compressi in parallelo
7zz a -mpfc=off archivio.7z dati/    # compressione sequenziale (comportamento upstream)
```

Quando l'archivio ha più folder (non solid `-ms=off`, `-ms=e`, `-ms=<dim>`), i folder
nuovi sono compressi in parallelo, ognuno da un proprio encoder con le **stesse opzioni**:
l'archivio è **identico byte per byte** a quello della compressione sequenziale (stesso
`-mmt`). Serve soprattutto con i metodi che da soli usano pochi thread (PPMd, Deflate,
LZMA, LZMA2 con folder più piccoli di un blocco, zstd con blocchi solid piccoli).
Risultati con 20 thread: Deflate 3,3×, PPMd 2,3×, Silesia non solid 2,1×, corpus
non solid 1,9×, zstd `-ms=16m` 1,75×; un archivio solid di default con eseguibili e
testi (due gruppi di filtri, compressi insieme) 1,37×; con un solo folder nessuna
differenza (design 0003, §10). Usa al
massimo `-mmt` thread e il limite di memoria di compressione (`-mmemuse`); i folder oltre
64 MiB, BCJ2, Copy e quelli che l'encoder comprime già con tutti i thread vanno in linea.
Opzione dell'handler 7z, come `-mpfd`.

## Zstandard nei `.7z` (`-m0=zstd`)

```sh
7zz a -m0=zstd -mx3 archivio.7z dati/      # veloce; livelli 1..22 (-mx o -m0=zstd:x=N)
7zz a -m0=zstd -mx19 -ms=32m archivio.7z dati/   # blocchi solid piccoli: estrazione piu' veloce
```

Metodo `0x4F71101`, compatibile con 7-Zip-zstd. L'estrazione usa il decoder di 7-Zip
(sempre presente, anche nelle build con i makefile originali); la compressione usa libzstd
1.5.7 inclusa in `third_party/zstd` (opzione CMake `SEVENZIP_ZSTD_ENCODER`, ON di default).
Gli archivi zstd non si aprono con 7-Zip upstream. Dettagli e misure:
`docs/design/0002-zstd-nel-formato-7z.md`.

## Output JSON dell'elenco (`l -json`)

```sh
7zz l -json archivio.7z | jq '.archives[0].items[] | {path, size}'
```

Un solo documento JSON su stdout, con gli stessi campi di `-slt` (nomi in camelCase, date
ISO 8601 UTC, dimensioni numeriche). Contratto completo: `docs/json-output.md`.

## Test automatici (CTest)

```sh
cmake --build build -j && ctest --test-dir build --output-on-failure
```

- `tests/extract_trace`: apre un `.7z` con l'handler 7z ed esegue `Extract()`
  registrando la sequenza di callback: elementi, modalità, risultati, dimensione e CRC
  dei dati per file, esito. Rileva le callback chiamate in concorrenza (violazione di
  `IArchive.h`) e può simulare annullamento (`--abort-at-file=N`) e scrittura fallita
  (`--fail-write-at-file=N`).
- `tests/extract_equivalence.py`: genera dati deterministici e 12 tipi di archivio
  (solid, non-solid, molti folder, BCJ2, PPMd, BZip2, Deflate, copia, AES, LZMA2 a
  blocchi), più copie corrotte o troncate e un archivio con un metodo sconosciuto in
  un folder, ed esegue 219 casi: selezioni parziali, modalità test, errori iniettati,
  password errata, zstd, dati estranei dopo la fine di un pack stream (`kDataAfterEnd`
  e `kDataError` segnalati a livello di blocco), un archivio da 3000 file (oltre la
  finestra di pianificazione dell'estrazione parallela). Configurazioni in CTest:
  `-mmt=1` contro `-mmt=on` e `-mpfd=off` contro `-mpfd=on` (default, memoria ridotta,
  2 thread). Confronta la traccia di **due
  configurazioni** (`--config-a/-b`) o di **due build** (`--trace-a/-b`), che devono
  essere identiche.

- `tests/unit_7zextract_mt`: test unitari di `7zExtractMt` e del pool `Common/MtTaskPool` (stima della memoria
  dei decoder, budget, stream su buffer, cache della password, pool di thread con stress).
- `tests/update_equivalence.py`: crea ogni archivio con due configurazioni
  (`--config-a/-b`, in CTest `-mpfc=off` contro `-mpfc=on`) e verifica che siano
  **identici byte per byte**, con la stessa console (`-bb3`) e lo stesso codice di uscita.
  58 casi: 23 profili (LZMA2, LZMA, zstd, PPMd, BZip2, Deflate, Deflate64, Copy, BCJ2,
  filtri, `-ms=off/e/<dim>`, `-mqs`, AES, memoria ridotta) con più valori di `-mmt`, un
  folder > 64 MiB tra folder piccoli, file illeggibili, aggiornamento di un archivio
  (aggiunta, sostituzione con ricompressione). Per gli archivi cifrati (IV casuali)
  confronta console ed esito ed estrae l'archivio.
- `tests/unit_7zupdate_mt`: test unitari di `7zUpdateMt` (stima di thread e memoria degli
  encoder per metodo, granularità di `CFolderReplayInStream`).
- `tests/update_growing.{cpp,py}`: file che cambiano dimensione durante la compressione
  (una callback dichiara una dimensione e fornisce dati di un'altra, fino a 140 MiB):
  archivi identici con `-mpfc=off/on`, integri, con la dimensione reale.

Sanitizer (`SEVENZIP_SANITIZE`): `address,undefined` per gli errori di memoria,
`thread` per le race condition. Si usano in una directory di build separata:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEVENZIP_SANITIZE=thread
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

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
