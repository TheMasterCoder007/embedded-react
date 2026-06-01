cmake_minimum_required(VERSION 3.16)

# No language is enabled: this root project compiles nothing (no compiler probe).
# The name is intentionally distinct from the engine's `embedded-react` so an IDE
# never confuses this utility project with the engine subproject.
project(embedded-react-tools NONE)

# ---------------------------------------------------------------------------
# Monorepo top-level
# ---------------------------------------------------------------------------
# Each component is a self-contained CMake project that configures and builds on
# its own: `engine/`, `bridges/quickjs/`, and each `examples/<target>/`. None are
# add_subdirectory'd from here — open or configure whichever component you're
# working on directly (see each folder's CMakeLists.txt). The bridge and examples
# pull the engine in themselves via a relative add_subdirectory fallback.
#
# This root file exists only for repo-wide developer utilities (the clang-format
# targets below). It deliberately builds none of the components, so an IDE rooted
# here stays neutral and you load each subproject explicitly.
# ---------------------------------------------------------------------------

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

    # Exclude generated files
    list(FILTER FORMAT_SOURCES EXCLUDE REGEX ".*/font_data\\.c$")

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