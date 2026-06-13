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

// The QuickJS C bridge (native_ui_bridge.c) installs a global `NativeUI` object before the
// bundle runs. Re-export it so the rest of the JS imports it cleanly instead of touching the
// global directly.
export const NativeUI = globalThis.NativeUI;

if (!NativeUI) {
  throw new Error('NativeUI global is missing — the QuickJS bridge must be installed before the bundle runs.');
}
