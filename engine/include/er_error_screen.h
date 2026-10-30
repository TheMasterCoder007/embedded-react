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

#ifndef EMBEDDED_REACT_ER_ERROR_SCREEN_H
#define EMBEDDED_REACT_ER_ERROR_SCREEN_H

/*
 * The error screen: one full-screen panel for every failure an app can cause, in both flows.
 *
 * It draws with fills and the built-in font straight through the render backend, with no scene tree and
 * no allocation. While it is up, er_commit() paints it in place of the scene and touch input is dropped.
 *
 * A fault in native code (an engine or firmware bug, a watchdog reset) can't draw anything when it
 * happens. The firmware records it in RAM that survives the reset (ERFaultRecord) and shows it at the
 * next boot.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Configuration
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Longest title, reason, amounts, location or hint the screen keeps, with its NUL. */
#ifndef ERUI_ERROR_SCREEN_LINE_BYTES
#define ERUI_ERROR_SCREEN_LINE_BYTES 128
#endif

/** @brief Longest detail (a stack trace, say) the screen keeps, with its NUL. */
#ifndef ERUI_ERROR_SCREEN_DETAIL_BYTES
#define ERUI_ERROR_SCREEN_DETAIL_BYTES 512
#endif

/** @brief Longest reason a fault record keeps, with its NUL. */
#define ER_FAULT_REASON_BYTES 48

    /*------------------------------------------------------------------------------------------------------------------
     - Types
     -----------------------------------------------------------------------------------------------------------------*/

    /** @brief What the error screen shows, top to bottom. Every field may be NULL. */
    typedef struct
    {
        const char* title;    /**< The heading; NULL shows "Error". */
        const char* reason;   /**< Why, in a few words. */
        const char* amounts;  /**< Needed and available amounts: "needs 41,210 B, firmware reserves 32,768 B". */
        const char* location; /**< Where in the source: "App.jsx:41". */
        const char* hint;     /**< What to do about it. */
        const char* detail;   /**< More for a developer, such as a stack trace. Cut short when it doesn't fit. */
    } ERErrorScreen;

    /**
     * @brief A native fault, kept in RAM the firmware leaves uninitialised so it survives a reset.
     *
     * The firmware places one in its no-init section (pico-sdk's __uninitialized_ram, ESP-IDF's
     * __NOINIT_ATTR), fills it from its fault handler with er_fault_record(), and shows it at the next boot
     * with er_fault_show_recorded(). A check word tells a real record from power-on garbage.
     */
    typedef struct
    {
        uint32_t magic;
        uint32_t pc;      /**< Where the fault happened, or 0. */
        uint32_t address; /**< The address it touched, or 0. */
        char reason[ER_FAULT_REASON_BYTES];
        uint32_t check;
    } ERFaultRecord;

    /*------------------------------------------------------------------------------------------------------------------
     - Functions
     -----------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Puts the error screen up. The text is copied, so the caller's strings needn't outlive the call.
     *
     * The next er_commit() paints it over the whole screen. The scene, if any, is kept but not drawn.
     *
     * @param[in] screen_w  The panel width in pixels.
     * @param[in] screen_h  The panel height in pixels.
     * @param[in] screen    What to show.
     */
    void er_error_screen_show(int screen_w, int screen_h, const ERErrorScreen* screen);

    /** @brief Takes the error screen down; the next er_commit() repaints the whole scene. */
    void er_error_screen_hide(void);

    /** @brief True while the error screen is up. */
    bool er_error_screen_active(void);

    /**
     * @brief Records a native fault. Safe from a fault handler: no allocation, no library calls.
     *
     * @param[out] record   The no-init record.
     * @param[in]  reason   What happened, such as "hard fault" or "watchdog reset"; cut to fit.
     * @param[in]  pc       Where it happened, or 0.
     * @param[in]  address  The address it touched, or 0.
     */
    void er_fault_record(ERFaultRecord* record, const char* reason, uint32_t pc, uint32_t address);

    /** @brief True when @p record holds a fault recorded before the last reset. */
    bool er_fault_recorded(const ERFaultRecord* record);

    /**
     * @brief Shows a recorded fault on the error screen and clears the record. Call once at boot.
     *
     * @param[in,out] record    The no-init record.
     * @param[in]     screen_w  The panel width in pixels.
     * @param[in]     screen_h  The panel height in pixels.
     *
     * @return true when there was a fault to show.
     */
    bool er_fault_show_recorded(ERFaultRecord* record, int screen_w, int screen_h);

#ifdef __cplusplus
}
#endif

#endif
