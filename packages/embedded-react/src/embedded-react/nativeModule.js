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

// nativeModule — a module the firmware provides in C, declared by a TypeScript interface of its methods and events.
//
// Flow B (AOT): the compiler reads the interface and turns each method called into a native import, which the
// runtime matches with what the firmware registers when it loads the package. Each useNativeEvent is a handler the
// runtime runs when the firmware posts the event, while its component is mounted.
//
// Flow A: the bundler reads the same interface and passes each method's and event's signature, since the types are
// gone by then. Each is looked up in the firmware's natives (the bridge's __erNative) here, as the app loads. A method
// calls the firmware through the bridge, which converts the arguments by the signature, and the bridge hands each
// event the firmware posts to __erNativeEvent, which calls its subscribers.
//
// The simulators: the bundler also passes the module's mock, which stands in for the firmware. Its values are
// converted as the bridge converts them, so the app sees what it would on a board. A mock that posts events is a
// function, which is given `emit` and returns the methods.
//
// nativeModule.optional: a module only some boards have. Its `available` is whether the firmware has every method
// and event, and in the simulators whether it has a mock.

import {useEffect, useRef} from 'react';

// What a value becomes in C, by its signature code: as the bridge converts it. An int64 is truncated and wraps, and
// comes back as the nearest number; a string ends at its first NUL.
const CONVERSIONS = {
  i: value => value | 0,
  b: value => !!value,
  f: value => Math.fround(value),
  l: value => {
    const whole = Math.trunc(Number(value));
    return Number.isFinite(whole)
      ? Number(BigInt.asIntN(64, BigInt(whole)))
      : 0;
  },
  s: value => String(value).split('\0')[0],
  v: () => undefined,
};

// Each module's events, by event name: its signature and the handlers subscribed to it, or why it can't be used.
const MODULE_EVENTS = new WeakMap();

// The handlers subscribed to each of the firmware's events, by its index in the natives, as the bridge posts them.
// An app-only hot reload evaluates the app again in this context, so every evaluation's modules share these: a post
// reaches each mounted subscriber, whichever evaluation it came from.
const HANDLERS_BY_INDEX = new Map();

// Calls an event's handlers with its values; a handler that unsubscribes another doesn't skip it in this post.
function deliver(handlers, values) {
  for (const handler of [...handlers]) {
    handler(...values);
  }
}

// The bridge posts each event here: the event's index in the natives, and its values.
function installDispatcher() {
  globalThis.__erNativeEvent = (index, values) => {
    const handlers = HANDLERS_BY_INDEX.get(index);
    if (handlers) {
      deliver(handlers, values);
    }
  };
}

// The arguments an event's values are, converted by its signature.
function convertedValues(event, values) {
  const parameters = event.signature.slice(2);
  if (values.length !== parameters.length) {
    throw new TypeError(
      `${event.qualifiedName} is posted with ${parameters.length} value${parameters.length === 1 ? '' : 's'}, ` +
        `not ${values.length}`,
    );
  }
  return values.map((value, position) =>
    CONVERSIONS[parameters[position]](value),
  );
}

// A mock's method, called as the bridge calls the firmware's. `calls` counts the mock's calls running now.
function mockMethod(implementation, name, method, signature, calls) {
  const qualifiedName = `${name}.${method}`;
  const found = implementation[method];
  // A method a plain object inherits, like toString, isn't the mock's.
  if (typeof found !== 'function' || found === Object.prototype[method]) {
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
    calls.running++;
    try {
      return convertResult(found.apply(implementation, converted));
    } finally {
      calls.running--;
    }
  };
}

// Where the firmware has each of a module's methods and events, by name: -1 for one it doesn't have.
function lookUp(host, name, version, signatures, finder) {
  const indices = new Map();
  for (const member of Object.keys(signatures)) {
    const signature = signatures[member];
    if (typeof signature === 'string') {
      indices.set(
        member,
        host ? host[finder](`${name}.${member}`, version, signature) : -1,
      );
    }
  }
  return indices;
}

// The module nativeModule() and nativeModule.optional() return. An optional module that isn't there has methods
// that throw, as a call to one stops the app in Flow B, and events that never arrive.
function declareModule(name, version, signatures, mock, optional) {
  if (!signatures) {
    throw new Error(
      `native module "${name}" has no signatures; build the app with the embedded-react CLI, which reads its interface`,
    );
  }
  if (mock === null && !optional) {
    throw new Error(
      `native module "${name}" has no mock for the simulator; add ${name}.mock.ts beside its interface`,
    );
  }
  const host = globalThis.__erNative;
  const module = {};
  const events = new Map();
  MODULE_EVENTS.set(module, {name, events});

  // On a board, every method and event is looked up as the app loads; in the simulators, the mock is the firmware.
  const onBoard = mock === undefined;
  const methodIndices = onBoard
    ? lookUp(host, name, version, signatures.methods, 'find')
    : new Map();
  const eventIndices = onBoard
    ? lookUp(host, name, version, signatures.events, 'findEvent')
    : new Map();
  const missing = [...eventIndices, ...methodIndices].find(
    ([, index]) => index < 0,
  );
  if (missing && !optional) {
    const [member] = missing;
    const signature = eventIndices.has(member)
      ? signatures.events[member]
      : signatures.methods[member];
    throw new Error(
      `missing native module: ${name}.${member} (version ${version}, signature "${signature}")`,
    );
  }
  const available = mock !== null && !missing;
  if (optional) {
    Object.defineProperty(module, 'available', {
      value: available,
      enumerable: true,
    });
  }

  // The events first, so a mock is given an emit that knows them.
  for (const event of Object.keys(signatures.events)) {
    const signature = signatures.events[event];
    const qualifiedName = `${name}.${event}`;
    if (typeof signature !== 'string') {
      events.set(event, {qualifiedName, reason: signature.reason});
      continue;
    }
    if (!onBoard || !available) {
      events.set(event, {qualifiedName, signature, handlers: new Set()});
      continue;
    }
    const index = eventIndices.get(event);
    if (!HANDLERS_BY_INDEX.has(index)) {
      HANDLERS_BY_INDEX.set(index, new Set());
    }
    events.set(event, {
      qualifiedName,
      signature,
      handlers: HANDLERS_BY_INDEX.get(index),
    });
    installDispatcher();
  }

  // A mock's emit posts as the firmware does, so it is refused while one of the mock's methods runs.
  let implementation = mock;
  const calls = {running: 0};
  if (typeof mock === 'function') {
    const emit = (event, ...values) => {
      const entry = events.get(event);
      if (!entry || !entry.signature) {
        throw new Error(
          entry
            ? `${entry.qualifiedName} ${entry.reason}`
            : `native module "${name}" has no event ${event}`,
        );
      }
      const converted = convertedValues(entry, values);
      if (calls.running) {
        return false;
      }
      deliver(entry.handlers, converted);
      return true;
    };
    implementation = mock(emit);
  }

  for (const method of Object.keys(signatures.methods)) {
    const signature = signatures.methods[method];
    const qualifiedName = `${name}.${method}`;
    if (typeof signature !== 'string') {
      module[method] = () => {
        throw new Error(`${qualifiedName} ${signature.reason}`);
      };
      continue;
    }
    if (!available) {
      module[method] = () => {
        throw new Error(
          `missing native module: ${qualifiedName} (the optional module "${name}" isn't available; ` +
            'read its available before calling it)',
        );
      };
      continue;
    }
    if (implementation) {
      module[method] = mockMethod(
        implementation,
        name,
        method,
        signature,
        calls,
      );
      continue;
    }
    module[method] = host.call.bind(host, methodIndices.get(method));
  }
  return module;
}

// @param {string} name  The module's name, as the firmware registers it.
// @param {number} version  Its version.
// @param {{methods: Record<string, string | {reason: string}>, events: Record<string, string | {reason: string}>}}
//        signatures  Each method's and event's signature, or why it can't be used; the bundler passes it.
// @param {object | Function | null} [mock]  In the simulators, the module's mock (its default export), or null when
//                                            it has none.
export function nativeModule(name, version, signatures, mock) {
  return declareModule(name, version, signatures, mock, false);
}

// A module only some boards have: it loads without the firmware's, and its `available` says whether it is there.
// In the simulators, it is there when it has a mock. Takes what nativeModule() takes.
nativeModule.optional = (name, version, signatures, mock) =>
  declareModule(name, version, signatures, mock, true);

// useNativeEvent — runs `handler` with an event's values each time the firmware posts it, while the component is
// mounted. The handler may change from render to render; the latest one runs.
//
// @param {object} module  A native module, as nativeModule() returns it.
// @param {string} event  One of the events its interface declares.
// @param {Function} handler  Called with the event's values.
export function useNativeEvent(module, event, handler) {
  const known = MODULE_EVENTS.get(module);
  if (!known) {
    throw new Error(
      'useNativeEvent() takes a native module, as nativeModule() returns it',
    );
  }
  const entry = known.events.get(event);
  if (!entry) {
    throw new Error(`native module "${known.name}" has no event ${event}`);
  }
  if (!entry.signature) {
    throw new Error(`${entry.qualifiedName} ${entry.reason}`);
  }
  const latest = useRef(handler);
  latest.current = handler;
  useEffect(() => {
    const subscriber = (...values) => latest.current(...values);
    entry.handlers.add(subscriber);
    return () => {
      entry.handlers.delete(subscriber);
    };
  }, [entry]);
}
