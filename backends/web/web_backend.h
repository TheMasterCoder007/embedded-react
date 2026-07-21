/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EMBEDDED_REACT_WEB_BACKEND_H
#define EMBEDDED_REACT_WEB_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * WASM simulator present layer + host ABI.
     *
     * This is the C side of the browser simulator. The engine renders through backends/software into an
     * ARGB8888 framebuffer; this layer converts that to canvas-ready RGBA and exposes a small flat ABI the
     * host page drives via cwrap. See tools/web-sim/README.md for the full design.
     *
     * Phase W2 runs a real Flow A bundle (QuickJS-in-WASM) via er_web_load_source on top of the portable
     * er_runtime host core. Asset packs (er_web_load_pack) and esbuild-watch hot reload arrive in W3.
     */

    /**
     * @brief Brings up the software backend and allocates the RGBA present buffer.
     *
     * @param[in] screen_w  Board width in pixels (> 0).
     * @param[in] screen_h  Board height in pixels (> 0).
     *
     * @return 1 on success, 0 on failure.
     */
    int er_web_init(int screen_w, int screen_h);

    /**
     * @brief Builds a static demo scene (W1 fallback when no app bundle has been loaded).
     *
     * Safe to call once after er_web_init(). In W2+ the host normally calls er_web_load_source() instead.
     */
    void er_web_demo_scene(void);

    /**
     * @brief Evaluates a Flow A app bundle (esbuild output) in the QuickJS runtime, replacing any running
     *        app.
     *
     * A private copy is kept so er_web_reset() (hot reload) and er_web_resize() can re-run it. On a JS error
     * an on-screen redbox is shown. The bytes are UTF-8 source text, not bytecode.
     *
     * @param[in] js   App bundle source (UTF-8; not required to be NUL-terminated).
     * @param[in] len  Byte length of @p js.
     */
    void er_web_load_source(const char* js, int len);

    /**
     * @brief Registers a baked ERPK asset pack (images + fonts) so the app's <Image>/custom-font nodes
     *        resolve.
     *
     * Call before er_web_load_source on first load, and again (followed by er_web_reset) when assets change
     * during a hot reload. A private copy of the pack is kept alive because the engine references its pixels +
     * glyph bitmaps by pointer.
     *
     * @param[in] buf  ERPK pack bytes (as produced by the JS asset baker).
     * @param[in] len  Byte length of @p buf.
     *
     * @return 1 on success, 0 on a malformed pack or allocation failure.
     */
    int er_web_load_pack(const uint8_t* buf, int len);

    /**
     * @brief Build-time `qjsc`: compiles a JS app bundle to a QuickJS bytecode blob using the QuickJS already
     *        embedded in this module — so the consumer `embedded-react build` needs no native toolchain.
     *
     * Uses a throwaway runtime (independent of any running app). The caller reads @p *out_len bytes from the
     * returned pointer and frees it with `Module._free`.
     *
     * @param[in]  src      App bundle source (UTF-8).
     * @param[in]  len      Byte length of @p src.
     * @param[out] out_len  Receives the bytecode byte length (0 on a compile error → returns NULL).
     * @param[in]  strip    Non-zero drops the embedded source text + debug tables (release/device:
     *                      ~8x smaller blob); 0 keeps them so dev stack traces have line numbers.
     *
     * @return Pointer to the bytecode in wasm memory (caller frees), or NULL on error.
     */
    const uint8_t* er_web_compile_bytecode(const char* src, int len, int* out_len, int strip);

    /**
     * @brief Changes the board size at runtime and rebuilds the scene to fit (responsive preview).
     *
     * Resizes the framebuffer + present buffer, resets the engine pools, and rebuilds the current scene at
     * the new dimensions. In W1 that scene is er_web_demo_scene(); in W2+ the loaded app re-runs with the
     * new screen size (the on-device "responsive layout" path). The host must resize its canvas to match
     * (er_web_fb_width/height).
     *
     * @param[in] screen_w  New board width in pixels (> 0).
     * @param[in] screen_h  New board height in pixels (> 0).
     *
     * @return 1 on success, 0 on failure (the previous size is kept).
     */
    int er_web_resize(int screen_w, int screen_h);

    /**
     * @brief Advances the engine one frame and refreshes the RGBA present buffer.
     *
     * Runs a commit + tick, then converts the software framebuffer (ARGB8888) to RGBA over the frame.
     *
     * @param[in] dt_ms  Milliseconds elapsed since the previous pump.
     */
    void er_web_pump(int dt_ms);

    /**
     * @brief Delivers a pointer event to the engine in framebuffer pixels.
     *
     * @param[in] phase  0 = down, 1 = move, 2 = up, 3 = cancel (ERTouchPhase order).
     * @param[in] x      X coordinate in framebuffer pixels.
     * @param[in] y      Y coordinate in framebuffer pixels.
     */
    void er_web_touch(int phase, int x, int y);

    /**
     * @brief Resets the simulator for a hot reload.
     *
     * W1: clears the framebuffer to opaque black. W2 wires this to er_runtime_reset() + bundle reload.
     */
    void er_web_reset(void);

    /**
     * @brief Returns the RGBA present buffer (screen_w * screen_h * 4 bytes, R,G,B,A order).
     *
     * The host wraps this as an ImageData and putImageData()s it to the canvas. Valid after er_web_init().
     *
     * @return Pointer to the RGBA buffer, or NULL before init.
     */
    const uint8_t* er_web_framebuffer(void);

    /**
     * @brief Returns the framebuffer width in pixels (0 before init).
     */
    int er_web_fb_width(void);

    /**
     * @brief Returns the framebuffer height in pixels (0 before init).
     */
    int er_web_fb_height(void);

#ifdef __cplusplus
}
#endif

#endif
