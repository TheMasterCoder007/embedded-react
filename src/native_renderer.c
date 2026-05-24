#include "font_blob.h"
#include "font_registry.h"
#include "native_renderer.h"
#include "renderer_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const EmbeddedRenderBackend* g_backend = NULL;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

const EmbeddedRenderBackend* er_backend(void)
{
    return g_backend;
}

void er_blit_fill(uint32_t argb, int x, int y, int w, int h)
{
    if (g_backend && g_backend->fill_rect)
        g_backend->fill_rect(argb, x, y, w, h, g_backend->ctx);
}

void er_blit_copy(const void* src, int stride, int x, int y, int w, int h)
{
    if (g_backend && g_backend->copy_rect)
        g_backend->copy_rect(src, stride, x, y, w, h, g_backend->ctx);
}

void er_blit_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h)
{
    if (g_backend && g_backend->blend_rect)
        g_backend->blend_rect(src, stride, alpha, x, y, w, h, g_backend->ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void embedded_renderer_set_backend(const EmbeddedRenderBackend* backend)
{
    g_backend = backend;
    font_registry_init();
    font_blob_init(0);
}

void embedded_renderer_tick(uint32_t delta_ms)
{
    (void)delta_ms;
}

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    (void)finger_id;
    (void)phase;
    (void)x;
    (void)y;
}
