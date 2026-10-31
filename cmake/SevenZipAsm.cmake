# Routine assembly x86-64 di 7-Zip (Asm/x86/*.asm, sintassi MASM).
#
# Replica la logica di CPP/7zip/7zip_gcc.mak con USE_ASM=1 e IS_X64=1:
#   - alcuni file C/*.c vengono sostituiti dalla versione .asm equivalente;
#   - LzmaDecOpt.asm si aggiunge e C/LzmaDec.c va compilato con -DZ7_LZMA_DEC_OPT.
#
# Assembler supportati: asmc (default upstream), uasm, jwasm.
# Con jwasm la versione asm di AesOpt non e' usata (come upstream).
#
# Input/Output: modifica SEVENZIP_C_SOURCES e imposta SEVENZIP_ASM_OBJECTS.

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  message(FATAL_ERROR "SEVENZIP_ASM e' supportato solo su x86_64 (CPU: ${CMAKE_SYSTEM_PROCESSOR})")
endif()

find_program(SEVENZIP_ASSEMBLER NAMES asmc uasm jwasm HINTS "$ENV{HOME}/.local/bin"
  DOC "Assembler MASM-compatibile per Asm/x86")
if(NOT SEVENZIP_ASSEMBLER)
  message(FATAL_ERROR
    "SEVENZIP_ASM=ON ma nessun assembler trovato (asmc, uasm, jwasm).\n"
    "Installa ad es. UASM (https://github.com/Terraspace/UASM) oppure ASMC "
    "(https://github.com/nidud/asmc) e rilancia cmake, o usa -DSEVENZIP_ASM=OFF.")
endif()
get_filename_component(_asm_name "${SEVENZIP_ASSEMBLER}" NAME)
message(STATUS "Assembler: ${SEVENZIP_ASSEMBLER}")

# File C sostituiti -> sorgente asm.
# 7zip-modern: LzFindOpt resta in C. Con LzFind_MatchEnd() (design 0004) il C e' piu'
# veloce di LzFindOpt.asm: compressione LZMA2 -mx5 con 20 thread 20,7 s contro 22,1 s,
# LZMA -mmt=2 34,8 s contro 37,3 s (Silesia, misure alternate, archivi identici).
set(_asm_replace
  7zCrcOpt
  XzCrc64Opt
  Sha1Opt
  Sha256Opt
  Sort)
if(NOT _asm_name STREQUAL "jwasm")
  list(APPEND _asm_replace AesOpt)
endif()

set(_asm_files "")
foreach(_name IN LISTS _asm_replace)
  list(REMOVE_ITEM SEVENZIP_C_SOURCES "C/${_name}.c")
  list(APPEND _asm_files ${_name})
endforeach()
list(APPEND _asm_files LzmaDecOpt)

set_source_files_properties(C/LzmaDec.c PROPERTIES COMPILE_DEFINITIONS Z7_LZMA_DEC_OPT)

set(SEVENZIP_ASM_OBJECTS "")
set(_asm_outdir "${CMAKE_BINARY_DIR}/asm")
file(MAKE_DIRECTORY "${_asm_outdir}")
foreach(_name IN LISTS _asm_files)
  set(_src "${CMAKE_SOURCE_DIR}/Asm/x86/${_name}.asm")
  set(_obj "${_asm_outdir}/${_name}.o")
  # -Fo<dir>/ : l'assembler genera <dir>/<nome>.o (stesso schema di AFLAGS upstream)
  add_custom_command(
    OUTPUT "${_obj}"
    COMMAND "${SEVENZIP_ASSEMBLER}" -nologo -elf64 -DABI_LINUX "-Fo${_asm_outdir}/" "${_src}"
    DEPENDS "${_src}" "${CMAKE_SOURCE_DIR}/Asm/x86/7zAsm.asm"
    # Alcuni .asm (es. Sort.asm) includono "../../../../Asm/x86/7zAsm.asm":
    # il percorso e' relativo a CPP/7zip/Bundles/<bundle>, da dove upstream
    # lancia l'assembler. Usiamo la stessa directory di lavoro.
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/CPP/7zip/Bundles/Alone2"
    COMMENT "ASM ${_name}.asm"
    VERBATIM)
  list(APPEND SEVENZIP_ASM_OBJECTS "${_obj}")
endforeach()
set_source_files_properties(${SEVENZIP_ASM_OBJECTS} PROPERTIES
  EXTERNAL_OBJECT TRUE GENERATED TRUE)
# Target che genera gli oggetti: chi li usa (7zz, eseguibili di tests/) deve dipendere
# da questo target, altrimenti in una build parallela un target di un'altra directory
# puo' cercare gli .o prima che esistano ("No rule to make target asm/...").
add_custom_target(sevenzip_asm DEPENDS ${SEVENZIP_ASM_OBJECTS})
