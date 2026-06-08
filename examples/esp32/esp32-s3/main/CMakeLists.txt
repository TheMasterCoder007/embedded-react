# Phase-2 host (RGB display): boots QuickJS in PSRAM, loads the embedded bytecode, brings up the
# Waveshare 7" RGB panel (board.c) and draws the React app to it via the esp32-lcd backend (falling
# back to a no-op backend + UART logging if the panel fails). The bytecode blob is embedded via
# EMBED_FILES, giving the linker symbols _binary_app_bundle_qbc_start / _end (see main.c).
#
# app.bundle.qbc is the precompiled bytecode of the app — a generated artifact, NOT committed (see
# .gitignore). Generate it before building (and whenever the JS app changes):
#   (in bridges/quickjs/js) npm run build
#   bridges/quickjs/build/er-bridge-quickjs-compile  bridges/quickjs/js/dist/app.bundle.js  \
#       examples/esp32/esp32-s3/main/app.bundle.qbc
# Fail with a clear message instead of a cryptic EMBED_FILES error if it hasn't been generated yet.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/app.bundle.qbc")
    message(FATAL_ERROR
        "main/app.bundle.qbc is missing — it's the precompiled app bytecode and is not committed.\n"
        "Generate it first:\n"
        "    1) in bridges/quickjs/js:  npm run build\n"
        "    2) build the precompiler:  cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile\n"
        "    3) er-bridge-quickjs-compile bridges/quickjs/js/dist/app.bundle.js examples/esp32/esp32-s3/main/app.bundle.qbc")
endif()

# Baked assets (images + fonts) the active demo imports, as premultiplied ARGB8888 / BitmapFont data
# in flash. assets.generated.c is produced by `npm run build` (step 1 above) into
# bridges/quickjs/js/dist and exposes er_register_assets(), which main.c calls at boot. Like the
# bytecode it's a generated artifact, not committed; compile it when present and define
# ER_HAVE_BAKED_ASSETS so main.c makes the call.
set(ER_GENERATED_ASSETS "${CMAKE_CURRENT_LIST_DIR}/../../../../bridges/quickjs/js/dist/assets.generated.c")
set(ER_GENERATED_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../bridges/quickjs/js/dist")

set(ER_MAIN_SRCS "main.c" "board.c")
set(ER_MAIN_INCS ".")
if(EXISTS "${ER_GENERATED_ASSETS}")
    list(APPEND ER_MAIN_SRCS "${ER_GENERATED_ASSETS}")
    list(APPEND ER_MAIN_INCS "${ER_GENERATED_ASSETS_DIR}")
endif()

idf_component_register(
    SRCS ${ER_MAIN_SRCS}
    INCLUDE_DIRS ${ER_MAIN_INCS}
    REQUIRES engine er-bridge-quickjs quickjs esp32-lcd-backend
             esp_psram esp_timer esp_lcd esp_driver_i2c esp_driver_gpio
    EMBED_FILES "app.bundle.qbc"
)

if(EXISTS "${ER_GENERATED_ASSETS}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_HAVE_BAKED_ASSETS=1)
endif()

# Enable the on-screen performance overlay (FPS / CPU / free memory). main.c gates its host-side
# metric gathering on ER_PERF_OVERLAY; the engine component sets the same flag so the renderer draws
# the panel. Set here too so the host wiring is enabled regardless of requirement propagation order.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_OVERLAY=1)

# Whole-screen rotation (clockwise): 0 / 90 / 180 / 270. Change this for a differently-mounted panel;
# 0 (the default) is the fast path (a plain framebuffer copy at present). 90/270 give a portrait UI on
# the landscape panel; the thermostat demo stacks its layout for the narrower width.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_DISPLAY_ROTATION=0)
