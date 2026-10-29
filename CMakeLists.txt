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

cmake_minimum_required(VERSION 3.20)

# Upstream compila con -O2: togliamo il -O3 implicito di CMake per Release e
# gestiamo il livello di ottimizzazione con SEVENZIP_OPT_LEVEL. Impostato prima di
# project() cosi' un valore passato dall'utente in cache ha la precedenza.
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-g -DNDEBUG" CACHE STRING "")

project(SevenZipModern
  VERSION 26.05
  DESCRIPTION "7-Zip modern fork"
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
option(SEVENZIP_ASM "Usa le routine assembly di Asm/x86 (richiede asmc/uasm/jwasm)" OFF)
option(SEVENZIP_WERROR "Tratta i warning come errori (come upstream)" ON)
option(SEVENZIP_STRIP "Rimuove i simboli dal binario in Release (come upstream: -s)" ON)
set(SEVENZIP_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GEN, USE")
set_property(CACHE SEVENZIP_PGO PROPERTY STRINGS OFF GEN USE)
set(SEVENZIP_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory dei profili PGO")
# Upstream non forza uno standard: GCC 13 usa gnu17 / gnu++17. Lo rendiamo esplicito
# per poter migrare a C++20/23 in modo controllato.
set(SEVENZIP_CXX_STANDARD "17" CACHE STRING "Standard C++ (17, 20, 23)")

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

# Main.cpp: variante "7zz" (equivale a CONSOLE_VARIANT_FLAGS in Alone2/makefile.gcc)
set_source_files_properties(CPP/7zip/UI/Console/Main.cpp PROPERTIES
  COMPILE_DEFINITIONS "Z7_PROG_VARIANT_Z$<$<BOOL:${SEVENZIP_ASM}>:;Z7_7ZIP_ASM>")

# ---------------------------------------------------------------------------
# Eseguibile 7zz
# ---------------------------------------------------------------------------
add_executable(7zz
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS})
target_link_libraries(7zz PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

include(GNUInstallDirs)
install(TARGETS 7zz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Riepilogo configurazione
# ---------------------------------------------------------------------------
message(STATUS "7-Zip modern: build=${CMAKE_BUILD_TYPE} -O${SEVENZIP_OPT_LEVEL} "
               "native=${SEVENZIP_NATIVE} lto=${SEVENZIP_LTO} asm=${SEVENZIP_ASM} "
               "pgo=${SEVENZIP_PGO} c++=${SEVENZIP_CXX_STANDARD}")
