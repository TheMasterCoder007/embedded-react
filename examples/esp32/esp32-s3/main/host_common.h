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

#ifndef HOST_COMMON_H
#define HOST_COMMON_H

/* What the S3's Flow A and Flow B hosts share: the screen's rotation, the render worker and the error screen's hold. */

#include "board.h"

#include <stdbool.h>
#include <stdint.h>

/* Whole-screen rotation (clockwise): 0 / 90 / 180 / 270. The panel is physically BOARD_LCD_WIDTH ×
   BOARD_LCD_HEIGHT (800×480); for 90/270 the LOGICAL screen the app renders into is the swap (480×800,
   a portrait UI). The backend maps logical→physical at present; touch is remapped by the inverse. */
#ifndef ER_DISPLAY_ROTATION
#define ER_DISPLAY_ROTATION 0
#endif

/* Logical screen the app sizes its root from (`screen` global) — panel size, swapped for 90/270. */
#if (ER_DISPLAY_ROTATION == 90) || (ER_DISPLAY_ROTATION == 270)
#define SCREEN_W BOARD_LCD_HEIGHT
#define SCREEN_H BOARD_LCD_WIDTH
#else
#define SCREEN_W BOARD_LCD_WIDTH
#define SCREEN_H BOARD_LCD_HEIGHT
#endif

/* Target frame period for the adaptive pacer (~50 fps, just above the RGB panel's ~51 Hz refresh).
 * Light frames sleep the remainder up to this; heavy frames yield the minimum and run as fast as the
 * work allows. The real win needs fine ticks — see CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults. */
#define ER_TARGET_FRAME_MS 20U

/** @brief Remaps a physical (panel) touch point to logical (app) coords by the inverse rotation. */
void remap_touch(int* x, int* y);

/**
 * @brief Keeps the error screen up until the panel is tapped or 10 s pass, then takes it down.
 *
 * @param[in] touch  Whether touch came up, so a tap can end it.
 */
void hold_error_screen(bool touch);

/**
 * @brief Shows the fault the last reset came from, if there was one, for as long as hold_error_screen() does. The
 *        chip keeps the reset reason itself, so the record needs no no-init RAM here.
 *
 * @param[in] touch  Whether touch came up, so a tap can end it.
 */
void show_reset_fault(bool touch);

#if ERUI_RENDER_WORKERS > 1
/** @brief Spawns the core-1 render worker and hands it to the engine. */
void install_render_workers(void);
#endif

#endif
