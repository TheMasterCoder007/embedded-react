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

#ifndef ERB_RUNTIME_H
#define ERB_RUNTIME_H

/*
 * erb_runtime — loads, verifies and runs a Flow B package (website/docs/internals/flow-b-packages.md).
 *
 * erb_load() checks the whole package before it builds anything, then lays out the firmware's arena, runs
 * the boot function and builds the root template. After that the runtime runs the app's functions from
 * the engine's callbacks and from erb_tick(). Nothing here allocates: mutable app state lives in the arena
 * the firmware reserves, and the runtime keeps a few fixed statics of its own.
 *
 * Single-threaded: call everything from the thread that drives the engine.
 */

#include "er_assets.h"
#include "er_scene.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Configuration
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Instructions one run of an entry point may execute, unless ErbConfig::budget says otherwise. */
#ifndef ERB_EVENT_BUDGET
#define ERB_EVENT_BUDGET 100000u
#endif

/** @brief Most functions a package may have. Sizes the verifier's per-function scratch (8 bytes each). */
#ifndef ERB_MAX_FUNCTIONS
#define ERB_MAX_FUNCTIONS 256u
#endif

/** @brief Most words one function may have. Sizes the verifier's instruction map (1 bit each). */
#ifndef ERB_MAX_FUNCTION_WORDS
#define ERB_MAX_FUNCTION_WORDS 16384u
#endif

/** @brief Most loops one function may have. */
#ifndef ERB_MAX_LOOPS
#define ERB_MAX_LOOPS 32u
#endif

/** @brief Most native imports a package may have. */
#ifndef ERB_MAX_IMPORTS
#define ERB_MAX_IMPORTS 32u
#endif

/**
 * @brief Most images a package's asset pack may have: as many as the engine's image registry holds. Sizes the
 *        verifier's image name index (4 bytes each). It follows ERUI_IMAGE_REGISTRY_MAX; set that, not this.
 */
#ifdef ERUI_IMAGE_REGISTRY_MAX
#define ERB_MAX_IMAGES ERUI_IMAGE_REGISTRY_MAX
#else
#define ERB_MAX_IMAGES 128u /* the engine's default */
#endif

/** @brief Most keys a package's keyboard layout may have, in all its layers. Sizes the runtime's copy of the layout
 *         when the engine has its on-screen keyboard (12 bytes a key on a 32-bit MCU). */
#ifndef ERB_MAX_KEYBOARD_KEYS
#define ERB_MAX_KEYBOARD_KEYS 128u
#endif

/** @brief Most rows and layers a package's keyboard layout may have, in all. */
#define ERB_MAX_KEYBOARD_ROWS 32u
#define ERB_MAX_KEYBOARD_LAYERS 8u

/** @brief Deepest a template's tree may nest. */
#ifndef ERB_MAX_TEMPLATE_DEPTH
#define ERB_MAX_TEMPLATE_DEPTH 32u
#endif

/** @brief Places a limit is reported from before the runtime stops remembering which it reported. */
#ifndef ERB_REPORT_PLACES
#define ERB_REPORT_PLACES 32u
#endif

/** @brief Update passes one event may take before it is an update loop. */
#define ERB_MAX_UPDATE_PASSES 50u

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Why a load failed or an app stopped. erb_status_str() gives each one's reason. */
typedef enum
{
    ERB_OK = 0,
    /* Load */
    ERB_ERR_NO_PACKAGE,   /**< No package installed. */
    ERB_ERR_CORRUPT,      /**< Not a package, or corrupt. */
    ERB_ERR_NOT_FLOW_B,   /**< Not a Flow B package. */
    ERB_ERR_NEWER_FORMAT, /**< Built for a newer runtime. */
    ERB_ERR_ENGINE_ABI,   /**< Built for an engine ABI this firmware doesn't run. */
    ERB_ERR_NO_PANEL,     /**< No layout for the screen size. */
    ERB_ERR_TOO_BIG,      /**< Needs more than the firmware reserves. */
    ERB_ERR_TABLE,        /**< Malformed table. */
    ERB_ERR_STRING,       /**< Malformed string. */
    ERB_ERR_ARENA,        /**< Bad arena layout. */
    ERB_ERR_STATE,        /**< Bad state layout. */
    ERB_ERR_PROP,         /**< Bad prop. */
    ERB_ERR_TEMPLATE,     /**< Bad template. */
    ERB_ERR_BINDING,      /**< Bad binding. */
    ERB_ERR_ANIMATION,    /**< Bad animation. */
    ERB_ERR_TAPE,         /**< Bad vector tape. */
    ERB_ERR_FUNCTION,     /**< Bad function. */
    ERB_ERR_BYTECODE,     /**< Bad bytecode. */
    ERB_ERR_CONTROL_FLOW, /**< Bad control flow. */
    ERB_ERR_RECURSION,    /**< Recursive call. */
    ERB_ERR_STACK,        /**< Stack too deep. */
    ERB_ERR_IMPORT,       /**< Missing native module. */
    ERB_ERR_INPUT,        /**< Bad input. */
    ERB_ERR_DEBUG,        /**< Debug table does not match. */
    ERB_ERR_ASSETS,       /**< Asset pack failed to load. */
    /* Run */
    ERB_ERR_BUDGET,       /**< Instruction budget exceeded. */
    ERB_ERR_UPDATE_LOOP,  /**< More than ERB_MAX_UPDATE_PASSES update passes. */
    ERB_ERR_QUEUE_FULL,   /**< The event queue filled. */
    ERB_ERR_LIST_INDEX,   /**< A list read past its end. */
    ERB_ERR_TRAP,         /**< The app stopped itself (a TRAP). */
    ERB_ERR_NATIVE,       /**< A native import reported failure. */
    ERB_ERR_STALE_NODE,   /**< A node handle outlived its node (debug packages only). */
    ERB_ERR_OUT_OF_NODES, /**< The engine couldn't give a node. */
    ERB_STATUS_COUNT_,
} ErbStatus;

/** @brief ErbError::resource for ERB_ERR_TOO_BIG, besides the ERB_RES_ resource ids. */
#define ERB_RESOURCE_ARENA 0u
#define ERB_RESOURCE_NODES 0x100u

/** @brief A failure, with what is known about it. */
typedef struct
{
    ErbStatus status;
    const char* detail; /**< Which check failed, for a developer; static text. */
    uint32_t needed;    /**< TOO_BIG: amount needed. ENGINE_ABI: the package's level. NO_PANEL: width. */
    uint32_t available; /**< TOO_BIG: amount reserved. NO_PANEL: height. */
    uint16_t resource;  /**< TOO_BIG: ERB_RESOURCE_ARENA, ERB_RESOURCE_NODES or an ERB_RES_ id. */
    uint16_t function;  /**< The function running, or 0xFFFF. */
    uint32_t offset;    /**< Its word offset when it failed. */
    const char* name;   /**< IMPORT and NATIVE: the module. TRAP: its message. Else NULL. */
    const char* file;   /**< From the debug table: the source file, or NULL. */
    uint32_t line;      /**< From the debug table: the line, or 0. */
    uint32_t build_id;  /**< The package's build id, when its header was read. */
} ErbError;

/**
 * @brief A native function a package may import.
 *
 * @param[in]  args     Arguments as 32-bit cells, as the signature lists them.
 * @param[out] result   The result cell (unused when the signature returns nothing).
 * @param[in]  context  ErbConfig::context.
 *
 * @return false to stop the app with ERB_ERR_NATIVE, naming the module.
 */
typedef bool (*ErbNativeFn)(const int32_t* args, int32_t* result, void* context);

/**
 * @brief One native function the firmware offers. The signature is the result, ':', then the parameters,
 *        each one of i (int), b (bool) or f (float); v as the result means none. "i:if" takes an int and a
 *        float and returns an int.
 */
typedef struct
{
    const char* name;
    uint16_t version;
    const char* signature;
    ErbNativeFn callback;
} ErbNative;

/** @brief What the firmware gives the runtime. */
typedef struct
{
    void* arena;          /**< 4-byte aligned, for the app's mutable state. */
    uint32_t arena_bytes; /**< Its size. */
    uint16_t screen_w;    /**< The panel size; the package must declare a layout for it. */
    uint16_t screen_h;
    uint16_t node_count;      /**< Nodes the app may use; 0 means ERUI_MAX_NODES. */
    const ErbNative* natives; /**< Native functions packages may import. */
    uint16_t native_count;
    ErAssetsStorage* fonts; /**< Storage for the asset pack's font tables; NULL if packages have no fonts. */
    uint32_t budget;        /**< Instructions per run; 0 means ERB_EVENT_BUDGET. */
    void (*log)(const char* line, void* context);           /**< Reports, one line each; may be NULL. */
    void (*on_fault)(const ErbError* error, void* context); /**< NULL: erb_show_error(). */
    void* context;                                          /**< Passed to natives, log and on_fault. */
} ErbConfig;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies a package and, when it passes, replaces the running app with it.
 *
 * Nothing is built until every check passes, so a package that fails verification leaves a running app
 * exactly as it was. A failure after that (a fault in the boot function) leaves no app running.
 *
 * @param[in]  package        The container, 4-byte aligned, kept live while the app runs.
 * @param[in]  package_bytes  Its length, or an upper bound (a flash partition's size).
 * @param[in]  config         The firmware's side; copied.
 * @param[out] error          Receives the reason on failure; may be NULL.
 *
 * @return true when the app is running.
 */
bool erb_load(const void* package, size_t package_bytes, const ErbConfig* config, ErbError* error);

/** @brief Tears the running app down: timers, animations, nodes, registered assets and the arena. */
void erb_unload(void);

/** @brief Advances the app's timers. Call each frame, before er_commit(). */
void erb_tick(uint32_t elapsed_ms);

/** @brief True while an app is loaded and has not stopped with a fault. */
bool erb_running(void);

/** @brief The last load failure or fault; status ERB_OK when there is none. */
const ErbError* erb_last_error(void);

/**
 * @brief Looks a host input (useHostValue) up by name.
 *
 * @return Its index for the erb_input_set functions, or -1.
 */
int erb_input_find(const char* name);

/** @brief Sets a host input and runs the update. false when the index or type is wrong or no app runs. */
bool erb_input_set_int(int input, int32_t value);
bool erb_input_set_bool(int input, bool value);
bool erb_input_set_float(int input, float value);
bool erb_input_set_string(int input, const char* value);

/** @brief Sets the wall clock DATENOW reads, in ms since the epoch, and runs the update. */
void erb_set_wall_clock(int64_t epoch_ms);

/** @brief Replaces the scene with the error screen for @p error. Draws with no app state and no allocation. */
void erb_show_error(const ErbError* error);

/** @brief erb_show_error(), and what to do about it. @p hint may be NULL. */
void erb_show_error_hint(const ErbError* error, const char* hint);

/** @brief The short reason for a status. */
const char* erb_status_str(ErbStatus status);

/**
 * @brief Writes a one-line description of @p error: the reason, the amounts and where it happened.
 *
 * @return The length written (excluding the NUL), truncated to fit.
 */
size_t erb_error_message(const ErbError* error, char* buffer, size_t capacity);

#endif
