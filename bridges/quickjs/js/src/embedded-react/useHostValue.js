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

// useHostValue — a numeric value fed by the C HOST at runtime, not by the app.
//
// Flow B (AOT): the compiler recognizes `const steps = useHostValue(0)` and lowers it to an ordinary
// `s_state` field PLUS a generated public setter `er_app_set_steps(int)`. The host (e.g. a pedometer
// reading an IMU in main.c) calls that setter each frame; the app renders the value like any state.
// There is no JS setter — the host is the only writer.
//
// Flow A / simulator: there is no host, so this simply returns the initial value (the simulator has no
// IMU to drive it anyway).
//
// @param {number} initial  Initial value, used until the host writes one.
// @returns {number} the current value.
export function useHostValue(initial) {
  return initial;
}
