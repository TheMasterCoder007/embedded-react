find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# er-backend-sdl — SDL2 desktop render backend
# Requires SDL 2.0.6+ for SDL_ComposeCustomBlendMode.
# ---------------------------------------------------------------------------
add_library(er-backend-sdl STATIC renderer_backend.c)

target_link_libraries(er-backend-sdl
        PUBLIC
        embedded-react
        SDL2::SDL2
)

target_include_directories(er-backend-sdl
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
