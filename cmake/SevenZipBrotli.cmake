# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
# Metodo brotli nei .7z (design 0014) con la libreria di third_party/brotli
# (vedi third_party/brotli/README-Heptazip.md): encoder e decoder.
#
# Input:  SEVENZIP_BROTLI (ON/OFF)
# Output: target OBJECT "sevenzip_brotli", variabile SEVENZIP_BROTLI_OBJECTS,
#         BrotliCoder.cpp e BrotliRegister.cpp aggiunti a SEVENZIP_CPP_SOURCES.

set(SEVENZIP_BROTLI_OBJECTS "")

if(NOT SEVENZIP_BROTLI)
  return()
endif()

set(_br_dir "${CMAKE_SOURCE_DIR}/third_party/brotli/c")
file(GLOB _br_sources CONFIGURE_DEPENDS
  "${_br_dir}/common/*.c"
  "${_br_dir}/dec/*.c"
  "${_br_dir}/enc/*.c")
add_library(sevenzip_brotli OBJECT ${_br_sources})
target_link_libraries(sevenzip_brotli PRIVATE sevenzip_flags)
target_include_directories(sevenzip_brotli PRIVATE "${_br_dir}/include")
# codice esterno non modificato: senza warning (proprieta' dei sorgenti: vengono dopo
# il -Werror di sevenzip_flags)
set_source_files_properties(${_br_sources} PROPERTIES COMPILE_OPTIONS "-w")
set(SEVENZIP_BROTLI_OBJECTS $<TARGET_OBJECTS:sevenzip_brotli>)

list(APPEND SEVENZIP_CPP_SOURCES
  CPP/7zip/Compress/BrotliCoder.cpp
  CPP/7zip/Compress/BrotliRegister.cpp)
set_source_files_properties(CPP/7zip/Compress/BrotliCoder.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${_br_dir}/include")
