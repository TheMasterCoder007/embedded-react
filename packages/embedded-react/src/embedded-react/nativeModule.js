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
// The simulators: the bundler also passes the module's mock, which stands in for the firmware. Its values are
// converted as the bridge converts them, so the app sees what it would on a board.

// What a value becomes in a C cell, by its signature code: as the bridge converts it.
const CONVERSIONS = {
  i: value => value | 0,
  b: value => !!value,
  f: value => Math.fround(value),
  v: () => undefined,
};

// A mock's method, called as the bridge calls the firmware's.
function mockMethod(mock, name, method, signature) {
  const qualifiedName = `${name}.${method}`;
  const implementation = mock[method];
  if (typeof implementation !== 'function') {
    throw new Error(`the mock of native module "${name}" has no ${method}()`);
  }
  const parameters = signature.slice(2);
  const convertResult = CONVERSIONS[signature[0]];
  return (...callArguments) => {
    if (callArguments.length !== parameters.length) {
      throw new TypeError(
        `${qualifiedName}() takes ${parameters.length} argument${parameters.length === 1 ? '' : 's'}, ` +
          `not ${callArguments.length}`,
      );
    }
    const converted = callArguments.map((argument, position) =>
      CONVERSIONS[parameters[position]](argument),
    );
    return convertResult(implementation.apply(mock, converted));
  };
}

// @param {string} name  The module's name, as the firmware registers it.
// @param {number} version  Its version.
// @param {Record<string, string | {reason: string}>} methods  Each method's signature, or why it can't be called;
//                                                              the bundler passes it.
// @param {object | null} [mock]  In the simulators, the module's mock (its default export), or null when it has none.
export function nativeModule(name, version, methods, mock) {
  if (!methods) {
    throw new Error(
      `native module "${name}" has no signatures; build the app with the embedded-react CLI, which reads its interface`,
    );
  }
  if (mock === null) {
    throw new Error(
      `native module "${name}" has no mock for the simulator; add ${name}.mock.ts beside its interface`,
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
    if (mock) {
      module[method] = mockMethod(mock, name, method, signature);
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
