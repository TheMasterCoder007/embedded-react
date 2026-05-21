#include "native_renderer.h"

static const EmbeddedRenderBackend *g_backend = NULL;

void embedded_renderer_set_backend(const EmbeddedRenderBackend *backend) {
    g_backend = backend;
}

void embedded_renderer_tick(uint32_t delta_ms) {
    (void)delta_ms;
}

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y) {
    (void)finger_id; (void)phase; (void)x; (void)y;
}
