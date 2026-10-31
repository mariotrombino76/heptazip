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

add_test(NAME extract_equivalence_mt
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/extract_equivalence.py
          --7zz $<TARGET_FILE:7zz>
          --trace-a $<TARGET_FILE:extract_trace>
          --config-a=-mmt=1
          --config-b=-mmt=on)
set_tests_properties(extract_equivalence_mt PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")

add_test(NAME unit_7zextract_mt COMMAND ${_test_launcher} $<TARGET_FILE:unit_7zextract_mt>)
set_tests_properties(unit_7zextract_mt PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

add_test(NAME unit_7zupdate_mt COMMAND ${_test_launcher} $<TARGET_FILE:unit_7zupdate_mt>)
set_tests_properties(unit_7zupdate_mt PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

# Compressione parallela dei folder (-mpfc, docs/design/0003): archivi identici byte
# per byte, stessa console e stesso codice di uscita della compressione sequenziale.
add_test(NAME update_equivalence_pfc
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/update_equivalence.py
          --7zz $<TARGET_FILE:7zz>
          --config-a=-mpfc=off
          --config-b=-mpfc=on)
set_tests_properties(update_equivalence_pfc PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")

add_test(NAME update_growing
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/update_growing.py
          --7zz $<TARGET_FILE:7zz>
          --tool $<TARGET_FILE:update_growing>)
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
            --7zz $<TARGET_FILE:7zz>
            --trace-a $<TARGET_FILE:extract_trace>
            "--config-a=-mpfd=off ${_opts}"
            "--config-b=-mpfd=on ${_opts}")
  set_tests_properties(extract_equivalence_pfd_${_name} PROPERTIES TIMEOUT 3600 ENVIRONMENT "${_test_env}")
endforeach()

# CRC32 con PCLMULQDQ (C/7zCrcClmul.h) contro un CRC32 di riferimento indipendente.
add_executable(unit_crc_clmul unit_crc_clmul.c ${CMAKE_SOURCE_DIR}/C/CpuArch.c)
target_link_libraries(unit_crc_clmul PRIVATE sevenzip_flags)
add_test(NAME unit_crc_clmul COMMAND ${_test_launcher} $<TARGET_FILE:unit_crc_clmul>)
set_tests_properties(unit_crc_clmul PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")

# Output JSON di "l -json" confrontato con "-slt" (docs/json-output.md).
add_test(NAME list_json
  COMMAND ${_test_launcher} $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/list_json.py
          --7zz $<TARGET_FILE:7zz>)
set_tests_properties(list_json PROPERTIES TIMEOUT 600 ENVIRONMENT "${_test_env}")
