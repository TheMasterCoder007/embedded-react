cmake_minimum_required(VERSION 3.16)
project(erb-fuzz C)

# ---------------------------------------------------------------------------
# erb-fuzz — fuzzes the Flow B loader, verifier and VM, on the engine and the software backend, under ASan and
# UBSan. See README.md. CI builds it with libFuzzer on Linux:
#   cmake -S tools/erb-fuzz -B tools/erb-fuzz/build -DCMAKE_C_COMPILER=clang-18 -DERB_FUZZ_LIBFUZZER=ON
#   cmake --build tools/erb-fuzz/build -j
#   ctest --test-dir tools/erb-fuzz/build --output-on-failure
#
# With ERB_FUZZ_LIBFUZZER off, erb-fuzz only replays the files it is given, for compilers with no libFuzzer.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ERB_FUZZ_LIBFUZZER "Link libFuzzer. Needs clang with its runtime; Apple's clang has none" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../erb-parity/engine-features.cmake)
set(ER_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)

# Before the subdirectories, so the engine and the runtime are instrumented too.
add_compile_options(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)
add_link_options(-fsanitize=address,undefined)
if (ERB_FUZZ_LIBFUZZER)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif ()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine  ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

add_library(erb-fuzz-software STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software/renderer_backend.c)
target_include_directories(erb-fuzz-software PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software)
target_link_libraries(erb-fuzz-software PUBLIC embedded-react)

add_executable(erb-fuzz fuzz_package.c)
target_link_libraries(erb-fuzz PRIVATE erb-runtime erb-fuzz-software)
if (ERB_FUZZ_LIBFUZZER)
    target_link_options(erb-fuzz PRIVATE -fsanitize=fuzzer)
else ()
    target_sources(erb-fuzz PRIVATE replay_main.c)
endif ()

# ---------------------------------------------------------------------------
# Seeds must load, or the fuzzer would only be mutating packages the verifier rejects at once. Everything else
# in the corpus only has to run clean.
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB ERB_FUZZ_SEEDS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/seeds/*)
file(GLOB ERB_FUZZ_FOUND CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/corpus/found/*
        ${CMAKE_CURRENT_SOURCE_DIR}/corpus/regressions/*)
add_test(NAME erb-fuzz-seeds-load COMMAND erb-fuzz ${ERB_FUZZ_SEEDS})
set_tests_properties(erb-fuzz-seeds-load PROPERTIES ENVIRONMENT "ERB_FUZZ_REQUIRE_LOAD=1")
if (ERB_FUZZ_FOUND)
    add_test(NAME erb-fuzz-corpus-replays COMMAND erb-fuzz ${ERB_FUZZ_FOUND})
endif ()
