# Phase-1 host (headless): boots QuickJS in PSRAM, loads the embedded bytecode, runs the React app
# with a no-op backend, and prints over UART. The bytecode blob is embedded via EMBED_FILES, giving
# the linker symbols _binary_app_bundle_qbc_start / _end (see main.c).
#
# app.bundle.qbc is the precompiled bytecode of the app — a generated artifact, NOT committed (see
# .gitignore). Generate it before building (and whenever the JS app changes):
#   (in bridges/quickjs/js) npm run build
#   bridges/quickjs/build/er-bridge-quickjs-compile  bridges/quickjs/js/dist/app.bundle.js  \
#       examples/esp32/main/app.bundle.qbc
# Fail with a clear message instead of a cryptic EMBED_FILES error if it hasn't been generated yet.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/app.bundle.qbc")
    message(FATAL_ERROR
        "main/app.bundle.qbc is missing — it's the precompiled app bytecode and is not committed.\n"
        "Generate it first:\n"
        "    1) in bridges/quickjs/js:  npm run build\n"
        "    2) build the precompiler:  cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile\n"
        "    3) er-bridge-quickjs-compile bridges/quickjs/js/dist/app.bundle.js examples/esp32/main/app.bundle.qbc")
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES engine er-bridge-quickjs quickjs esp_psram esp_timer
    EMBED_FILES "app.bundle.qbc"
)
