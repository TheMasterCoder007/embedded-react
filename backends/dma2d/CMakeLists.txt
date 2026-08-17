# STM32 DMA2D (Chrom-ART) render backend for embedded-react.
# Requires the `embedded-react` engine target (add the engine first).

add_library(er-backend-dma2d STATIC renderer_backend.c)

target_include_directories(er-backend-dma2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(er-backend-dma2d PUBLIC embedded-react)
