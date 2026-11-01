# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
# Encoder Zstandard (metodo 7z 0x4F71101) con la libreria di riferimento inclusa in
# third_party/zstd (vedi third_party/zstd/README-Tensor7.md).
#
# Il decoder e' sempre quello di 7-Zip (C/ZstdDec.c, registrato da
# CPP/7zip/Compress/ZstdCodecRegister.cpp): l'estrazione non dipende da questa opzione.
#
# Input:  SEVENZIP_ZSTD_ENCODER (ON/OFF)
# Output: target OBJECT "sevenzip_zstd", variabile SEVENZIP_ZSTD_OBJECTS,
#         sorgente ZstdLibEncoder.cpp aggiunto a SEVENZIP_CPP_SOURCES,
#         definizione Z7_ZSTD_LIB_ENCODER sui sorgenti che la usano.

set(SEVENZIP_ZSTD_OBJECTS "")

if(NOT SEVENZIP_ZSTD_ENCODER)
  return()
endif()

set(_zstd_dir "${CMAKE_SOURCE_DIR}/third_party/zstd/lib")
if(NOT EXISTS "${_zstd_dir}/zstd.h")
  message(FATAL_ERROR "SEVENZIP_ZSTD_ENCODER=ON ma ${_zstd_dir}/zstd.h non esiste")
endif()

# Solo common/ e compress/ (la decompressione usa il decoder di 7-Zip)
file(GLOB _zstd_sources CONFIGURE_DEPENDS
  "${_zstd_dir}/common/*.c"
  "${_zstd_dir}/compress/*.c")

add_library(sevenzip_zstd OBJECT ${_zstd_sources})
# Stesse opzioni di ottimizzazione/sanitizer del resto della build; ma e' codice
# esterno non modificato: i warning non bloccano la build.
target_link_libraries(sevenzip_zstd PRIVATE sevenzip_flags)
target_compile_options(sevenzip_zstd PRIVATE -Wno-error)
target_compile_definitions(sevenzip_zstd PRIVATE
  ZSTD_MULTITHREAD          # compressione multi-thread (ZSTD_c_nbWorkers)
  ZSTD_DISABLE_ASM          # l'assembly di zstd riguarda solo la decompressione
  XXH_NAMESPACE=ZSTD_)      # gia' il default di zstd: esplicito per chiarezza
set(SEVENZIP_ZSTD_OBJECTS $<TARGET_OBJECTS:sevenzip_zstd>)

# L'encoder del fork e la registrazione del codec
list(FIND SEVENZIP_CPP_SOURCES "CPP/7zip/Compress/ZstdCodecRegister.cpp" _reg_pos)
if(_reg_pos LESS 0)
  message(FATAL_ERROR "ZstdCodecRegister.cpp non trovato in SEVENZIP_CPP_SOURCES")
endif()
math(EXPR _enc_pos "${_reg_pos} + 1")
list(INSERT SEVENZIP_CPP_SOURCES ${_enc_pos} "CPP/7zip/Compress/ZstdLibEncoder.cpp")
set_source_files_properties(
  CPP/7zip/Compress/ZstdCodecRegister.cpp
  CPP/7zip/Compress/ZstdLibEncoder.cpp
  CPP/7zip/Archive/7z/7zUpdateMt.cpp  # stima dell'encoder zstd (design 0003)
  C/FloatPlaneLz.c                    # blocchi non float del metodo FLOAT (design 0008)
  PROPERTIES COMPILE_DEFINITIONS Z7_ZSTD_LIB_ENCODER)
