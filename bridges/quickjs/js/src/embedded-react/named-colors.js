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

// The named colors every path accepts, as ARGB8888 with their CSS values. Keys are lowercase; callers
// lowercase the name first. The bridge's k_named table (native_ui_bridge.c) must list the same entries.
// No prototype, so `name in NAMED_COLORS` never finds `toString` and friends.

/** @type {Readonly<Record<string, number>>} */
export const NAMED_COLORS = Object.freeze(
  Object.assign(Object.create(null), {
    transparent: 0x00000000,
    black: 0xff000000,
    white: 0xffffffff,
    red: 0xffff0000,
    green: 0xff008000,
    blue: 0xff0000ff,
    gray: 0xff808080,
    grey: 0xff808080,
    yellow: 0xffffff00,
    cyan: 0xff00ffff,
    magenta: 0xffff00ff,
    orange: 0xffffa500,
  }),
);
