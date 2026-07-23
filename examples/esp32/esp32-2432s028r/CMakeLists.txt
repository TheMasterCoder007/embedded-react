# ESP-IDF project for the embedded-react Flow B (AOT) host on the DIYmall ESP32-2432S028R "Cheap Yellow
# Display" (plain ESP32-WROOM-32, NO PSRAM).
#   idf.py set-target esp32 && idf.py build flash monitor
#
# Flow B needs NO QuickJS (the app is compiled C), so unlike the esp32-s3 example this only pulls in the
# embedded-react engine + backend — from the LOCAL monorepo checkout when building in-tree, else fetched
# from GitHub. The AOT app itself (bridges/quickjs/js/dist/app.gen.c) is generated separately by
# `npm run aot` and compiled by main/ (see main/CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)

# A globally-set CMAKE_TOOLCHAIN_FILE (e.g. a user-wide vcpkg env var) is wrong for ESP-IDF, which
# cross-compiles and picks its own toolchain. Clear it before IDF's project() runs.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "embedded-react: ignoring inherited CMAKE_TOOLCHAIN_FILE '${CMAKE_TOOLCHAIN_FILE}' — IDF manages the toolchain")
    unset(CMAKE_TOOLCHAIN_FILE CACHE)
    unset(CMAKE_TOOLCHAIN_FILE)
endif()

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR
        "IDF_PATH is not set — this is an ESP-IDF project.\n"
        "Build it from an IDF-exported shell:\n"
        "    . $IDF_PATH/export.sh        (Linux/macOS)  or  %IDF_PATH%\\export.bat  (Windows)\n"
        "    idf.py set-target esp32 && idf.py build flash monitor")
endif()

include(FetchContent)

# --- embedded-react engine + backend (local in-repo, else fetched) ---------------------------------
# ../../.. = repo root (this project lives at examples/esp32/esp32-2432s028r/).
set(_er_local "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(EXISTS "${_er_local}/engine/include/er_scene.h")
    set(EMBEDDED_REACT_SRC_DIR "${_er_local}" CACHE INTERNAL "embedded-react source (local monorepo)")
    message(STATUS "embedded-react: using local monorepo source at ${_er_local}")
else()
    FetchContent_Declare(embedded_react
        GIT_REPOSITORY https://github.com/TheMasterCoder007/embedded-react.git
        GIT_TAG        v0.9.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _skip_cmake_
    )
    message(STATUS "embedded-react: fetching engine + backend from GitHub ...")
    FetchContent_MakeAvailable(embedded_react)
    set(EMBEDDED_REACT_SRC_DIR "${embedded_react_SOURCE_DIR}" CACHE INTERNAL "embedded-react source (fetched)")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(embedded-react-cyd)
