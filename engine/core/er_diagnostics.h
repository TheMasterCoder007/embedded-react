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

#ifndef EMBEDDED_REACT_ER_DIAGNOSTICS_H
#define EMBEDDED_REACT_ER_DIAGNOSTICS_H

#include <stdbool.h>

/*----------------------------------------------------------------------------------------------------------------------
 - One-shot developer warnings, behind ONE knob.
 *
 * The engine's failure modes are deliberately silent and memory-safe: a full pool truncates a shape, an
 * unregistered image just doesn't draw. That is right for a panel in the field and awful at a desk, where
 * the result looks like a rendering bug rather than a budget that ran out. These warnings name the flag to
 * raise, once per site — an overflow usually recurs every frame, and one line is enough to act on.
 *
 * ERUI_DIAGNOSTICS is the single switch, because the reason to turn them off is almost always "this target
 * must not link <stdio.h>", and that has to be answerable in one place. Split across a knob per subsystem
 * it is not: turning off two of three still drags fprintf in through the third.
 *
 *   0  nothing — no <stdio.h>, no strings, no code. The bare-metal answer.
 *   1  release warnings only: the few failures that stay invisible on a real panel (see below).
 *   2  everything, including pool-overflow diagnostics.
 *
 * The default is 1 under NDEBUG and 2 otherwise.
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_DIAGNOSTICS
#ifdef NDEBUG
#define ERUI_DIAGNOSTICS 1
#else
#define ERUI_DIAGNOSTICS 2
#endif
#endif

#if ERUI_DIAGNOSTICS >= 1
#include <stdio.h>

/* The latch is static to each expansion, so every call site warns independently. */
#define ER_WARN_ONCE_IMPL(...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool er_warned_ = false;                                                                                \
        if (!er_warned_)                                                                                               \
        {                                                                                                              \
            er_warned_ = true;                                                                                         \
            fprintf(stderr, __VA_ARGS__);                                                                              \
        }                                                                                                              \
    } while (0)
#else
#define ER_WARN_ONCE_IMPL(...) ((void)0)
#endif

/**
 * @brief Warns once per call site, printf-style. Compiled out below ERUI_DIAGNOSTICS 2.
 *
 * For failures a developer can SEE: a truncated shape, an image that refuses to load. The screen already
 * shows something recognisably wrong and points at the thing being edited, so this is a convenience on a
 * dev build rather than something a release needs.
 */
#if ERUI_DIAGNOSTICS >= 2
#define ERUI_WARN_ONCE(...) ER_WARN_ONCE_IMPL(__VA_ARGS__)
#else
#define ERUI_WARN_ONCE(...) ((void)0)
#endif

/**
 * @brief Warns once per call site even in a release build. Compiled out only at ERUI_DIAGNOSTICS 0.
 *
 * For failures that leave NO usable trace on a real panel — where the symptom is not the code that broke.
 * Today that is exhausting the vector slot pool: slots are handed out in mount order, so which nodes lose
 * their geometry shifts as screens mount and unmount, and the panel just glitches with no culprit. One
 * fprintf on a path that has already failed, on a build that is otherwise silent.
 */
#define ERUI_WARN_ONCE_RELEASE(...) ER_WARN_ONCE_IMPL(__VA_ARGS__)

#endif
