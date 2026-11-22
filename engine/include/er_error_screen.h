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
 * next boot. The record holds enough to trace the fault on the host: `embedded-react decode-fault`.
 */

#include <stdbool.h>
#include <stddef.h>
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

/** @brief Longest app source file name a fault record keeps, with its NUL. A longer one keeps its end. */
#define ER_FAULT_FILE_BYTES 32

/** @brief Words of the stack a fault record keeps, 4 bytes each: up to 255. */
#ifndef ER_FAULT_STACK_WORDS
#define ER_FAULT_STACK_WORDS 32
#endif
#if ER_FAULT_STACK_WORDS > 255
#error "ER_FAULT_STACK_WORDS is at most 255"
#endif

/** @brief Fault status registers a fault record keeps. */
#define ER_FAULT_STATUS_WORDS 4

/** @brief Raised when a field of ERFaultRecord changes. */
#define ER_FAULT_VERSION 1u

/** @brief ERFaultRecord::core: whose registers the status words are. */
#define ER_FAULT_CORE_NONE 0u
#define ER_FAULT_CORE_CORTEX_M 1u /**< CFSR, HFSR, MMFAR, BFAR; all 0 on a core without them. */
#define ER_FAULT_CORE_XTENSA 2u   /**< EXCCAUSE, EXCVADDR. */
#define ER_FAULT_CORE_RISCV 3u    /**< mcause, mtval. */

/** @brief ERFaultRecord::flags. */
#define ER_FAULT_FLAG_APP 0x01u     /**< The app was running: the app fields are set. */
#define ER_FAULT_FLAG_CALLERS 0x02u /**< The stack words are the callers' addresses, innermost first. */

/** @brief Room for er_fault_text()'s line, with its NUL. */
#define ER_FAULT_TEXT_BYTES (2u * sizeof(ERFaultRecord) + 1u)

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
     * __NOINIT_ATTR), fills it from its fault handler with the er_fault_record functions, and shows it at the
     * next boot with er_fault_show_recorded(). A check word tells a real record from power-on garbage.
     *
     * `embedded-react decode-fault` reads it as er_fault_text() writes it, so a field is only ever added with a
     * new ER_FAULT_VERSION.
     */
    typedef struct
    {
        uint32_t magic;
        uint8_t version;                        /**< ER_FAULT_VERSION. */
        uint8_t core;                           /**< An ER_FAULT_CORE_ value. */
        uint8_t flags;                          /**< ER_FAULT_FLAG_ bits. */
        uint8_t stack_count;                    /**< Words of stack kept. */
        uint32_t program_counter;               /**< Where the fault happened, or 0. */
        uint32_t link_register;                 /**< The return address the core held, or 0. */
        uint32_t stack_pointer;                 /**< The stack pointer before the fault: where the stack words start. */
        uint32_t address;                       /**< The address it touched, or 0. */
        uint32_t status[ER_FAULT_STATUS_WORDS]; /**< The core's fault status registers. */
        uint32_t app_offset;                    /**< The word offset in the app's function. */
        uint32_t app_build_id;                  /**< The package's build_id. */
        uint32_t app_line;                      /**< The app's source line, or 0 when the package has no debug table. */
        uint16_t app_function;                  /**< The app function running. */
        uint16_t app_panel;                     /**< The package layout running, by index. */
        char app_file[ER_FAULT_FILE_BYTES];     /**< The app's source file, or empty. */
        char reason[ER_FAULT_REASON_BYTES];
        uint32_t stack[ER_FAULT_STACK_WORDS];
        uint32_t check;
    } ERFaultRecord;

    /** @brief Where the app was when a fault came, for er_fault_record_app(). */
    typedef struct
    {
        uint16_t function; /**< The function running. */
        uint16_t panel;    /**< The package layout running, by index. */
        uint32_t offset;   /**< Its word offset. */
        uint32_t build_id; /**< The package's build_id. */
        const char* file;  /**< The source file from the debug table, or NULL. */
        uint32_t line;     /**< Its line, or 0. */
    } ERFaultApp;

    /*------------------------------------------------------------------------------------------------------------------
     - Functions
     -----------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Puts the error screen up. The text is copied, so the caller's strings needn't outlive the call.
     *
     * The next er_commit() paints it over the whole screen. The scene, if any, is kept but not drawn.
     *
     * @param[in] screen_width   The panel width in pixels.
     * @param[in] screen_height  The panel height in pixels.
     * @param[in] screen         What to show.
     */
    void er_error_screen_show(int screen_width, int screen_height, const ERErrorScreen* screen);

    /**
     * @brief Gives a showing error screen a new size, with the same text, after the display changes size. Does
     *        nothing when none is up.
     *
     * @param[in] screen_width   The new width in pixels.
     * @param[in] screen_height  The new height in pixels.
     */
    void er_error_screen_resize(int screen_width, int screen_height);

    /** @brief Takes the error screen down; the next er_commit() repaints the whole scene. */
    void er_error_screen_hide(void);

    /** @brief True while the error screen is up. */
    bool er_error_screen_active(void);

    /**
     * @brief Starts a record of a native fault, clearing what an earlier one held. Like the other er_fault_record
     *        functions it is safe from a fault handler: no allocation, no library calls. Each leaves the record
     *        complete, so a handler that faults part way keeps what it had.
     *
     * @param[out] record           The no-init record.
     * @param[in]  reason           What happened, such as "hard fault" or "watchdog reset"; cut to fit.
     * @param[in]  program_counter  Where it happened, or 0.
     * @param[in]  address          The address it touched, or 0.
     */
    void er_fault_record(ERFaultRecord* record, const char* reason, uint32_t program_counter, uint32_t address);

    /**
     * @brief Adds the core's registers to a record.
     *
     * @param[in,out] record         The record.
     * @param[in]     core           An ER_FAULT_CORE_ value: whose registers @p status holds.
     * @param[in]     link_register  The return address the core held, or 0.
     * @param[in]     stack_pointer  The stack pointer before the fault.
     * @param[in]     status         The fault status registers; may be NULL.
     * @param[in]     status_count   How many; those past ER_FAULT_STATUS_WORDS are dropped.
     */
    void er_fault_record_registers(ERFaultRecord* record,
                                   uint8_t core,
                                   uint32_t link_register,
                                   uint32_t stack_pointer,
                                   const uint32_t* status,
                                   unsigned status_count);

    /**
     * @brief Adds words of the stack to a record, for the host to rebuild the call chain from.
     *
     * @param[in,out] record   The record.
     * @param[in]     words    The words, from the stack pointer up. Every one must be readable.
     * @param[in]     count    How many; those past ER_FAULT_STACK_WORDS are dropped.
     * @param[in]     callers  true when they are the callers' addresses, innermost first, from the board's own
     *                         unwinder, not the stack as it lay.
     */
    void er_fault_record_stack(ERFaultRecord* record, const uint32_t* words, unsigned count, bool callers);

    /**
     * @brief Adds where the app was to a record. The Flow B runtime's erb_fault_record_app() calls it.
     *
     * @param[in,out] record  The record.
     * @param[in]     app     The app's place.
     */
    void er_fault_record_app(ERFaultRecord* record, const ERFaultApp* app);

    /**
     * @brief Writes a record as one line of hex, for a log or wherever the firmware reports faults.
     *        `embedded-react decode-fault` reads it back. Call it before er_fault_show_recorded(), which clears the
     *        record.
     *
     * @param[in]  record    The record.
     * @param[out] buffer    Receives the line.
     * @param[in]  capacity  Its size: ER_FAULT_TEXT_BYTES holds it all.
     *
     * @return The length written, or 0 when @p record holds no fault or the line doesn't fit.
     */
    size_t er_fault_text(const ERFaultRecord* record, char* buffer, size_t capacity);

    /** @brief True when @p record holds a fault recorded before the last reset. */
    bool er_fault_recorded(const ERFaultRecord* record);

    /**
     * @brief True when a record's address is one the fault touched. Zero is an address too, as a null pointer's
     *        is, so this goes by what the core reported: the address-valid bits on a Cortex-M, always on an
     *        Xtensa, and otherwise any address but zero.
     */
    bool er_fault_has_address(const ERFaultRecord* record);

    /** @brief Clears a record, so it no longer holds a fault. */
    void er_fault_clear(ERFaultRecord* record);

    /**
     * @brief Shows a recorded fault on the error screen and clears the record. Call once at boot.
     *
     * @param[in,out] record         The no-init record.
     * @param[in]     screen_width   The panel width in pixels.
     * @param[in]     screen_height  The panel height in pixels.
     *
     * @return true when there was a fault to show.
     */
    bool er_fault_show_recorded(ERFaultRecord* record, int screen_width, int screen_height);

#ifdef __cplusplus
}
#endif

#endif
