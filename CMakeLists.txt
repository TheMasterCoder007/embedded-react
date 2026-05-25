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

if(BUILD_TESTING AND CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
endif()

add_subdirectory(engine)
