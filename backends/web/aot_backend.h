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
 * The Flow B simulator's exports: what its host calls to load a package, run frames and read the panel.
 */

#ifndef ER_WEB_AOT_BACKEND_H
#define ER_WEB_AOT_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Makes the panel. Call it once, before anything else.
     *
     * @param[in] screen_width   The app's width in pixels.
     * @param[in] screen_height  The app's height in pixels.
     *
     * @return 1 when made.
     */
    int er_web_aot_init(int screen_width, int screen_height);

    /**
     * @brief Loads a package in place of the running app. One that is refused leaves the running app as it was.
     *
     * @param[in] package        The package's bytes; copied.
     * @param[in] length         How many.
     * @param[in] wall_clock_ms  The time, as milliseconds since the epoch.
     *
     * @return 1 when the new app runs; 0 with er_web_aot_error() saying why not.
     */
    int er_web_aot_load(const uint8_t* package, int length, double wall_clock_ms);

    /** @brief Why the last load was refused, or an empty string. */
    const char* er_web_aot_error(void);

    /** @brief 1 while an app runs: one is loaded and hasn't faulted. */
    int er_web_aot_running(void);

    /**
     * @brief Gives the panel and the app a new size. An app whose layout is for another size starts again.
     *
     * @return 1 when the panel has the size.
     */
    int er_web_aot_resize(int screen_width, int screen_height);

    /**
     * @brief Runs one frame: the engine's clock, the app's timers, a commit and the panel's conversion to RGBA.
     *
     * @param[in] delta_ms  Milliseconds since the last frame.
     *
     * @return How many rows of the panel changed, from er_web_aot_dirty_top().
     */
    int er_web_aot_pump(int delta_ms);

    /** @brief A touch: phase 0 is down, 1 a move and 2 up, at a place on the panel. */
    void er_web_aot_touch(int phase, int x, int y);

    /** @brief The panel as RGBA bytes, a row after a row. */
    const uint8_t* er_web_aot_framebuffer(void);

    /** @brief The first row the last pump changed, or -1 when it changed none. */
    int er_web_aot_dirty_top(void);

    /** @brief The runtime's descriptor (ErbDebug), which the host's debugger reads the app through. */
    const void* er_web_aot_debug(void);

#ifdef __cplusplus
}
#endif

#endif /* ER_WEB_AOT_BACKEND_H */
