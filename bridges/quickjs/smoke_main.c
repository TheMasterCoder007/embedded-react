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

/*
 * QuickJS bridge smoke test — proves the Flow A toolchain works end to end:
 * QuickJS compiles + links against the engine, a JS program drives the NativeUI
 * surface to build a scene graph, console.log reaches stdout, and er_commit()
 * runs a real layout + paint pass.
 *
 * Headless: a no-op backend is installed so the font registry initialises and
 * the paint path executes without a window. Verification reads back the layout
 * pass count and the repainted dirty rectangle. The SDL-window
 * milestone is folded into examples/linux later.
 */

#include "er_runtime.h" /* er_js_new_context — the device's lite intrinsic profile */
#include "er_scene.h"
#include "host_harness.h"
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "quickjs.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SMOKE_SCREEN_WIDTH 240
#define SMOKE_SCREEN_HEIGHT 240

/** @brief Centre of the scene's Pressable, which is laid out at 8,8 and 50x50. */
#define SMOKE_PRESSABLE_CENTRE 33

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — no-op backend
 ---------------------------------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — console shim
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Minimal console.log implementation that prints space-separated arguments to stdout.
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] this_val  JS `this` (unused).
 * @param[in] argc      Number of arguments.
 * @param[in] argv      Argument values.
 *
 * @return JS_UNDEFINED on success, JS_EXCEPTION if an argument cannot be stringified.
 */
static JSValue smoke_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;

    for (int argument_index = 0; argument_index < argc; argument_index++)
    {
        const char* text = JS_ToCString(ctx, argv[argument_index]);
        if (!text)
        {
            return JS_EXCEPTION;
        }
        printf("%s%s", argument_index ? " " : "", text);
        JS_FreeCString(ctx, text);
    }
    printf("\n");
    return JS_UNDEFINED;
}

/**
 * @brief Installs a minimal `console` object exposing log() onto the global scope.
 *
 * @param[in] ctx  QuickJS context to install console into.
 */
static void smoke_install_console(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, smoke_console_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Boots QuickJS, drives the NativeUI bridge to build + commit a scene, and verifies it.
 *
 * @return 0 when the JS evaluates cleanly and a layout + paint pass ran; 1 otherwise.
 */
int main(void)
{
    /* Install a no-op backend first: this initialises the font registry that text
       measurement relies on, and lets the paint path run without a window. */
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = er_js_new_context(rt, ER_JS_INTRINSIC_EVAL); /* lite profile + eval hook for JS_Eval */

    smoke_install_console(ctx);
    er_bridge_install(ctx);

    static const char* source =
        "console.log('hello from QuickJS', 1 + 2);\n"
        "console.log('typeof NativeUI =', typeof NativeUI);\n"
        "const root = NativeUI.createNode('View');\n"
        "NativeUI.setProps(root, { width: 200, height: 120, flexDirection: 'row', flexWrap: 'wrap',\n"
        "                          padding: 8, backgroundColor: '#202020' });\n"
        "globalThis.pressCount = 0;\n"
        "const capturedTag = 'CPU';\n" /* top-level const captured by the handler closure */
        "const box = NativeUI.createNode('Pressable');\n"
        "NativeUI.setProps(box, { width: 50, height: 50, backgroundColor: '#e94560', borderRadius: 8,\n"
        "                         borderWidth: 2, borderColor: 'white', borderStyle: 'dashed' });\n"
        "NativeUI.setEvent(box, 'onPress', (e) => {\n"
        "    globalThis.pressCount++;\n"
        "    NativeUI.setProps(label, { text: 'tapped', color: 'white', fontSize: 16 });\n" /* setProps from handler */
        "    globalThis.capturedTag = capturedTag;\n" /* only set if the setProps line above did not throw */
        "    console.log('onPress fired:', e.type, 'at', e.x + ',' + e.y, 'count', globalThis.pressCount);\n"
        "});\n"
        "NativeUI.appendChild(root, box);\n"
        "const label = NativeUI.createNode('Text');\n"
        "NativeUI.setProps(label, { text: 'Hi', color: 'white', fontSize: 16, marginLeft: 12,\n"
        "                           fontStyle: 'italic', textDecorationLine: 'underline' });\n"
        "NativeUI.appendChild(root, label);\n"
        "// Exercise object-valued + component-specific + transform marshalling (detached, then freed).\n"
        "const sw = NativeUI.createNode('Switch');\n"
        "NativeUI.setProps(sw, { value: true, trackColor: { false: '#767577', true: '#81b0ff' },\n"
        "                        thumbColor: 'white' });\n"
        "const card = NativeUI.createNode('View');\n"
        "NativeUI.setProps(card, { width: 40, height: 40, shadowColor: '#000', shadowOpacity: 0.5,\n"
        "                          shadowOffset: { width: 2, height: 3 }, shadowRadius: 4,\n"
        "                          transform: [{ translateX: 4 }, { rotate: '10deg' }] });\n"
        "NativeUI.destroyNode(sw);\n"
        "NativeUI.destroyNode(card);\n"
        "NativeUI.setRoot(root);\n"
        "NativeUI.commit();\n"
        "console.log('handles:', root, box, label, 'now =', NativeUI.now());\n";

    JSValue result = JS_Eval(ctx, source, strlen(source), "<smoke>", JS_EVAL_TYPE_GLOBAL);

    int status = 0;
    if (JS_IsException(result))
    {
        JSValue exception = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, exception);
        fprintf(stderr, "JS exception: %s\n", message ? message : "(unknown)");
        if (message)
        {
            JS_FreeCString(ctx, message);
        }
        JS_FreeValue(ctx, exception);
        status = 1;
    }
    JS_FreeValue(ctx, result);

    /* Headless verification: a layout pass must have run, and the commit must have
       repainted a non-empty region bounded by the screen. */
    if (status == 0)
    {
        const uint32_t passes = er_layout_pass_count();
        ERRect dirty = {0, 0, 0, 0};
        const bool painted = er_get_dirty_rect(&dirty);

        printf("layout passes: %u\n", passes);
        printf("dirty rect: x=%d y=%d w=%d h=%d painted=%d\n", dirty.x, dirty.y, dirty.width, dirty.height, painted);

        if (passes == 0)
        {
            fprintf(stderr, "FAIL: no layout pass ran\n");
            status = 1;
        }
        else if (!painted || dirty.width <= 0 || dirty.height <= 0)
        {
            fprintf(stderr, "FAIL: commit repainted nothing\n");
            status = 1;
        }
        else if (dirty.width > SMOKE_SCREEN_WIDTH || dirty.height > SMOKE_SCREEN_HEIGHT)
        {
            fprintf(stderr, "FAIL: dirty rect exceeds screen bounds\n");
            status = 1;
        }
    }

    /* Event round-trip: tap the centre of the Pressable and confirm
       the JS onPress handler ran by reading the global counter it increments. */
    if (status == 0)
    {
        embedded_renderer_touch(0, ER_TOUCH_DOWN, SMOKE_PRESSABLE_CENTRE, SMOKE_PRESSABLE_CENTRE);
        embedded_renderer_touch(0, ER_TOUCH_UP, SMOKE_PRESSABLE_CENTRE, SMOKE_PRESSABLE_CENTRE);

        JSValue press_global = JS_GetGlobalObject(ctx);
        JSValue press_count_value = JS_GetPropertyStr(ctx, press_global, "pressCount");
        int32_t press_count = 0;
        JS_ToInt32(ctx, &press_count, press_count_value);
        JS_FreeValue(ctx, press_count_value);
        JS_FreeValue(ctx, press_global);

        JSValue tag_global = JS_GetGlobalObject(ctx);
        JSValue tag_value = JS_GetPropertyStr(ctx, tag_global, "capturedTag");
        const char* tag = JS_ToCString(ctx, tag_value);
        printf("pressCount after tap: %d, capturedTag = %s\n", press_count, tag ? tag : "(unset)");
        const bool captured_ok = tag && strcmp(tag, "CPU") == 0;
        if (tag)
        {
            JS_FreeCString(ctx, tag);
        }
        JS_FreeValue(ctx, tag_value);
        JS_FreeValue(ctx, tag_global);

        if (press_count < 1)
        {
            fprintf(stderr, "FAIL: onPress did not round-trip to JS\n");
            status = 1;
        }
        else if (!captured_ok)
        {
            fprintf(stderr, "FAIL: handler closure could not read a top-level const\n");
            status = 1;
        }
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf(status == 0 ? "SMOKE OK\n" : "SMOKE FAIL\n");
    return status;
}
