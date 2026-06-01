/*
 * QuickJS bridge smoke test — proves the Flow A toolchain works end to end:
 * QuickJS compiles + links against the engine, a JS program drives the NativeUI
 * surface to build a scene graph, console.log reaches stdout, and er_commit()
 * runs a real layout + paint pass.
 *
 * Headless: a no-op backend is installed so the font registry initialises and
 * the paint path executes without a window. Verification reads back the layout
 * pass count and the repainted dirty rectangle (BRIDGE.md §0). The SDL-window
 * milestone is folded into examples/linux later.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "quickjs.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SMOKE_SCREEN_W 240
#define SMOKE_SCREEN_H 240

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — no-op backend
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief No-op fill callback. @param argb Color. @param x X. @param y Y. @param w Width. @param h Height. @param ctx
 * Context. */
static void noop_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op copy callback. @param src Source. @param stride Stride. @param x X. @param y Y. @param w Width. @param
 * h Height. @param ctx Context. */
static void noop_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op blend callback. @param src Source. @param stride Stride. @param a Alpha. @param x X. @param y Y. @param
 * w Width. @param h Height. @param ctx Context. */
static void noop_blend(const void* src, int stride, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

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

    for (int i = 0; i < argc; i++)
    {
        const char* str = JS_ToCString(ctx, argv[i]);
        if (!str)
        {
            return JS_EXCEPTION;
        }
        printf("%s%s", i ? " " : "", str);
        JS_FreeCString(ctx, str);
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
    static const EmbeddedRenderBackend backend = {noop_fill, noop_copy, noop_blend, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    smoke_install_console(ctx);
    er_bridge_install(ctx);

    static const char* src =
        "console.log('hello from QuickJS', 1 + 2);\n"
        "console.log('typeof NativeUI =', typeof NativeUI);\n"
        "const root = NativeUI.createNode('View');\n"
        "NativeUI.setProps(root, { width: 200, height: 120, flexDirection: 'row', flexWrap: 'wrap',\n"
        "                          padding: 8, backgroundColor: '#202020' });\n"
        "const box = NativeUI.createNode('View');\n"
        "NativeUI.setProps(box, { width: 50, height: 50, backgroundColor: '#e94560', borderRadius: 8,\n"
        "                         borderWidth: 2, borderColor: 'white', borderStyle: 'dashed',\n"
        "                         transform: [{ translateX: 4 }, { translateY: 2 }] });\n"
        "NativeUI.appendChild(root, box);\n"
        "const label = NativeUI.createNode('Text');\n"
        "NativeUI.setProps(label, { text: 'Hi', color: 'white', fontSize: 16, marginLeft: 12,\n"
        "                           fontStyle: 'italic', textDecorationLine: 'underline' });\n"
        "NativeUI.appendChild(root, label);\n"
        "// Exercise object-valued + component-specific marshalling (detached, then freed).\n"
        "const sw = NativeUI.createNode('Switch');\n"
        "NativeUI.setProps(sw, { value: true, trackColor: { false: '#767577', true: '#81b0ff' },\n"
        "                        thumbColor: 'white' });\n"
        "const card = NativeUI.createNode('View');\n"
        "NativeUI.setProps(card, { width: 40, height: 40, shadowColor: '#000', shadowOpacity: 0.5,\n"
        "                          shadowOffset: { width: 2, height: 3 }, shadowRadius: 4 });\n"
        "NativeUI.destroyNode(sw);\n"
        "NativeUI.destroyNode(card);\n"
        "NativeUI.setRoot(root);\n"
        "NativeUI.commit();\n"
        "console.log('handles:', root, box, label, 'now =', NativeUI.now());\n";

    JSValue result = JS_Eval(ctx, src, strlen(src), "<smoke>", JS_EVAL_TYPE_GLOBAL);

    int status = 0;
    if (JS_IsException(result))
    {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "JS exception: %s\n", msg ? msg : "(unknown)");
        if (msg)
        {
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
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
        printf("dirty rect: x=%d y=%d w=%d h=%d painted=%d\n", dirty.x, dirty.y, dirty.w, dirty.h, painted);

        if (passes == 0)
        {
            fprintf(stderr, "FAIL: no layout pass ran\n");
            status = 1;
        }
        else if (!painted || dirty.w <= 0 || dirty.h <= 0)
        {
            fprintf(stderr, "FAIL: commit repainted nothing\n");
            status = 1;
        }
        else if (dirty.w > SMOKE_SCREEN_W || dirty.h > SMOKE_SCREEN_H)
        {
            fprintf(stderr, "FAIL: dirty rect exceeds screen bounds\n");
            status = 1;
        }
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf(status == 0 ? "SMOKE OK\n" : "SMOKE FAIL\n");
    return status;
}
