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

#ifndef ER_DESKTOP_HOST_H
#define ER_DESKTOP_HOST_H

/*
 * Desktop host core (SDL + QuickJS + engine bridge) — shared by the desktop demo (main.c) and the
 * simulator (see tools/simulator/README.md). The demo is a thin driver: er_host_start → er_host_load_config →
 * er_host_run → er_host_shutdown. The simulator drives the same core one frame at a time with
 * er_host_step (loading an explicit bundle via er_host_load_app) so it can interleave file-watching
 * + reload.
 */

#include <stdbool.h>
#include <stdint.h>

/* SDL types, forward-declared so this header doesn't pull in <SDL2/SDL.h>. */
struct SDL_Window;
struct SDL_Renderer;

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Window configuration passed to er_host_start. */
typedef struct
{
    const char* title; /**< Window title (NULL → a default). */
    int width;         /**< Logical window width in pixels. */
    int height;        /**< Logical window height in pixels. */
    bool persist;      /**< Install __erPersist so usePersistentState survives reload (simulator only). */
} ErHostConfig;

/**
 * @brief A running desktop host: the SDL window/renderer + the frame-loop bookkeeping. The QuickJS
 *        runtime/context lives in er_runtime (the portable host core this wraps).
 */
typedef struct
{
    struct SDL_Window* window;     /**< SDL window. */
    struct SDL_Renderer* renderer; /**< SDL renderer backing the engine's SDL backend. */
    int phys_w;                    /**< Framebuffer width in physical pixels. */
    int phys_h;                    /**< Framebuffer height in physical pixels. */
    float dpi_scale;               /**< Physical/logical pixel ratio (HiDPI input scaling). */
    bool running;                  /**< Cleared when quit (window close / ESC) is requested. */
    bool reload_requested;         /**< Set when the reload key (R) is pressed; the simulator acts on it. */
    uint32_t prev_ticks;           /**< SDL_GetTicks() at the previous frame, for the tick delta. */
} ErHost;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Brings up SDL + the SDL backend and a QuickJS context with console/screen/NativeUI
 *        installed. No assets are registered here — they ride inside the config container (demo) or a
 *        runtime pack (simulator). Fills @p host on success.
 *
 * @param[in]  cfg   Window configuration.
 * @param[out] host  Host state to initialize.
 *
 * @return true on success; false (with an SDL_Log) if any stage failed (partial state is torn down).
 */
bool er_host_start(const ErHostConfig* cfg, ErHost* host);

/**
 * @brief Loads the config container from the fixed "config slot" (app.erpkg next to the executable)
 *        and runs it — the desktop demo's device-parity boot path.
 *
 * On no config / a corrupt / version-incompatible one, paints an on-screen panel (via er_runtime) and
 * returns false; there is NO built-in fallback. Callers keep the window up so the panel stays visible,
 * exactly as firmware would. Mirrors how an MCU loads its config from a flash region.
 *
 * @param[in] host  Started host.
 *
 * @return true if a valid config loaded and ran; false otherwise (a panel was rendered).
 */
bool er_host_load_config(ErHost* host);

/**
 * @brief Loads, evaluates, and pumps an explicit app file: `.erpkg` = config container, `.qbc` =
 *        bytecode, else JS source. No fallback. Used by the simulator (watched source bundle) and for
 *        ad-hoc CLI overrides.
 *
 * On failure it renders the matching on-screen panel itself (a "Couldn't load config" panel for a bad
 * container, the JS-error redbox for a source/bytecode error), so the caller need not — the file
 * couldn't be read is the only silent failure.
 *
 * @param[in] host           Started host.
 * @param[in] explicit_path  App path (required; NULL returns false).
 *
 * @return true on clean evaluation; false if the file was unreadable or a JS/container load failed.
 */
bool er_host_load_app(ErHost* host, const char* explicit_path);

/**
 * @brief Live-reloads the app: frees the JS context, resets the engine to an empty scene, brings up a
 *        fresh context (bridge + globals reinstalled), and re-runs the app. Component state is not
 *        preserved. Registered images/fonts survive (baked assets keep resolving). The simulator
 *        calls this when the watched bundle changes. See tools/simulator/README.md.
 *
 * @param[in] host           Started host.
 * @param[in] explicit_path  App path to reload (typically the watched bundle).
 *
 * @return true on clean evaluation; false if a JS exception propagated (the host stays usable —
 *         the simulator keeps running and can reload again once the source is fixed).
 */
bool er_host_reload(ErHost* host, const char* explicit_path);

/**
 * @brief Renders an on-screen error overlay (RN-redbox style) from the last uncaught JS error.
 *
 * Call after er_host_load_app / er_host_reload returns false so the failure shows in the window
 * instead of leaving a blank or stale screen. The next successful reload replaces it.
 *
 * @param[in] host  Started host with a live context.
 */
void er_host_show_error(ErHost* host);

/**
 * @brief Clears the persisted-state store backing usePersistentState (simulator only).
 *
 * Call before a reload to drop preserved state and remount the app fresh (the simulator's manual
 * reload, R). A no-op when persistence was never enabled.
 */
void er_host_clear_persist(void);

/**
 * @brief Scales a logical SDL input coordinate to physical framebuffer pixels.
 *
 * @param[in] host     Started host.
 * @param[in] logical  Logical-pixel coordinate from an SDL event.
 *
 * @return Physical-pixel coordinate.
 */
int er_host_event_px(const ErHost* host, int logical);

/**
 * @brief Processes one frame: drains SDL events (input → engine touch, quit/ESC), pumps the JS job
 *        queue + timers, commits, presents, and advances the engine clock.
 *
 * @param[in] host  Started host with an app loaded.
 *
 * @return true to keep running; false once quit was requested.
 */
bool er_host_step(ErHost* host);

/**
 * @brief Runs er_host_step in a loop until quit. The one-shot driver used by the desktop demo.
 *
 * @param[in] host  Started host with an app loaded.
 */
void er_host_run(ErHost* host);

/**
 * @brief Headless render: settles a few frames, optionally injects taps (ER_TAPS="x,y x,y …"), then saves
 *        the framebuffer to a BMP. Used by the Flow A↔B parity harness to compare the two render paths.
 *
 * @param[in] host  Started host with an app loaded.
 * @param[in] path  Output BMP path.
 */
void er_host_screenshot(ErHost* host, const char* path);

/**
 * @brief Tears down the QuickJS context/runtime, the SDL backend, the renderer, and the window.
 *
 * @param[in] host  Host to shut down (safe to call after a partial start).
 */
void er_host_shutdown(ErHost* host);

#endif
