#ifndef EMBEDDED_REACT_NATIVE_RENDERER_H
#define EMBEDDED_REACT_NATIVE_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmbeddedRenderBackend {
    void (*fill_rect)(uint32_t argb, int x, int y, int w, int h, void *ctx);
    void (*copy_rect)(const void *src, int src_stride_bytes,
                      int x, int y, int w, int h, void *ctx);
    void (*blend_rect)(const void *src, int src_stride_bytes, uint8_t alpha,
                       int x, int y, int w, int h, void *ctx);
    void (*wait)(void *ctx);
    void (*frame_ready)(void *ctx);
    void *ctx;
} EmbeddedRenderBackend;

void embedded_renderer_set_backend(const EmbeddedRenderBackend *backend);
void embedded_renderer_tick(uint32_t delta_ms);

typedef enum {
    ER_TOUCH_DOWN,
    ER_TOUCH_MOVE,
    ER_TOUCH_UP,
    ER_TOUCH_CANCEL
} ERTouchPhase;

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
