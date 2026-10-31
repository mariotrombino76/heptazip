# Differenze dal codice upstream

Registro delle modifiche del fork ai file **esistenti** di 7-Zip (il codice di
`github.com/ip7z/7zip`, copiato senza modifiche nel branch `main`). Il fork segue una
strada propria: le release upstream **non** vengono riportate in `modern`, quindi i file
originali si modificano liberamente quando serve. Il registro serve a:

- sapere in che cosa il fork diverge da 7-Zip, per esempio quando si confrontano
  prestazioni o archivi con il binario upstream;
- trovare le correzioni generali (es. comportamento indefinito) da proporre a upstream.

I file nuovi del fork (`CMakeLists.txt`, `cmake/`, `tools/`, `tests/`, `docs/`, moduli
nuovi come `7zExtractMt.*`, `ZstdLibEncoder.*`, `ZstdCodecRegister.cpp`) non sono elencati
qui. Il codice esterno incluso è in `third_party/` (provenienza e versione nel README di
ciascuna libreria). 
Nel codice ogni modifica è marcata con un commento che inizia con `7zip-modern:`
(`grep -rn "7zip-modern:" C CPP`).

| File | Modifica | Motivo | Effetto sull'output / sulle prestazioni |
|---|---|---|---|
| `CPP/7zip/Archive/7z/7zExtract.cpp` (`CHandler::Extract`) | Raggruppamento degli elementi per folder → `GetExtractJob()`; esito di `Decode` → `ProcessFolderDecodeResult()`; corpo del ciclo → `InitJobOutput()` + `DecodeJobInline()` con `CExtractContext`. Nuova sezione `#if !defined(Z7_ST)` con l'estrazione parallela (`CParallelExtract`), attivata da `-mpfd` | Estrazione parallela dei folder (design 0001) | Con `pfd=off` nessuno (tracce identiche alla build upstream su 175 casi); con `pfd=on` tracce identiche al sequenziale (suite CTest) |
| `CPP/7zip/Archive/7z/7zHandler.h/.cpp`, `7zHandlerOut.cpp` | Campo `_parallelFolderDecoding` e proprietà `pfd` (stesso schema di `_useMultiThreadMixer` / `mtf`), default `k_ParallelFolderDecoding_Default` | Opzione `-mpfd` | Nessuno con il default |
| Makefile upstream (16 file: `7zip_gcc.mak`, `Arc_gcc.mak`, `Arc.mak`, `Archive/7z/makefile`, `Bundles/*/makefile[.gcc]`) | Aggiunto l'oggetto `7zExtractMt` accanto a `7zExtract` (e la regola in `7zip_gcc.mak`) | Nuovo modulo del fork per l'estrazione parallela (design 0001); le build originali restano equivalenti a CMake | Nessuno sull'output |
| Makefile upstream (`7zip_gcc.mak`, `Arc_gcc.mak`, `Arc.mak`, `Bundles/Alone/makefile[.gcc]`) | Aggiunto `ZstdCodecRegister` accanto a `ZstdDecoder` | Decoder zstd registrato per `.7z` (design 0002) | `7zz i` elenca `4F71101 ZSTD`; nessun effetto sugli altri formati |
| `C/7zCrc.c` (`CrcUpdate`, `CrcGenerateTable`, `z7_GetFunc_CrcUpdate`) | Su x86-64 `CrcUpdate` usa il folding PCLMULQDQ (`C/7zCrcClmul.h`, header di implementazione con funzioni `static` incluso solo qui) se la CPU lo supporta; la versione a tabelle diventa `CrcUpdate_Base`. Algoritmo `128` nel selettore del benchmark (`-mm=crc32:128`); `12` resta la versione a tabelle | CRC32 da 5 a 11 GB/s su buffer grandi (molto di più in cache); nessun file di build da modificare | Nessuno sull'output: archivi identici a upstream (i CRC sono salvati negli archivi); `tests/unit_crc_clmul` confronta con un CRC di riferimento indipendente su tutte le lunghezze 0..4096. Disattivabile con `-DZ7_CRC_CLMUL_DISABLE` |
| `CPP/7zip/UI/Common/ArchiveCommandLine.{h,cpp}`, `UI/Console/Main.cpp`, `UI/Console/List.cpp` | Switch `-json` (in coda alla tabella degli switch), campo `JsonOutput`, chiamata a `ListArchives_Json()`; in `List.cpp` la funzione di accesso `List_GetPropName()` ai nomi di `-slt`. Makefile della console (`Console.mak`, `UI/Console/makefile.gcc`, `Bundles/Alone*/makefile.gcc`, `7zip_gcc.mak`): oggetto `ListJson` | Output JSON di `l` (`docs/json-output.md`) | Nessuno senza `-json` |
| `C/CpuArch.h` (`GetUi16/32/64`, `SetUi16/32/64`, `GetBe*`, `SetBe*` e varianti `…a`) | Le macro leggono e scrivono tramite le funzioni inline `Z7_LoadNative16/32/64` e `Z7_StoreNative16/32/64`, cioè con `memcpy` a dimensione fissa, invece di dereferenziare un puntatore convertito (`*(const UInt32 *)(const void *)p`) | Un accesso non allineato o un accesso a un buffer di `Byte` tramite un puntatore a `UInt32` è comportamento indefinito (allineamento e strict aliasing). Ora UBSan gira con il controllo `alignment` attivo | Archivi identici. GCC 13: codice macchina identico in 302 oggetti su 348; negli altri cambiano solo ordine delle istruzioni e allocazione dei registri. Nessuna differenza di velocità misurabile (SHA3 isolato, MD5, decompressione zstd, LZMA e CRC hanno codice identico) |
| `C/ZstdDec.c` (`COPY_CHUNK_4_2`, `COPY_CHUNK` a 64 bit, riempimento con `offset < 2` in `CopyMatch`) | Copie non allineate tramite `Z7_LoadNative*` / `Z7_StoreNative*` | Accessi a 64 bit non allineati tramite cast segnalati da UBSan (`alignment`) | Decompressione zstd identica in velocità (0,170 s contro 0,171 s su Silesia) |
| `CPP/7zip/Archive/Wim/WimIn.cpp` (`CheckName_and_Fix_in_Meta`, `ParseDirItem`), `WimHandlerOut.cpp` (`WriteItem`) | Puntatori dichiarati `Byte *` invece di `const Byte *` dove il codice scrive nel buffer; letture e scritture a 16 bit tramite `GetUi16a` / `SetUi16a` | Il codice scriveva attraverso puntatori `const`; il cast `(void *)` nelle vecchie macro nascondeva l'errore, emerso compilando le nuove funzioni con tipi corretti | Nessuno |
| `C/LzFindOpt.c` (`GetMatchesSpecN_2`, calcolo di `src`) | `(...) << 1` → `(...) * 2` | Shift a sinistra di un valore negativo: comportamento indefinito in C, segnalato da UBSan (`-fsanitize=shift-base`) | Nessuno: stesso codice macchina; archivi identici (`compare-archives.sh`) |

## Comportamenti upstream noti, non modificati

| Dove | Cosa | Perché non è modificato |
|---|---|---|
| `C/LzFindMt.c` (`stopWriting` in `MtSync_StopWriting` / `BtThreadFunc` / `HashThreadFunc`) | Flag `BoolInt` non atomico scritto da un thread e letto da un altro. La lettura che conta è ordinata dai semafori; TSan segnala quella dell'iterazione precedente (race formale C11, benigna su x86). Solo encoder multi-thread | Codice dell'encoder, fuori dall'ambito della fase 2. Soppresso nei test con `tests/tsan.supp`. Correzione possibile: flag atomico C11 (da proporre a upstream) |
