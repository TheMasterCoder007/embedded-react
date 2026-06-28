# ESP-IDF project for the embedded-react Flow A host on the ESP32-S3.
# Build:  idf.py set-target esp32s3 && idf.py build flash monitor
#
# Dependencies are FETCHED from GitHub at configure time (CMake FetchContent), so this example is a
# self-contained template: copy it to its own repo and `idf.py build` pulls everything it needs.
#   - QuickJS-ng — ALWAYS fetched, pinned to the version below. It MUST match the bytecode baked into
#     main/app.bundle.qbc (JS_ReadObject is version-specific) and the monorepo's bridge build.
#   - embedded-react (engine + bridge) — uses the LOCAL monorepo checkout when building in-tree (so
#     development picks up your live edits), otherwise fetched from GitHub.
cmake_minimum_required(VERSION 3.16)

# Defend against an inherited host toolchain file. A globally-set CMAKE_TOOLCHAIN_FILE (e.g. a
# user-wide env var pointing at vcpkg, common on dev machines + IDEs) is wrong for an ESP-IDF
# project — IDF cross-compiles and selects its own toolchain. Left in place it fails configure with
# "CMAKE_TOOLCHAIN_FILE does not match IDF_TARGET". Clear it before IDF's project() picks the toolchain.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "embedded-react: ignoring inherited CMAKE_TOOLCHAIN_FILE '${CMAKE_TOOLCHAIN_FILE}' — IDF manages the toolchain")
    unset(CMAKE_TOOLCHAIN_FILE CACHE)
    unset(CMAKE_TOOLCHAIN_FILE)
endif()

# This is an ESP-IDF project — it must be configured by idf.py (which sets IDF_PATH), not by plain
# cmake or a non-IDF CLion toolchain. Fail early with a clear message instead of a cryptic
# "/tools/cmake/project.cmake not found" later.
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR
        "IDF_PATH is not set — this is an ESP-IDF project.\n"
        "Build it from an IDF-exported shell:\n"
        "    . $IDF_PATH/export.sh        (Linux/macOS)  or  %IDF_PATH%\\export.bat  (Windows)\n"
        "    idf.py set-target esp32s3 && idf.py build flash monitor\n"
        "Plain cmake / a non-IDF CLion toolchain cannot configure it.")
endif()

include(FetchContent)

# --- QuickJS-ng (pinned) ---------------------------------------------------------------------------
# SOURCE_SUBDIR points at a non-existent dir so MakeAvailable populates the source WITHOUT running
# QuickJS's own CMake (we compile its 4 sources ourselves in components/quickjs). This is the
# non-deprecated replacement for FetchContent_Populate.
set(QUICKJS_GIT_TAG "v0.15.0")
FetchContent_Declare(quickjs
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG        ${QUICKJS_GIT_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _skip_cmake_
)
message(STATUS "embedded-react: ensuring QuickJS-ng ${QUICKJS_GIT_TAG} ...")
FetchContent_MakeAvailable(quickjs)
set(QUICKJS_SRC_DIR "${quickjs_SOURCE_DIR}" CACHE INTERNAL "QuickJS-ng source dir")

# --- embedded-react engine + bridge (local in-repo, else fetched) ----------------------------------
# ../../.. = repo root (this project lives at examples/esp32/esp32-s3/).
set(_er_local "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(EXISTS "${_er_local}/engine/include/er_scene.h")
    set(EMBEDDED_REACT_SRC_DIR "${_er_local}" CACHE INTERNAL "embedded-react source (local monorepo)")
    message(STATUS "embedded-react: using local monorepo source at ${_er_local}")
else()
    FetchContent_Declare(embedded_react
        GIT_REPOSITORY https://github.com/TheMasterCoder007/embedded-react.git
        GIT_TAG        v0.5.2
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _skip_cmake_
    )
    message(STATUS "embedded-react: fetching engine + bridge from GitHub ...")
    FetchContent_MakeAvailable(embedded_react)
    set(EMBEDDED_REACT_SRC_DIR "${embedded_react_SOURCE_DIR}" CACHE INTERNAL "embedded-react source (fetched)")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(embedded-react-esp32)

# --- distclean -------------------------------------------------------------------------------------
# `idf.py fullclean` deletes build/ with Python's shutil.rmtree, which on Windows CANNOT unlink the
# read-only git packfiles that FetchContent's clone of QuickJS-ng leaves under
# build/_deps/quickjs-src/.git/objects/pack/ — it aborts with "[WinError 5] Access is denied" and
# leaves a half-wiped build/. This target removes that FetchContent cache the read-only-safe way
# (`cmake -E rm -rf` clears the attribute, unlike shutil.rmtree); the next configure re-fetches it
# cleanly, and a subsequent `idf.py fullclean` then succeeds too. Run it when the deps cache wedges:
#
#     cmake --build build --target distclean
add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/_deps"
    COMMENT "distclean: removing FetchContent deps cache (${CMAKE_BINARY_DIR}/_deps)")
