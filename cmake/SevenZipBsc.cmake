# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
# Metodo BWT nei .7z (design 0016, issue #81) con la libreria libbsc di
# third_party/libbsc (vedi third_party/libbsc/README-Heptazip.md): LZP + Burrows-Wheeler
# (libsais) + coder QLFC. Senza OpenMP: il parallelismo e' per blocchi nel codec.
#
# Input:  SEVENZIP_BSC (ON/OFF)
# Output: target OBJECT "sevenzip_bsc", variabile SEVENZIP_BSC_OBJECTS,
#         BscCoder.cpp e BscRegister.cpp aggiunti a SEVENZIP_CPP_SOURCES.

set(SEVENZIP_BSC_OBJECTS "")

if(NOT SEVENZIP_BSC)
  return()
endif()

set(_bsc_dir "${CMAKE_SOURCE_DIR}/third_party/libbsc")
set(_bsc_sources
  "${_bsc_dir}/libbsc/adler32/adler32.cpp"
  "${_bsc_dir}/libbsc/bwt/bwt.cpp"
  "${_bsc_dir}/libbsc/bwt/libsais/libsais.c"
  "${_bsc_dir}/libbsc/coder/coder.cpp"
  "${_bsc_dir}/libbsc/coder/qlfc/qlfc.cpp"
  "${_bsc_dir}/libbsc/coder/qlfc/qlfc_model.cpp"
  "${_bsc_dir}/libbsc/filters/detectors.cpp"
  "${_bsc_dir}/libbsc/filters/preprocessing.cpp"
  "${_bsc_dir}/libbsc/libbsc/libbsc.cpp"
  "${_bsc_dir}/libbsc/lzp/lzp.cpp"
  "${_bsc_dir}/libbsc/platform/platform.cpp"
  "${_bsc_dir}/libbsc/st/st.cpp")
add_library(sevenzip_bsc OBJECT ${_bsc_sources})
target_link_libraries(sevenzip_bsc PRIVATE sevenzip_flags)
target_include_directories(sevenzip_bsc PRIVATE "${_bsc_dir}")
# il range coder legge i dati compressi a 16 bit: con dati corrotti l'indirizzo puo'
# essere impari (UBSan, fuzz_codec); con questa opzione libbsc usa memcpy (stesso codice
# macchina su x86-64)
target_compile_definitions(sevenzip_bsc PRIVATE LIBBSC_NO_UNALIGNED_ACCESS)
# codice esterno non modificato: senza warning (proprieta' dei sorgenti: vengono dopo
# il -Werror di sevenzip_flags)
set_source_files_properties(${_bsc_sources} PROPERTIES COMPILE_OPTIONS "-w")
set(SEVENZIP_BSC_OBJECTS $<TARGET_OBJECTS:sevenzip_bsc>)

list(APPEND SEVENZIP_CPP_SOURCES
  CPP/7zip/Compress/BscCoder.cpp
  CPP/7zip/Compress/BscRegister.cpp)
set_source_files_properties(CPP/7zip/Compress/BscCoder.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${_bsc_dir}")
