# Heptazip (fork di 7-Zip, branch modern) — build e test

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
| `SEVENZIP_ASM` | `ON` se c'è un assembler | usa `Asm/x86/*.asm` (CRC, AES, SHA, Sort, **LzmaDecOpt**; `LzFindOpt` resta in C, più veloce); richiede `asmc`, `uasm` o `jwasm` (cercati anche in `~/.local/bin`); senza assembler la build è in C puro |
| `SEVENZIP_PGO` | `OFF` | `GEN` / `USE`: profile-guided optimization (usare `tools/pgo.sh`) |
| `SEVENZIP_CXX_STANDARD` | `20` | standard C++ (17/20/23); upstream compila in gnu++17, il fork in gnu++20 (verificato: nessun warning con GCC 13 e Clang 20, archivi identici) |
| `SEVENZIP_ZSTD_ENCODER` | `ON` | encoder zstd per `.7z` con `third_party/zstd` (il decoder è sempre presente) |
| `SEVENZIP_BROTLI` | `ON` | metodo brotli nei `.7z` con `third_party/brotli` (compatibile con 7-Zip-zstd) |
| `SEVENZIP_BSC` | `ON` | metodo BWT nei `.7z` con libbsc (`third_party/libbsc`, solo Heptazip) |
| `SEVENZIP_LIBDEFLATE` | `ON` | compressore Deflate di `third_party/libdeflate` per gzip, zip e 7z (senza: l'encoder di 7-Zip) |
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

### ARM64 (aarch64)

Il codice compila anche per Linux ARM64 (`-Werror`), con il SIMD NEON nel decoder FLOAT.
Build incrociata con i test eseguiti da qemu-user (`cmake/toolchains/aarch64-linux-gnu.cmake`):

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j"$(nproc)"
ctest --test-dir build-arm64 -j8     # tutti i test sotto qemu (~13 minuti)
```

Nei test CTest i programmi ARM64 sono lanciati attraverso l'emulatore, anche quelli
avviati dagli script Python (`tests/CMakeLists.txt`, `_sz_program`). Senza root si
possono estrarre i pacchetti `.deb` con `apt-get download` e `dpkg -x` e passare i
compilatori con `-DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...` (con `-static`, per
non dipendere dalle librerie ARM64 in `/usr/aarch64-linux-gnu`) e qemu con
`-DSEVENZIP_QEMU_AARCH64=...`.

### Assembly x86-64 (consigliato)

Con un assembler disponibile `SEVENZIP_ASM` è attivo di default: decompressione LZMA
**1,4 volte più veloce** (`LzmaDecOpt.asm`; Silesia `-mx5`: 169 → 239 MB/s, `-mx1`
649 → 985 MB/s), output identico. Il match finder resta in C (`LzFindOpt.c` con
`LzFind_MatchEnd()`), più veloce della versione assembly di upstream (design 0004).
Serve un assembler MASM-compatibile. UASM **v2.57r** compila su Linux, mentre
`master` non compila (include `<direct.h>`):

```sh
git clone https://github.com/Terraspace/UASM.git ~/.local/src/UASM
cd ~/.local/src/UASM && git checkout v2.57r
make -f Makefile-Linux-GCC-64.mak -j"$(nproc)" CFLAGS=-fcommon
cp GccUnixR/uasm ~/.local/bin/

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # trova ~/.local/bin/uasm da solo
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
così). Risultati: da 1,15× a 2,9× sugli archivi con più folder (design 0001, §10). È un'opzione
degli handler 7z e zip (sotto): come `-mtf`, non è accettata dagli altri formati (xz…).

## Estrazione parallela degli zip (`-mpfd`)

```sh
7zz x archivio.zip             # attiva di default: file diversi decodificati in parallelo
7zz x -mpfd=off archivio.zip   # estrazione sequenziale (comportamento upstream)
```

I file di uno zip sono flussi indipendenti: i worker ne leggono i byte compressi sotto
lock dello stream dell'archivio e li decodificano in memoria, il thread chiamante
riproduce in ordine le stesse callback dell'estrazione sequenziale (stream, risultato per
file, esito), così quello che l'utente o il gestore di archivi osserva è identico
(`docs/design/0017-estrazione-zip-parallela.md`). Usa al massimo `-mmt` thread e il budget
di memoria `-mmemuse`. Restano sequenziali, con lo stesso codice di prima: gli archivi
multivolume, i file oltre 64 MiB, i metodi la cui memoria non si conosce dagli header
(LZMA, PPMd, zstd, xz), i file grandi non cifrati memorizzati o quasi (si decodificano
alla velocità di una copia). Risultati (i7-12700H, 8 thread): silesia Deflate 0,25 → 0,13 s
in test e 0,29 → 0,17 in estrazione; corpus AES-256 0,45 → 0,13 s (0,11 con 20 thread);
1200 file piccoli 0,021 → 0,010 s. ZipCrypto è un cifrario seriale a byte (~156 MB/s): il
file più grande fa da limite (corpus 0,53 → 0,38 s).

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
massimo `-mmt` thread e il limite di memoria di compressione (`-mmemuse`, di default la
metà della RAM per i thread automatici); i folder oltre
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

## Brotli nei `.7z` (`-m0=brotli`)

```sh
7zz a -m0=brotli -mx5 archivio.7z dati/     # equilibrato
7zz a -m0=brotli -mx11 archivio.7z dati/    # massimo rapporto (lento in compressione)
```

Metodo `0x4F71102`, nel formato di 7-Zip-zstd (frame indipendenti, compressi e
decompressi in parallelo). Gli archivi non si aprono con 7-Zip upstream. Dettagli e
misure: [docs/design/0014-brotli-nel-7z.md](docs/design/0014-brotli-nel-7z.md).

## Testi e dati strutturati: BWT (`-m0=bwt`)

```sh
7zz a -m0=bwt archivio.7z testi/            # livello 5: blocchi da 16 MiB, coder QLFC adattivo
7zz a -m0=bwt -mx3 archivio.7z testi/       # blocchi da 8 MiB, coder statico: più veloce
7zz a -m0=bwt:c=64m:a=1 archivio.7z testi/  # blocco e coder dati (c=64 KiB..1 GiB; a=0 veloce, 1 statico, 2 adattivo)
```

Burrows-Wheeler (libsais) con LZP e coder QLFC della libreria libbsc (`third_party/libbsc`,
Apache-2.0), in blocchi indipendenti compressi e decompressi in parallelo. Su Silesia a 20
thread 22,41% a 72 MB/s (LZMA2 `-mx9`: 22,97% a 7,6 MB/s), sui testi molto di più
(webster 16% contro 20%); sui binari resta meglio LZMA2. Metodo `0x3F707F5E728B0002`, solo
Heptazip: gli archivi non si aprono con 7-Zip. Memoria ~7 volte il blocco per thread,
limitata a metà della RAM. Dettagli e misure:
[docs/design/0016-metodo-bwt-libbsc.md](docs/design/0016-metodo-bwt-libbsc.md).

## Pesi dei modelli AI (`-m0=FLOAT`)

```sh
7zz a modello.7z model.safetensors config.json             # automatico: BF16 ~66,3% (LZMA2 -mx5: 70,2%)
7zz a -m0=FLOAT:c=4m modello.7z modello-f16.gguf           # blocchi da 64k a 4m (default 1m)
7zz a modello.7z modello-Q4_K_M.gguf                       # automatico: ~93,5% (LZMA2: 97,8%), 15x piu' veloce
```

Senza perdita, per array di float (BF16, FP16, FP32): l'esponente di ogni peso è
codificato con Huffman, segno e mantissa sono copiati. L'encoder sceglie tipo e
allineamento per ogni blocco; i blocchi che non sono float (maschere, testo) vanno in
zstd o restano grezzi. Nei GGUF quantizzati (Q2_K, Q3_K, Q4_0, Q8_0, Q4_K, Q5_K, Q6_K)
ogni tipo di campo delle strutture (scale, valori a 2-8 bit) ha il suo codice rANS con
un modello a contesti (il valore dato il punto zero o la scala del suo sotto-blocco).
**Automatico** per i file `.safetensors` e `.gguf` in F16/BF16/F32 e nei tipi
quantizzati qui sopra quando il metodo è quello di default (`-mf=off` lo disattiva; con
`-m0=zstd` e altri metodi espliciti non cambia nulla). Metodo `3F707F5E728B0001`, solo in
questo fork. Formato e misure: `docs/design/0008-metodo-float.md`.

## Immagini ISO (`-tiso`)

```sh
7zz a -tiso disco.iso cartella/                 # ISO 9660 + Joliet + Rock Ridge
7zz a -tiso -mvol="DATI 2026" -snl disco.iso cartella/   # etichetta; -snl conserva i link
7zz a -tiso -mjoliet=off disco.iso cartella/    # solo 9660 + Rock Ridge
7zz a -tiso -mrr=off disco.iso cartella/        # solo 9660 + Joliet
7zz a disco.iso altro.txt                       # aggiunge riscrivendo l'immagine
```

Immagine ISO 9660 con l'albero Joliet (nomi Unicode fino a 103 caratteri, Windows) e
l'albero Rock Ridge (nomi interi in UTF-8, permessi, mtime, link simbolici: Linux, macOS);
file oltre 4 GiB con estensioni multiple; nessuna compressione, nessun thread. `-mvol`
accetta lettere maiuscole, cifre, `_` e spazio (32 caratteri al massimo; `ISOIMAGE` se
assente; nella riscrittura resta quella dell'immagine). Il dialogo Aggiungi della GUI ha
`iso` tra i formati. Non si scrivono El Torito (avviabili) e UDF. Design: `docs/design/0019`.

## Output JSON dell'elenco (`l -json`)

```sh
7zz l -json archivio.7z | jq '.archives[0].items[] | {path, size}'
```

Un solo documento JSON su stdout, con gli stessi campi di `-slt` (nomi in camelCase, date
ISO 8601 UTC, dimensioni numeriche). Contratto completo: `docs/json-output.md`.

## Windows: GUI (7zFM, 7zG) con Visual Studio

Il dialogo "Aggiungi all'archivio" conosce i metodi del fork per il 7z: **ZSTD**, **BROTLI**,
**BWT** e **FLOAT** compaiono nella tendina Metodo con il dizionario (finestra di zstd,
blocco del BWT) e i thread del livello, e con la memoria stimata come per LZMA2. I profili
(Più veloce = zstd, Estrazione veloce = brotli, Testi = BWT, Bilanciato e Migliore = LZMA2,
Compatibile = zip) scelgono formato, livello e metodo nelle tendine; con un `0=metodo` nel
campo Parametri le tendine del metodo restano disabilitate.


```bat
tools\windows\build-msvc.bat out-msvc
rem -> 7z.dll, 7z.exe, 7zFM.exe, 7zG.exe, 7-zip.dll in out-msvc\
```

I file si usano senza installazione (stessa cartella). La CI li pubblica come artefatto
`heptazip-windows-x64-msvc`. 7zFM e 7zG richiedono Windows 10 1903 o successivo: i loro
makefile compilano con `Z7_WIN32_WINNT_MIN=0x0a00` e collegano `uxtheme` e `dwmapi` in modo
statico (`DarkMode.cpp` cerca a runtime solo le funzioni del tema scuro esportate per
ordinale); i manifest dichiarano solo Windows 10/11; su una versione più vecchia la GUI
avvisa all'avvio e parte con il tema chiaro. `7z.exe`, `7z.dll`, `7-zip.dll` e `7zz.exe`
mantengono il minimo di 7-Zip. Heptazip convive con 7-Zip installato: impostazioni in
`HKCU\Software\Heptazip`, codec cercati solo nella propria cartella (non in quella di
7-Zip), associazioni dei file con il prefisso `Heptazip.`, estensione di Explorer con un
CLSID proprio (`{90642948-CE43-44BE-810E-2CA2A1F77CEE}`). I file di traduzione di 7-Zip
(`Lang\*.txt`) restano usabili.

## Windows: 7zz.exe con MinGW-w64

La versione da riga di comando per Windows si compila da Linux con i makefile di 7-Zip
(che ora contengono anche FLOAT, l'encoder zstd e libdeflate):

```sh
sudo apt install g++-mingw-w64-x86-64-posix
cd CPP/7zip/Bundles/Alone2
make -f makefile.gcc -j"$(nproc)" O=_mingw IS_MINGW=1 \
  CC=x86_64-w64-mingw32-gcc-posix CXX=x86_64-w64-mingw32-g++-posix \
  RC=x86_64-w64-mingw32-windres
# -> _mingw/7zz.exe (statico, nessuna DLL)
```

Lo stesso makefile senza `IS_MINGW` compila `7zz` per Linux. La CI compila entrambi, e
prova `7zz.exe` su un runner Windows (`tests/roundtrip_smoke.py`, `tests/deflate_mt.py`);
l'eseguibile è tra gli artefatti dell'esecuzione (`7zz-windows-x64-mingw`).

## Gestori di archivi di Linux (Ark, File Roller)

Heptazip non ha una GUI per Linux, ma i gestori di archivi dei desktop eseguono 7-Zip e
ne leggono l'output, quindi usano il motore di Heptazip se lo trovano nel PATH:

```sh
sudo install -m 755 build/7zz /usr/local/bin/7zz      # File Roller (GNOME) usa 7zz
sudo ln -sf /usr/local/bin/7zz /usr/local/bin/7z      # Ark (KDE) esegue 7z
```

La prima riga del banner resta quella di 7-Zip (`7-Zip (z) 26.05 ...`): Ark legge un
elenco solo dopo questa riga. Il test `gui_compat` legge l'output di `7zz` con le regole
di Ark e di File Roller (7z, zip, tar, gzip).

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

- `tests/zip_extract_equivalence.py`: zip con Deflate ai livelli, Store, Deflate64, BZip2,
  LZMA, PPMd, ZipCrypto, AES-256, da stdin (descrittori) e 150 file piccoli, più copie
  corrotte (byte invertiti, tagli) di ciascuno: 272 casi in cui la traccia delle callback
  di `extract_trace -z` con `-mmt=8` (e `-mpfd=off` sugli archivi integri) deve essere
  identica a `-mmt=1`, anche con selezioni parziali, modalità test e password sbagliata.
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
- `fuzz_regress`: riesegue gli input che il fuzzing aveva trasformato in crash, leak o
  allocazioni enormi (vedi sotto).

Sanitizer (`SEVENZIP_SANITIZE`): `address,undefined` per gli errori di memoria,
`thread` per le race condition. Si usano in una directory di build separata:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEVENZIP_SANITIZE=thread
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

## Fuzzing (design 0007)

Harness libFuzzer per i gestori di tutti i formati (`tests/fuzz/fuzz_archive.cpp`):
apre l'input, legge le proprietà degli elementi e li estrae in modalità test. Richiede
Clang e una directory di build dedicata:

```sh
CC=clang-20 CXX=clang++-20 cmake -S . -B build-fuzz -DSEVENZIP_FUZZ=ON -DSEVENZIP_ASM=OFF \
  -DSEVENZIP_SANITIZE=address,undefined,fuzzer-no-link -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz -j --target fuzz_archive
tools/fuzz/make-seeds.sh build/7zz ~/devtools/fuzz/seeds          # archivi semi
tools/fuzz/run-campaign.sh build-fuzz/tests/fuzz_archive ~/devtools/fuzz 900   # 15 min per formato
FUZZ_FORMAT=wim build-fuzz/tests/fuzz_archive <input>             # riproduce un artefatto
```

Ogni input che ha trovato un bug va in `tests/fuzz/regress/<formato>/`: il test CTest
`fuzz_regress` (programma `fuzz_replay`, lo stesso harness senza libFuzzer) li riesegue
in ogni build; con ASan le allocazioni oltre 1 GiB sono errori.

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
   `compare-archives.sh` contro la build **precedente del fork** deve dare `IDENTICI`.
   Contro 7-Zip upstream i profili LZMA2 dal livello 5 su input grandi differiscono per
   scelta (dimensione parola 64, `C/LzmaEnc.c`; blocchi LZMA2 pari al dizionario del
   livello e, al livello 5, dizionario di 16 MiB con blocchi da 32 MiB, `C/Lzma2Enc.h`):
   il formato è lo stesso e gli archivi si aprono con upstream.
3. Se la modifica riguarda le prestazioni, allegare l'output di `bench.py`
   (minimo 3 ripetizioni) con il confronto rispetto a upstream.

## Rilascio

La versione è `HZ_VERSION` in `C/7zVersion.h`: base 7-Zip più `-m<n>` (es. `26.05-m1`).
Per rilasciare:

1. Scrivere `docs/release-notes/<versione>.md` (italiano e inglese) e unirlo in `develop`.
2. Pull request da `develop` a `modern` (la linea stabile) e merge a CI verde.
3. Tag sul merge: `git tag 26.05-m1 modern && git push heptazip 26.05-m1`.
4. Il workflow **Release** (`.github/workflows/release.yml`) compila `7zz` Linux x64 statico
   con le routine assembly (uasm v2.57r compilato nel runner), il pacchetto Windows x64
   (MSVC: `7zFM`, `7zG`, `7z.exe`, `7z.dll`, `7-zip.dll`; MinGW: `7zz.exe`), esegue i test di
   andata e ritorno su ogni binario, calcola `SHA256SUMS` e crea una **bozza** di release
   con le note del punto 1.
5. Scaricare i pacchetti dalla bozza, provarli, poi pubblicarla
   (`gh release edit <versione> --draft=false`) e aggiornare la pagina Installazione del wiki.
