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

#ifdef __cplusplus
}
#endif

#endif
