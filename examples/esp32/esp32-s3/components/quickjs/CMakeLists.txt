# QuickJS-ng as an ESP-IDF component, compiled from the FetchContent'd source (QUICKJS_SRC_DIR is set
# by the top-level CMakeLists). The `qjs` library is exactly these four sources (matching upstream's
# CMakeLists). _GNU_SOURCE matches the upstream build flags. QuickJS is third-party C that trips a
# number of warnings the IDF build may escalate; keep them non-fatal so it compiles cleanly on the
# xtensa toolchain. The JS heap is routed to PSRAM by the host's JS_NewRuntime2 allocator (main.c).
idf_component_register(
    SRCS
        "${QUICKJS_SRC_DIR}/quickjs.c"
        "${QUICKJS_SRC_DIR}/libregexp.c"
        "${QUICKJS_SRC_DIR}/libunicode.c"
        "${QUICKJS_SRC_DIR}/dtoa.c"
    INCLUDE_DIRS "${QUICKJS_SRC_DIR}"
)

# Lite (parser-less) build: the device only ever runs precompiled bytecode (the erpkg from the
# config partition / hot reload), so the JS source parser is compiled out (~60 KB of flash).
# eval()/Function and er_runtime_load_source throw at runtime; the error overlay is precompiled
# bytecode, so it still renders. Remove QJS_DISABLE_PARSER to restore on-device source eval.
target_compile_definitions(${COMPONENT_LIB} PRIVATE _GNU_SOURCE QJS_DISABLE_PARSER)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-format
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-maybe-uninitialized
    -Wno-array-bounds
    # GCC 14 promotes these to hard errors by default (not gated by -Werror), and they fire on
    # benign cross-platform typedef differences (e.g. int32_t is `long` on xtensa, `int` on desktop,
    # so quickjs.c's int*/int32_t* mismatches trip -Wincompatible-pointer-types). Demote them.
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration
)
