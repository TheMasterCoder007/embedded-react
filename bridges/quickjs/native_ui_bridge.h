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

#ifdef __cplusplus
}
#endif

#endif
