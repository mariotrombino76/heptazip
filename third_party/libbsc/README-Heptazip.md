# libbsc (vendored)

La libreria **libbsc** di Ilya Grebnov (compressione a blocchi: LZP, Burrows-Wheeler con
libsais, coder QLFC), usata dal metodo `BWT` nei `.7z` (`CPP/7zip/Compress/BscCoder.cpp`,
design 0016, issue #81). Il metodo è solo di Heptazip, come FLOAT.

| | |
|---|---|
| Versione | **3.4.0** (commit `1dc676540cb40a41562af6f2db04bd1adce97f90` del 2026-10-29, branch `master`) |
| Fonte | https://github.com/IlyaGrebnov/libbsc |
| Licenza | Apache-2.0 (`LICENSE`; libsais inclusa, Apache-2.0: `libbsc/bwt/libsais/LICENSE`), compatibile con la LGPL di 7-Zip e la BSD-2-Clause del fork |
| Build | `cmake/SevenZipBsc.cmake`, opzione `SEVENZIP_BSC` (ON); makefile GCC e nmake |

Contenuto copiato: `LICENSE`, `AUTHORS`, `VERSION`, `CHANGES` e la cartella `libbsc/`
(sorgenti C/C++), con **una modifica**: in `libbsc/coder/common/rangecoder.h` i puntatori al
flusso compresso (`ari_input`, `ari_output`, `ari_outputEOB`, `ari_outputStart`) sono
`unsigned char *` invece di `unsigned short *`, letti e scritti con `memcpy` di 2 byte
(stesso codice macchina su x86-64): con dati corrotti la posizione può essere impari, e un
puntatore disallineato a un tipo a 16 bit è comportamento indefinito (UBSan; trovato dal
fuzzing del decoder, casi in `tests/fuzz/regress-codec/BWT/`). Le righe modificate sono
marcate `Heptazip`. La libreria è compilata anche con `LIBBSC_NO_UNALIGNED_ACCESS`. Escluso: i sorgenti CUDA (`libbsc/st/st.cu`,
`libbsc/bwt/libcubwt/libcubwt.cu`; restano i loro header `.cuh`, inclusi senza condizione
da `st.cpp` e `bwt.cpp`, e i file di licenza di libcubwt), `bsc.cpp` (il programma a riga
di comando), il `CMakeLists.txt` originale, `README`.

Compilata senza OpenMP (`LIBBSC_OPENMP_SUPPORT` non definito): il parallelismo è per
blocchi indipendenti nel codec, come per brotli (design 0014).

## Aggiornamento

1. Clonare il repository alla nuova release e annotare il commit.
2. Sostituire i file elencati sopra (non vanno modificati) e aggiornare questa tabella e
   gli elenchi di oggetti dei makefile se cambiano i sorgenti.
3. Ricompilare ed eseguire `ctest` (`bwt_codec`, `roundtrip_smoke`).
