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

# Banded rendering (LCD-agnostic): render in horizontal strips through a small band buffer instead of a
# full persistent framebuffer. Mirrors the no-PSRAM device path on desktop so it can be verified here.
# Enable with -DER_LCD_BANDED=ON (optionally -DER_LCD_BANDED_ROWS=<n>).
if (ER_LCD_BANDED)
    target_compile_definitions(er-backend-sdl PRIVATE ER_LCD_BANDED=1)
    if (DEFINED ER_LCD_BANDED_ROWS)
        target_compile_definitions(er-backend-sdl PRIVATE ER_LCD_BANDED_ROWS=${ER_LCD_BANDED_ROWS})
    endif()
endif()
