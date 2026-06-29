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

#ifndef ER_RUNTIME_H
#define ER_RUNTIME_H

/*
 * er_runtime — the portable QuickJS host core for Flow A.
 *
 * Wraps the lifecycle every embedded-react host shares: create a QuickJS runtime + context, install
 * the NativeUI bridge and host globals (console, screen, optional persist), evaluate an app bundle
 * (bytecode or source), pump the job queue/timers, reset for a reload, and report errors. It is
 * backend-agnostic and platform-neutral (no SDL, no ESP-IDF, no filesystem) — the caller owns the
 * window/display backend, where the app bytes come from, and the frame loop.
 *
 * This is what lets embedded-react drop into any firmware with a few lines of glue:
 *
 *     ErRuntimeConfig cfg = { .screen_width = W, .screen_height = H, .log = my_log };
 *     er_runtime_init(&cfg);
 *     // (optional) install custom device-API globals on er_runtime_context()
 *     er_register_assets();                 // baked images/fonts (or load a pack)
 *     er_runtime_load_bytecode(qbc, len);   // your app — a pointer into flash/RAM
 *     for (;;) { er_runtime_pump(); er_commit(); my_present(); embedded_renderer_tick(dt); }
 *
 * The engine + bridge are single-instance (static state), so this runtime is a singleton too.
 */

#include "quickjs.h"

#include <stdbool.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Configuration for er_runtime_init (and re-applied on er_runtime_reset). */
typedef struct
{
    int screen_width;                          /**< `screen.width` exposed to JS (framebuffer pixels). */
    int screen_height;                         /**< `screen.height`. */
    float screen_scale;                        /**< `screen.scale` (DPI); <= 0 is treated as 1.0. */
    bool install_persist;                      /**< Expose `__erPersist` so usePersistentState survives a reload
                                                    (dev/simulator). Off on devices, where it falls back to useState. */
    void (*log)(const char* line);             /**< console.log/warn/error + error sink (one line, no newline).
                                                    NULL → stdout. On a device, point this at your UART logger. */
    const JSMallocFunctions* malloc_functions; /**< Custom JS-heap allocator, or NULL for the QuickJS
                                        default. On an MCU with external RAM, point the JS heap at PSRAM
                                        (JS_NewRuntime2). Must outlive the runtime. */
    size_t max_stack_size;                     /**< JS_SetMaxStackSize value (stack-overflow guard); 0 leaves the
                                                QuickJS default. Set below the host/task stack on an MCU. */
} ErRuntimeConfig;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Creates the QuickJS runtime + context and installs the NativeUI bridge and host globals.
 *
 * Call once after the render backend is set. Does not load an app — call er_runtime_load_* next.
 *
 * @param[in] cfg  Runtime configuration (copied; re-applied on reset).
 *
 * @return true on success; false if the runtime/context could not be created.
 */
bool er_runtime_init(const ErRuntimeConfig* cfg);

/**
 * @brief Returns the live QuickJS context, for installing custom globals (e.g. device-API bindings).
 *
 * Valid after er_runtime_init; the pointer changes across er_runtime_reset, so re-install any custom
 * globals after a reset.
 *
 * @return The current JSContext, or NULL before init / after shutdown.
 */
JSContext* er_runtime_context(void);

/** @brief Result of er_runtime_load_container (see er_runtime_container_status_str). */
typedef enum
{
    ER_CONTAINER_OK = 0,      /**< Loaded and the app ran. */
    ER_CONTAINER_BAD_MAGIC,   /**< Not an ERCF container. */
    ER_CONTAINER_BAD_VERSION, /**< Unsupported container format version (or malformed structure). */
    ER_CONTAINER_BAD_CRC,     /**< Integrity check failed — corrupt or partially written. */
    ER_CONTAINER_BAD_QJS,     /**< Built for a different QuickJS version than this firmware. */
    ER_CONTAINER_LOAD_FAILED, /**< No bytecode section, or the app threw on evaluation. */
} ErContainerStatus;

/**
 * @brief Loads an ERCF config container: verifies its CRC + QuickJS version, registers its asset pack,
 *        then evaluates its bytecode (and pumps).
 *
 * The universal "load a config" path — desktop, embedded bin, or a flash/config region on a device.
 * The buffer is referenced (image pixels / font bitmaps point into it) and must outlive use. On any
 * failure the engine is left as it was (call er_runtime_reset first if replacing a running app).
 *
 * @p len may be an UPPER BOUND, not the exact byte count: the loader walks the container's
 * self-describing sections (bounds-checked against @p len) to find its true end, then verifies the CRC
 * over exactly that payload. So you can pass the full size of a memory-mapped flash/config partition
 * without knowing the stored container's length.
 *
 * @param[in] buf  Container bytes (ERCF; caller-owned, must stay live — e.g. a partition mmap).
 * @param[in] len  Buffer length, or an upper bound (e.g. the config partition size).
 *
 * @return ER_CONTAINER_OK, or a specific failure the caller can log/show.
 */
ErContainerStatus er_runtime_load_container(const void* buf, size_t len);

/**
 * @brief Like er_runtime_load_container, but optionally copies the asset pack so @p buf need not stay live.
 *
 * Same as er_runtime_load_container when @p copy_assets is false (the bytes are referenced in place — use
 * that for a config mmap'd from flash or a kept file buffer; it costs no RAM for the asset pixels). Pass
 * true when @p buf is VOLATILE — a hot-reload staging buffer you want to reuse for the next upload: the
 * asset pack is copied into a persistent (reused) block and the engine is pointed at the copy, so @p buf
 * is free the moment this returns. The bytecode is always consumed in place during the call, so it never
 * needs to outlive it either way.
 *
 * @param[in] buf          Container bytes (ERCF).
 * @param[in] len          Buffer length, or an upper bound.
 * @param[in] copy_assets  true → own the asset bytes (volatile source); false → reference in place.
 *
 * @return ER_CONTAINER_OK, or a specific failure the caller can log/show.
 */
ErContainerStatus er_runtime_load_container_ex(const void* buf, size_t len, bool copy_assets);

/** @brief Returns a short human-readable string for an ErContainerStatus (for logging). */
const char* er_runtime_container_status_str(ErContainerStatus status);

/**
 * @brief Evaluates an app from a compiled QuickJS bytecode blob (JS_ReadObject), then pumps once.
 *
 * @param[in] buf  Bytecode bytes (caller-owned; may point into flash).
 * @param[in] len  Byte length.
 *
 * @return true on clean evaluation; false if a JS exception propagated (see er_runtime_last_error).
 */
bool er_runtime_load_bytecode(const void* buf, size_t len);

/**
 * @brief Compiles JS source to a QuickJS bytecode blob (the build-time `qjsc` step, without the native
 *        tool). Uses a throwaway runtime so it never touches the app runtime; the bytecode targets the
 *        same QuickJS version `er_runtime_load_bytecode` expects. COMPILE_ONLY — the bundle's references
 *        to NativeUI/screen/console are resolved only at load time, not here.
 *
 * @param[in]  src      Source text (UTF-8; need not be NUL-terminated).
 * @param[in]  len      Byte length of @p src.
 * @param[out] out_len  Receives the bytecode byte length (0 on failure).
 *
 * @return malloc'd bytecode buffer the caller must release with er_runtime_free(), or NULL on a compile
 *         error (see er_runtime_last_error).
 */
uint8_t* er_runtime_compile_bytecode(const char* src, size_t len, size_t* out_len);

/** @brief Frees a buffer returned by er_runtime_compile_bytecode (plain free; safe on NULL). */
void er_runtime_free(void* p);

/**
 * @brief Evaluates an app from JS source text, then pumps once.
 *
 * @param[in] src   Source text.
 * @param[in] len   Byte length.
 * @param[in] name  Display name for stack traces (NULL → "<app>").
 *
 * @return true on clean evaluation; false if a JS exception propagated.
 */
bool er_runtime_load_source(const char* src, size_t len, const char* name);

/**
 * @brief Drains the QuickJS job queue (Promises/microtasks) and fires due timers.
 *
 * Call once per frame before committing so async/timer-driven state lands in the frame.
 */
void er_runtime_pump(void);

/**
 * @brief Tears the runtime back to a clean slate for a reload: frees the JS context, resets the engine
 *        scene (er_reset), and brings up a fresh context with the bridge + globals reinstalled.
 *
 * Keeps the render backend and registered images/fonts. After this, re-install any custom globals and
 * call er_runtime_load_* to run the new app. Component state is not preserved (use usePersistentState
 * + install_persist for that).
 *
 * @return true on success.
 */
bool er_runtime_reset(void);

/**
 * @brief Returns the last uncaught JS error (message + stack), or "" if none.
 *
 * @return A read-only string owned by the runtime.
 */
const char* er_runtime_last_error(void);

/**
 * @brief Renders a full-screen red message panel (RN-redbox) into the current scene.
 *
 * Resets the engine scene and builds a `title` / `body` / optional `hint` screen via the bridge; the
 * caller's next commit paints it. Requires a live context + a render backend. The portable building
 * block for both JS-error and config-load-failure screens — a device shows the same panel as desktop.
 *
 * @param[in] title  Heading (NULL → "Error").
 * @param[in] body   Detail text (NULL → empty).
 * @param[in] hint   Optional smaller hint line below the body (NULL/empty → omitted).
 */
void er_runtime_show_message(const char* title, const char* body, const char* hint);

/**
 * @brief Renders an on-screen error overlay (RN-redbox) from the last JS error into the current scene.
 *
 * Convenience wrapper over er_runtime_show_message for the "JS error" case. Useful after a failed
 * load/reload.
 */
void er_runtime_show_error(void);

/**
 * @brief Clears the persisted-state store backing usePersistentState (no-op if persistence is off).
 */
void er_runtime_clear_persist(void);

/**
 * @brief Frees the QuickJS context and runtime. The engine/backend are left untouched.
 */
void er_runtime_shutdown(void);

#endif
