#ifndef EMBEDDED_REACT_NATIVE_RENDERER_H
#define EMBEDDED_REACT_NATIVE_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Platform-supplied rendering callbacks.
 *
 * The host application fills this struct and passes it to embedded_renderer_set_backend().
 * All function pointers are optional; NULL pointers are silently ignored by the blit helpers.
 */
typedef struct EmbeddedRenderBackend
{
    /** @brief Fill a solid-color rectangle. argb is straight-alpha ARGB8888. */
    void (*fill_rect)(uint32_t argb, int x, int y, int w, int h, void* ctx);

    /** @brief Copy a premultiplied ARGB8888 buffer into the framebuffer. */
    void (*copy_rect)(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx);

    /** @brief Blend a premultiplied ARGB8888 buffer into the framebuffer at the given global alpha. */
    void (*blend_rect)(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx);

    /** @brief Block until the hardware has finished consuming the last frame. May be NULL. */
    void (*wait)(void* ctx);

    /** @brief Signal that a new frame is ready for display. May be NULL. */
    void (*frame_ready)(void* ctx);

    /** @brief Opaque context pointer forwarded to every callback. */
    void* ctx;
} EmbeddedRenderBackend;

/**
 * @brief Phase of a touch event reported to the renderer.
 */
typedef enum
{
    ER_TOUCH_DOWN,   /**< Finger made contact with the screen. */
    ER_TOUCH_MOVE,   /**< Finger moved while in contact. */
    ER_TOUCH_UP,     /**< Finger lifted from the screen. */
    ER_TOUCH_CANCEL, /**< Touch sequence cancelled (e.g. system interrupt). */
} ERTouchPhase;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Registers a platform render backend and initialises the font subsystem.
 *
 * Must be called once before any rendering or font loading.
 *
 * @param[in] backend  Pointer to the backend descriptor to activate.
 */
void embedded_renderer_set_backend(const EmbeddedRenderBackend* backend);

/**
 * @brief Advances the renderer by one time step.
 *
 * Call this once per display refresh from the application main loop.
 *
 * @param[in] delta_ms  Milliseconds elapsed since the last call.
 */
void embedded_renderer_tick(uint32_t delta_ms);

/**
 * @brief Delivers a touch event to the renderer's input subsystem.
 *
 * @param[in] finger_id  Finger index (0 for single-touch devices).
 * @param[in] phase      Phase of the touch event.
 * @param[in] x          X coordinate of the touch point in framebuffer pixels.
 * @param[in] y          Y coordinate of the touch point in framebuffer pixels.
 */
void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
