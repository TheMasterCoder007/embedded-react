cmake_minimum_required(VERSION 3.16)
project(embedded-react C)

# ---------------------------------------------------------------------------
# Monorepo top-level
# ---------------------------------------------------------------------------
# The engine builds the C99 static library that backends, bridges, and examples
# all link against. Backends, bridges, and examples are intentionally NOT
# add_subdirectory'd from here — each example pins one backend + one bridge in
# its own CMakeLists.txt. This keeps the engine build self-contained and lets
# downstream consumers (firmware projects pulling embedded-react via
# FetchContent) opt into only the pieces they want.
# ---------------------------------------------------------------------------

if (BUILD_TESTING AND CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
endif ()

add_subdirectory(engine)

find_program(CLANG_FORMAT_EXE clang-format)

if (CLANG_FORMAT_EXE)
    file(GLOB_RECURSE FORMAT_SOURCES
            CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/engine/*.c"
            "${CMAKE_SOURCE_DIR}/engine/*.h"
            "${CMAKE_SOURCE_DIR}/backends/*.c"
            "${CMAKE_SOURCE_DIR}/backends/*.h"
            "${CMAKE_SOURCE_DIR}/examples/*.c"
            "${CMAKE_SOURCE_DIR}/examples/*.h"
            "${CMAKE_SOURCE_DIR}/bridges/*.c"
            "${CMAKE_SOURCE_DIR}/bridges/*.h"
    )

    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-format"
    )

    add_custom_target(format-check
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking clang-format"
    )
endif ()