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

// nativeModule — a module the firmware provides in C, declared by a TypeScript interface of its methods.
//
// Flow B (AOT): the compiler reads the interface and turns each method called into a native import, which the
// runtime matches with what the firmware registers when it loads the package.
//
// Flow A / simulator: not wired up yet, so declaring one stops the app with a message saying so.
//
// @param {string} name  The module's name, as the firmware registers it.
// @param {number} version  Its version.
export function nativeModule(name, version) {
  throw new Error(
    `embedded-react: native module "${name}" v${version} runs in Flow B only for now`,
  );
}
