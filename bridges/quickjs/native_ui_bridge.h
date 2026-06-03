#ifndef EMBEDDED_REACT_NATIVE_UI_BRIDGE_H
#define EMBEDDED_REACT_NATIVE_UI_BRIDGE_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C"
{

#endif

    /*------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     -----------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Installs the NativeUI bridge object into a QuickJS global scope.
     *
     * Creates the global `NativeUI` object whose methods forward React host-config calls
     * (createNode, setProps, appendChild, commit, …) to the engine's er_scene.h API. Call
     * once per JSContext after the runtime is initialized and before evaluating any app
     * bundle. The method surface is fleshed out incrementally — see BRIDGE.md §1.
     *
     * @param[in] ctx  QuickJS context to install the bridge into.
     */
    void er_bridge_install(JSContext* ctx);

    /**
     * @brief Services the JS event loop for one host frame.
     *
     * Drains the QuickJS job queue (Promise reactions / microtasks) and fires any
     * `setTimeout`/`setInterval` callbacks whose deadline has passed on the engine clock
     * (`er_now_ms`). Call once per frame from the host loop, after advancing the clock with
     * `embedded_renderer_tick`. `NativeUI.tick()` performs the same pump for JS-driven loops
     * and tests, so a host that only ticks via JS need not call this directly.
     *
     * @param[in] ctx  Context the bridge was installed into (NULL is a no-op).
     */
    void er_bridge_pump(JSContext* ctx);

    /**
     * @brief Loads and runs a precompiled QuickJS bytecode blob.
     *
     * Reads the blob (produced by er-bridge-quickjs-compile) with JS_ReadObject and runs it with
     * JS_EvalFunction — the parser and source text never run, which is the point on MCU. The QuickJS
     * VM still interprets the bytecode (this is not the Flow B AOT compiler). The blob must come from
     * the SAME QuickJS build/version the host links. Only feed trusted input.
     *
     * @param[in] ctx  QuickJS context (bridge + host globals installed beforehand).
     * @param[in] buf  Bytecode bytes.
     * @param[in] len  Byte count.
     *
     * @return The evaluation result, or a JS exception value (check with JS_IsException). The caller
     *         owns the returned value and must JS_FreeValue it.
     */
    JSValue er_bridge_run_bytecode(JSContext* ctx, const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
