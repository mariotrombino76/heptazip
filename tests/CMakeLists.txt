# Test del fork (vedi BUILD-modern.md, sezione "Test").
#
#   cmake --build build -j && ctest --test-dir build --output-on-failure
#
# Per l'esecuzione con i sanitizer:
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEVENZIP_SANITIZE=thread
#   cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# extract_trace: registra la sequenza di callback di IInArchive::Extract().
# Usa gli stessi oggetti di 7zz (incluse le routine asm), esclusa la UI console.
add_executable(extract_trace
  extract_trace.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(extract_trace PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

# Test unitari del modulo di estrazione parallela (7zExtractMt).
add_executable(unit_7zextract_mt
  unit_7zextract_mt.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(unit_7zextract_mt PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

# Archivio creato con una callback che dichiara per un file una dimensione diversa da
# quella dei dati (file cresciuto/accorciato durante la compressione, design 0003).
add_executable(update_growing
  update_growing.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(update_growing PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

# Banco di prova dei match finder (strumento, non test: docs/design/0005).
add_executable(mf_bench
  ${CMAKE_SOURCE_DIR}/tools/mfbench/mf_bench.c
  $<TARGET_OBJECTS:sevenzip_c>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})  # C/FloatPlaneLz.c usa l'encoder zstd
target_link_libraries(mf_bench PRIVATE sevenzip_flags Threads::Threads)

# Test unitari del modulo di compressione parallela (7zUpdateMt, docs/design/0003).
add_executable(unit_7zupdate_mt
  unit_7zupdate_mt.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(unit_7zupdate_mt PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})
if(SEVENZIP_ZSTD_ENCODER)
  # i controlli sulla stima dell'encoder zstd
  set_source_files_properties(unit_7zupdate_mt.cpp PROPERTIES COMPILE_DEFINITIONS Z7_ZSTD_LIB_ENCODER)
endif()

# COM_TRY_END: E_OUTOFMEMORY solo per i fallimenti di allocazione (CPP/Common/ComTry.h).
add_executable(unit_com_try
  unit_com_try.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(unit_com_try PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

# Fuzzing dei gestori di archivio (design 0007). fuzz_replay e' lo stesso harness senza
# libFuzzer: riesegue gli input che avevano trovato bug (tests/fuzz/regress/<formato>/).
add_executable(fuzz_replay
  fuzz/fuzz_archive.cpp
  $<TARGET_OBJECTS:sevenzip_c>
  $<TARGET_OBJECTS:sevenzip_cpp>
  ${SEVENZIP_ASM_OBJECTS}
  ${SEVENZIP_ZSTD_OBJECTS})
target_compile_definitions(fuzz_replay PRIVATE FUZZ_REPLAY)
target_link_libraries(fuzz_replay PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

# fuzz_archive (libFuzzer) non e' un test di CTest: si lancia con tools/fuzz/run-campaign.sh.
set(_fuzz_targets "")
if(SEVENZIP_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT SEVENZIP_SANITIZE MATCHES "fuzzer-no-link")
    message(FATAL_ERROR "SEVENZIP_FUZZ richiede Clang e SEVENZIP_SANITIZE con fuzzer-no-link")
  endif()
  add_executable(fuzz_archive
    fuzz/fuzz_archive.cpp
    $<TARGET_OBJECTS:sevenzip_c>
    $<TARGET_OBJECTS:sevenzip_cpp>
    ${SEVENZIP_ASM_OBJECTS}
    ${SEVENZIP_ZSTD_OBJECTS})
  target_link_libraries(fuzz_archive PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(fuzz_archive PRIVATE -fsanitize=fuzzer)
  add_executable(fuzz_float fuzz/fuzz_float.c
    $<TARGET_OBJECTS:sevenzip_c> ${SEVENZIP_ASM_OBJECTS} ${SEVENZIP_ZSTD_OBJECTS})
  target_link_libraries(fuzz_float PRIVATE sevenzip_flags Threads::Threads)
  target_link_options(fuzz_float PRIVATE -fsanitize=fuzzer)
  set(_fuzz_targets fuzz_archive fuzz_float)
endif()

# Gli oggetti assembly (SEVENZIP_ASM) sono generati nella directory principale.
if(TARGET sevenzip_asm)
  foreach(_t extract_trace unit_7zextract_mt update_growing mf_bench unit_7zupdate_mt unit_com_try fuzz_replay ${_fuzz_targets})
    add_dependencies(${_t} sevenzip_asm)
  endforeach()
endif()

# Equivalenza delle callback tra configurazioni di estrazione.
# Al passo 1 confronta -mmt=1 con -mmt=<tutti i core>: anche la decompressione
# multi-thread di LZMA2 e il mixer MT devono produrre la stessa traccia.
# Con i sanitizer: opzioni runtime e, per TSan con GCC, ASLR disattivato
# (setarch -R): la libtsan di GCC 13 non supporta l'entropia ASLR dei kernel
# recenti ("FATAL: ThreadSanitizer: unexpected memory mapping"). Con Clang 20
# (compiler-rt) non serve.
set(_test_launcher "")
set(_test_env "")
if(SEVENZIP_SANITIZE MATCHES "thread")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    find_program(SETARCH_EXECUTABLE setarch REQUIRED)
    set(_test_launcher ${SETARCH_EXECUTABLE} ${CMAKE_SYSTEM_PROCESSOR} -R)
  endif()
  list(APPEND _test_env
    "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
endif()
if(SEVENZIP_SANITIZE MATCHES "address")
  list(APPEND _test_env "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0")
endif()
if(SEVENZIP_SANITIZE MATCHES "undefined")
  list(APPEND _test_env "UBSAN_OPTIONS=print_stacktrace=1")
endif()

# Cross build (cmake/toolchains/): the programs of the target run through the emulator
# (qemu-user). Direct tests get it as a prefix (_emu); the Python tests get, for each
# program, a small script that runs it through the emulator (_sz_program).
set(_emu "")
if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
  set(_emu ${CMAKE_CROSSCOMPILING_EMULATOR})
endif()
function(_sz_program var target)
  if(_emu)
    list(JOIN _emu " " _e)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/emu/${target}
      CONTENT "#!/bin/sh\nexec ${_e} \"$<TARGET_FILE:${target}>\" \"$@\"\n"
      FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                       WORLD_READ WORLD_EXECUTE)
    set(${var} ${CMAKE_CURRENT_BINARY_DIR}/emu/${target} PARENT_SCOPE)
  else()
    set(${var} $<TARGET_FILE:${target}> PARENT_SCOPE)
  endif()
endfunction()
_sz_program(_7zz 7zz)
_sz_program(_extract_trace extract_trace)
_sz_program(_update_growing update_growing)

add_test(NAME extract_equivalence_mt
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/extract_equivalence.py
          --7zz ${_7zz}
          --trace-a ${_extract_trace}
          --config-a=-mmt=1
          --config-b=-mmt=on)
set_tests_properties(extract_equivalence_mt PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")

add_test(NAME unit_7zextract_mt COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:unit_7zextract_mt>)
set_tests_properties(unit_7zextract_mt PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

add_test(NAME unit_com_try COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:unit_com_try>)
set_tests_properties(unit_com_try PROPERTIES TIMEOUT 60 ENVIRONMENT "${_test_env}")

add_test(NAME unit_7zupdate_mt COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:unit_7zupdate_mt>)
set_tests_properties(unit_7zupdate_mt PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

# Compressione parallela dei folder (-mpfc, docs/design/0003): archivi identici byte
# per byte, stessa console e stesso codice di uscita della compressione sequenziale.
add_test(NAME update_equivalence_pfc
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/update_equivalence.py
          --7zz ${_7zz}
          --config-a=-mpfc=off
          --config-b=-mpfc=on)
set_tests_properties(update_equivalence_pfc PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")

add_test(NAME update_growing
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/update_growing.py
          --7zz ${_7zz}
          --tool ${_update_growing})
set_tests_properties(update_growing PROPERTIES TIMEOUT 1800 ENVIRONMENT "${_test_env}")

# Estrazione parallela dei folder (-mpfd, docs/design/0001): la traccia deve essere
# identica all'estrazione sequenziale. Varianti:
#  - lowmem: con 28 MiB il limite dei job "grandi" e' 7 MiB; nell'archivio non-solid
#    della suite i folder dei file piccoli costano ~6.5 MiB (dizionario LZMA2 di 3 MiB
#    comune a tutti i folder) e vanno in parallelo, quelli dei file grandi (7.5-9 MiB)
#    in linea: i due percorsi si alternano (verificato: 42 paralleli, 4 in linea).
#  - 2threads: pochi worker, il controllo d'ammissione resta in attesa.
foreach(_variant IN ITEMS "default;" "lowmem;-mmemuse=28m" "2threads;-mmt=2")
  list(GET _variant 0 _name)
  list(GET _variant 1 _opts)
  add_test(NAME extract_equivalence_pfd_${_name}
    COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/extract_equivalence.py
            --7zz ${_7zz}
            --trace-a ${_extract_trace}
            "--config-a=-mpfd=off ${_opts}"
            "--config-b=-mpfd=on ${_opts}")
  set_tests_properties(extract_equivalence_pfd_${_name} PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")
endforeach()

# CRC32 con PCLMULQDQ (C/7zCrcClmul.h) contro un CRC32 di riferimento indipendente.
add_executable(unit_crc_clmul unit_crc_clmul.c ${CMAKE_SOURCE_DIR}/C/CpuArch.c)
target_link_libraries(unit_crc_clmul PRIVATE sevenzip_flags)
add_test(NAME fuzz_regress
  COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:fuzz_replay> ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regress)
# Con ASan le allocazioni oltre 1 GiB sono errori: cosi' anche gli input "oom-*" (file di
# pochi byte che chiedevano gigabyte) fanno fallire il test se il bug ritorna.
set(_fuzz_env "${_test_env}")
list(TRANSFORM _fuzz_env REPLACE "^ASAN_OPTIONS=(.*)" "ASAN_OPTIONS=\\1:max_allocation_size_mb=1024")
set_tests_properties(fuzz_regress PROPERTIES TIMEOUT 300 ENVIRONMENT "${_fuzz_env}")

# Formato FLOAT (design 0008): round-trip, scelta del tipo, input corrotti e troncati
add_executable(unit_float_plane unit_float_plane.c
  $<TARGET_OBJECTS:sevenzip_c> ${SEVENZIP_ASM_OBJECTS} ${SEVENZIP_ZSTD_OBJECTS})
target_link_libraries(unit_float_plane PRIVATE sevenzip_flags Threads::Threads m)
if(TARGET sevenzip_asm)
  add_dependencies(unit_float_plane sevenzip_asm)
endif()
add_test(NAME unit_float_plane COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:unit_float_plane>)
set_tests_properties(unit_float_plane PROPERTIES TIMEOUT 1800 ENVIRONMENT "${_test_env}")

add_test(NAME unit_crc_clmul COMMAND ${_test_launcher} ${_emu} $<TARGET_FILE:unit_crc_clmul>)
set_tests_properties(unit_crc_clmul PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

# Output JSON di "l -json" confrontato con "-slt" (docs/json-output.md).
add_test(NAME list_json
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/list_json.py
          --7zz ${_7zz})
set_tests_properties(list_json PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")
