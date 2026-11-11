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
// Flow A: the bundler reads the same interface and passes each method's signature, since the types are gone by
// then. Each method is looked up in the firmware's natives (the bridge's __erNative) here, as the app loads, and
// calls it through the bridge, which converts the arguments by the signature.
//
// @param {string} name  The module's name, as the firmware registers it.
// @param {number} version  Its version.
// @param {Record<string, string | {reason: string}>} methods  Each method's signature, or why it can't be called;
//                                                              the bundler passes it.
export function nativeModule(name, version, methods) {
  if (!methods) {
    throw new Error(
      `native module "${name}" has no signatures; build the app with the embedded-react CLI, which reads its interface`,
    );
  }
  const host = globalThis.__erNative;
  const module = {};
  for (const method of Object.keys(methods)) {
    const signature = methods[method];
    const qualifiedName = `${name}.${method}`;
    if (typeof signature !== 'string') {
      module[method] = () => {
        throw new Error(`${qualifiedName} ${signature.reason}`);
      };
      continue;
    }
    const index = host ? host.find(qualifiedName, version, signature) : -1;
    if (index < 0) {
      throw new Error(
        `missing native module: ${qualifiedName} (version ${version}, signature "${signature}")`,
      );
    }
    module[method] = host.call.bind(host, index);
  }
  return module;
}
