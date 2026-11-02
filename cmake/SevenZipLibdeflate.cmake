# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
# Compressore Deflate di libdeflate (third_party/libdeflate, vedi README-Tensor7.md),
# usato da CPP/7zip/Compress/DeflateEncoder.cpp per gzip, zip e 7z (design 0012).
#
# Il decoder e Deflate64 restano quelli di 7-Zip: senza questa opzione l'encoder usa
# solo il proprio codice.
#
# Input:  SEVENZIP_LIBDEFLATE (ON/OFF)
# Output: target OBJECT "sevenzip_libdeflate", variabile SEVENZIP_LIBDEFLATE_OBJECTS,
#         definizione Z7_DEFLATE_LIBDEFLATE su DeflateEncoder.cpp.

set(SEVENZIP_LIBDEFLATE_OBJECTS "")

if(NOT SEVENZIP_LIBDEFLATE)
  return()
endif()

set(_ld_dir "${CMAKE_SOURCE_DIR}/third_party/libdeflate")
add_library(sevenzip_libdeflate OBJECT
  "${_ld_dir}/lib/deflate_compress.c"
  "${_ld_dir}/lib/utils.c")
target_link_libraries(sevenzip_libdeflate PRIVATE sevenzip_flags)
# codice esterno: senza warning (proprieta' dei sorgenti: vengono dopo
# il -Werror di sevenzip_flags)
set_source_files_properties(
  "${_ld_dir}/lib/deflate_compress.c" "${_ld_dir}/lib/utils.c"
  PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(sevenzip_libdeflate PRIVATE "${_ld_dir}")
set(SEVENZIP_LIBDEFLATE_OBJECTS $<TARGET_OBJECTS:sevenzip_libdeflate>)

set_source_files_properties(CPP/7zip/Compress/DeflateEncoder.cpp PROPERTIES
  COMPILE_DEFINITIONS Z7_DEFLATE_LIBDEFLATE
  INCLUDE_DIRECTORIES "${_ld_dir}")
