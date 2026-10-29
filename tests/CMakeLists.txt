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
  ${SEVENZIP_ASM_OBJECTS})
target_link_libraries(extract_trace PRIVATE sevenzip_flags Threads::Threads ${CMAKE_DL_LIBS})

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
