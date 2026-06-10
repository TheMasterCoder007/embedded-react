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

        /*------------------------------------------------------------------------------------------------
         - Banded rendering (optional). A backend with no RAM for a full framebuffer keeps only a small
           band buffer of `band_height` rows and relies on the panel's own GRAM to retain the rest of the
           frame. When band_height > 0 the engine renders each commit's damage region as horizontal
           strips: for every strip it calls band_begin(), emits the (Y-translated) fill/copy/blend ops for
           that strip into the band buffer, then calls band_flush() to push it to the panel. The fill/copy/
           blend callbacks receive BAND-LOCAL Y (already offset by the strip's top), so the band buffer is
           a plain `screen_w x band_height` surface. Leave band_height = 0 (and these NULL) for the
           classic full-framebuffer path — the engine behaviour is then unchanged.
         ------------------------------------------------------------------------------------------------*/

        /** @brief Rows per band buffer; > 0 enables banded rendering. 0 = full-framebuffer mode. */
        int band_height;

        /** @brief Begin a strip [x,y,w,h] (screen space): clear the band buffer, remember the rect. */
        void (*band_begin)(int x, int y, int w, int h, void* ctx);

        /** @brief Flush the strip begun by band_begin() to the panel (convert + DMA). */
        void (*band_flush)(void* ctx);
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

/** @brief Keycode for the Backspace key. */
#define ER_KEY_BACKSPACE 8U
/** @brief Keycode for the Return / Enter key. */
#define ER_KEY_RETURN 13U
/** @brief Keycode for the Escape key. */
#define ER_KEY_ESCAPE 27U
/** @brief Keycode for the Delete key (forward-delete). */
#define ER_KEY_DELETE 127U

    /**
     * @brief Delivers a keyboard event to the currently focused TextInput node.
     *
     * When a TextInput is focused (via er_text_input_focus()), calling this function
     * inserts utf8_char into the text buffer (for printable characters), or processes
     * control codes such as ER_KEY_BACKSPACE and ER_KEY_RETURN.
     *
     * @param[in] keycode    Key code (use ER_KEY_* macros for control keys, or 0 for
     *                       pure UTF-8 character input).
     * @param[in] utf8_char  Null-terminated UTF-8 encoded character to insert, or NULL
     *                       for pure control keys (ER_KEY_BACKSPACE, ER_KEY_RETURN, …).
     */
    void embedded_renderer_key(uint32_t keycode, const char* utf8_char);

#ifdef __cplusplus
}
#endif

#endif
