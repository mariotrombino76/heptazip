# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
# 7-Zip "modern" fork - build CMake per Linux (GCC/Clang).
#
# Affianca (non sostituisce) i makefile originali: CPP/7zip/Bundles/*/makefile.gcc
# restano utilizzabili e servono come riferimento per verificare che la build
# CMake produca archivi identici byte per byte (vedi tools/bench/compare-archives.sh).
#
# Uso tipico:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Opzioni principali (tutte OFF/neutre di default = stesso comportamento upstream):
#   SEVENZIP_OPT_LEVEL   livello -O per Release/RelWithDebInfo (default 2, come upstream)
#   SEVENZIP_NATIVE      -march=native (binario NON portabile su altre CPU)
#   SEVENZIP_LTO         link-time optimization
#   SEVENZIP_ASM         usa Asm/x86/*.asm (richiede asmc, uasm o jwasm)
#   SEVENZIP_PGO         OFF | GEN | USE  (profile-guided optimization, vedi tools/pgo.sh)
#   SEVENZIP_SANITIZE    "" | address | thread | undefined | address,undefined
#   SEVENZIP_BUILD_TESTS programmi di test e suite CTest (ctest --test-dir build)

cmake_minimum_required(VERSION 3.20)

# Upstream compila con -O2: togliamo il -O3 implicito di CMake per Release e
# gestiamo il livello di ottimizzazione con SEVENZIP_OPT_LEVEL. Impostato prima di
# project() cosi' un valore passato dall'utente in cache ha la precedenza.
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-g -DNDEBUG" CACHE STRING "")

project(Heptazip
  VERSION 26.05
  DESCRIPTION "Heptazip, fork of 7-Zip 26.05"
  LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(WARNING "Questa build CMake e' testata solo su Linux.")
endif()

# ---------------------------------------------------------------------------
# Opzioni
# ---------------------------------------------------------------------------
set(SEVENZIP_OPT_LEVEL "2" CACHE STRING "Livello di ottimizzazione (-O<n>) per build non-Debug")
set_property(CACHE SEVENZIP_OPT_LEVEL PROPERTY STRINGS 1 2 3 s fast)
option(SEVENZIP_NATIVE "Compila con -march=native" OFF)
option(SEVENZIP_LTO "Abilita link-time optimization" OFF)
# Assembly: attivo di default se la CPU e' x86-64 e c'e' un assembler MASM-compatibile
# (asmc, uasm, jwasm, anche in ~/.local/bin). Decompressione LZMA ~1,4 volte piu' veloce
# (LzmaDecOpt.asm): vedi docs/benchmark-mercato.md. Senza assembler: C puro, nessun errore.
set(_asm_default OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  find_program(SEVENZIP_ASSEMBLER NAMES asmc uasm jwasm HINTS "$ENV{HOME}/.local/bin"
    DOC "Assembler MASM-compatibile per Asm/x86")
  if(SEVENZIP_ASSEMBLER)
    set(_asm_default ON)
  endif()
endif()
option(SEVENZIP_ASM "Usa le routine assembly di Asm/x86 (richiede asmc/uasm/jwasm)" ${_asm_default})
if(NOT SEVENZIP_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT SEVENZIP_ASSEMBLER)
  message(STATUS "Nessun assembler (asmc/uasm/jwasm): build in C puro, decompressione LZMA piu' lenta")
endif()
option(SEVENZIP_WERROR "Tratta i warning come errori (come upstream)" ON)
# Fuzzing (design 0007): harness libFuzzer in tests/fuzz. Richiede Clang e
# SEVENZIP_SANITIZE con fuzzer-no-link, es.:
#   CC=clang-20 CXX=clang++-20 cmake -B build-fuzz -DSEVENZIP_FUZZ=ON \
#     -DSEVENZIP_SANITIZE=address,undefined,fuzzer-no-link -DCMAKE_BUILD_TYPE=RelWithDebInfo
option(SEVENZIP_FUZZ "Compila gli harness libFuzzer (tests/fuzz)" OFF)
option(SEVENZIP_STRIP "Rimuove i simboli dal binario in Release (come upstream: -s)" ON)
set(SEVENZIP_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GEN, USE")
set_property(CACHE SEVENZIP_PGO PROPERTY STRINGS OFF GEN USE)
set(SEVENZIP_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory dei profili PGO")
# Upstream non forza uno standard (GCC 13 usa gnu++17). Il fork usa C++20 (gnu++20):
# verificato senza warning con GCC 13 e Clang 20, archivi identici a upstream.
# Il codice nuovo puo' usare C++20 (es. std::span); per tornare a C++17: -DSEVENZIP_CXX_STANDARD=17
set(SEVENZIP_CXX_STANDARD "20" CACHE STRING "Standard C++ (17, 20, 23)")
set(SEVENZIP_SANITIZE "" CACHE STRING
  "Sanitizer: vuoto, address, thread, undefined o lista separata da virgole (es. address,undefined)")
option(SEVENZIP_SANITIZE_RECOVER
  "UBSan: continua dopo un errore invece di terminare (per fare l'inventario delle segnalazioni)" OFF)
option(SEVENZIP_ZSTD_ENCODER
  "Encoder Zstandard per .7z con la libreria inclusa in third_party/zstd (il decoder e' sempre presente)" ON)
option(SEVENZIP_LIBDEFLATE
  "Compressore Deflate di libdeflate (third_party/libdeflate) per gzip, zip e 7z" ON)
option(SEVENZIP_BUILD_TESTS "Compila i programmi di test e registra la suite CTest" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD ${SEVENZIP_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # upstream usa -fPIC
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)     # per clangd / clang-tidy

find_package(Threads REQUIRED)

include(cmake/SevenZipSources.cmake)

# ---------------------------------------------------------------------------
# Flag comuni (interface target, ereditato da tutte le librerie)
# ---------------------------------------------------------------------------
add_library(sevenzip_flags INTERFACE)

target_compile_definitions(sevenzip_flags INTERFACE
  _REENTRANT
  _FILE_OFFSET_BITS=64
  _LARGEFILE_SOURCE)

target_compile_options(sevenzip_flags INTERFACE
  $<$<NOT:$<CONFIG:Debug>>:-O${SEVENZIP_OPT_LEVEL}>
  $<$<COMPILE_LANGUAGE:C,CXX>:-Wall -Wextra>
  $<$<AND:$<BOOL:${SEVENZIP_WERROR}>,$<COMPILE_LANGUAGE:C,CXX>>:-Werror>)

# Alcuni warning di GCC dipendono dall'inlining/dai profili: con PGO/LTO/-O3
# producono falsi positivi nel codice upstream. Restano warning visibili, ma
# non bloccano la build:
#   maybe-uninitialized : ChmHandler.cpp (compressedSize)
#   stringop-overflow   : Bcj2Enc.c:491 (temp[8], scrittura limitata a rem <= 4)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sevenzip_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error=maybe-uninitialized -Wno-error=stringop-overflow>)
endif()
# GCC 16: -Warray-bounds su chiamate virtuali devirtualizzate in modo speculativo
# verso il tipo sbagliato, in percorsi mai eseguiti (falsi positivi):
#   HandlerCont.cpp:281 (CLocalProgress::Release su un CHandlerImgProgress)
#   RarHandler.cpp:1672 (CFilterCoder::AddRef su un CCopyCoder)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  target_compile_options(sevenzip_flags INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=array-bounds>)
endif()

if(SEVENZIP_NATIVE)
  target_compile_options(sevenzip_flags INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:-march=native>)
endif()

if(SEVENZIP_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES C CXX)
  if(NOT _ipo_ok)
    message(FATAL_ERROR "LTO non supportato dal toolchain: ${_ipo_msg}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

if(SEVENZIP_PGO STREQUAL "GEN")
  # prefer-atomic: 7-Zip e' multi-thread, contatori non atomici darebbero profili corrotti
  target_compile_options(sevenzip_flags INTERFACE -fprofile-generate=${SEVENZIP_PGO_DIR} -fprofile-update=prefer-atomic)
  target_link_options(sevenzip_flags INTERFACE -fprofile-generate=${SEVENZIP_PGO_DIR})
elseif(SEVENZIP_PGO STREQUAL "USE")
  if(NOT EXISTS "${SEVENZIP_PGO_DIR}")
    message(FATAL_ERROR "SEVENZIP_PGO=USE ma ${SEVENZIP_PGO_DIR} non esiste: esegui prima tools/pgo.sh")
  endif()
  # -fprofile-partial-training: le funzioni non esercitate dal training non
  # vengono ottimizzate "per dimensione" (importante per i codec poco usati).
  set(_pgo_use -fprofile-use=${SEVENZIP_PGO_DIR} -fprofile-correction)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND _pgo_use -fprofile-partial-training -Wno-missing-profile)
  endif()
  target_compile_options(sevenzip_flags INTERFACE ${_pgo_use})
  target_link_options(sevenzip_flags INTERFACE ${_pgo_use})
elseif(NOT SEVENZIP_PGO STREQUAL "OFF")
  message(FATAL_ERROR "SEVENZIP_PGO deve essere OFF, GEN o USE")
endif()

# Sanitizer: per i test (TSan per il codice multi-thread, ASan+UBSan per la memoria).
# -fno-omit-frame-pointer rende leggibili gli stack trace; i simboli non vanno rimossi.
if(SEVENZIP_SANITIZE)
  if(SEVENZIP_SANITIZE MATCHES "thread" AND SEVENZIP_SANITIZE MATCHES "address")
    message(FATAL_ERROR "ThreadSanitizer e AddressSanitizer non possono essere combinati")
  endif()
  set(_san -fsanitize=${SEVENZIP_SANITIZE} -fno-omit-frame-pointer)
  if(SEVENZIP_SANITIZE MATCHES "undefined")
    if(NOT SEVENZIP_SANITIZE_RECOVER)
      list(APPEND _san -fno-sanitize-recover=undefined)
    endif()
  endif()
  target_compile_options(sevenzip_flags INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:${_san}>)
  target_link_options(sevenzip_flags INTERFACE ${_san})
  set(SEVENZIP_STRIP OFF CACHE BOOL "" FORCE)
endif()

target_link_options(sevenzip_flags INTERFACE
  -Wl,-z,noexecstack
  $<$<AND:$<BOOL:${SEVENZIP_STRIP}>,$<CONFIG:Release>>:-s>)

# ---------------------------------------------------------------------------
# Assembly opzionale (modifica SEVENZIP_C_SOURCES e definisce SEVENZIP_ASM_OBJECTS)
# ---------------------------------------------------------------------------
set(SEVENZIP_ASM_OBJECTS "")
if(SEVENZIP_ASM)
  include(cmake/SevenZipAsm.cmake)
endif()

# Encoder Zstandard opzionale (modifica SEVENZIP_CPP_SOURCES, definisce SEVENZIP_ZSTD_OBJECTS)
include(cmake/SevenZipZstd.cmake)

# Compressore Deflate di libdeflate opzionale (definisce SEVENZIP_LIBDEFLATE_OBJECTS)
include(cmake/SevenZipLibdeflate.cmake)

# ---------------------------------------------------------------------------
# Librerie
#
# OBJECT library e non STATIC: i codec e i formati si registrano tramite
# costruttori statici (REGISTER_CODEC / REGISTER_ARC nei file *Register.cpp,
# *Handler.cpp). Con una libreria statica il linker scarterebbe gli oggetti non
# referenziati e i codec sparirebbero silenziosamente dal binario.
# ---------------------------------------------------------------------------
add_library(sevenzip_c OBJECT ${SEVENZIP_C_SOURCES})
target_link_libraries(sevenzip_c PRIVATE sevenzip_flags)

add_library(sevenzip_cpp OBJECT ${SEVENZIP_CPP_SOURCES})
target_link_libraries(sevenzip_cpp PRIVATE sevenzip_flags)

# UI console (main()): separata da sevenzip_cpp per poterla escludere nei test
add_library(sevenzip_console OBJECT ${SEVENZIP_CONSOLE_SOURCES})
target_link_libraries(sevenzip_console PRIVATE sevenzip_flags)

# Main.cpp: variante "7zz" (equivale a CONSOLE_VARIANT_FLAGS in Alone2/makefile.gcc)
set_source_files_properties(CPP/7zip/UI/Console/Main.cpp PROPERTIES
  COMPILE_DEFINITIONS "Z7_PROG_VARIANT_Z$<$<BOOL:${SEVENZIP_ASM}>:;Z7_7ZIP_ASM>")

# ---------------------------------------------------------------------------
# Eseguibile 7zz
# ---------------------------------------------------------------------------
add_executable(7zz
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  $<TARGET_OBJECTS:sevenzip_console>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS} ${SEVENZIP_LIBDEFLATE_OBJECTS})
target_link_libraries(7zz PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET sevenzip_asm)
  add_dependencies(7zz sevenzip_asm)
endif()

include(GNUInstallDirs)
install(TARGETS 7zz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------
if(SEVENZIP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Riepilogo configurazione
# ---------------------------------------------------------------------------
message(STATUS "Heptazip: build=${CMAKE_BUILD_TYPE} -O${SEVENZIP_OPT_LEVEL} "
               "native=${SEVENZIP_NATIVE} lto=${SEVENZIP_LTO} asm=${SEVENZIP_ASM} "
               "pgo=${SEVENZIP_PGO} c++=${SEVENZIP_CXX_STANDARD} "
               "sanitize=${SEVENZIP_SANITIZE} tests=${SEVENZIP_BUILD_TESTS} "
               "zstd-encoder=${SEVENZIP_ZSTD_ENCODER}")
