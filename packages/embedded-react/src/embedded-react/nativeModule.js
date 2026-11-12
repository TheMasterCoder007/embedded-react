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
// A record passes the bridge as its fields one by one, in the order the signature names them, and is built again from
// them here: a result, and an event's value.
//
// The simulators: the bundler also passes the module's mock, which stands in for the firmware. Its values are
// converted as the bridge converts them, so the app sees what it would on a board. A mock that posts events is a
// function, which is given `emit` and returns the methods.
//
// nativeModule.optional: a module only some boards have. Its `available` is whether the firmware has every method
// and event, and in the simulators whether it has a mock.

import {useEffect, useRef} from 'react';

// The values an int64 holds, which it wraps around.
const INT64_SPAN = 2 ** 64;

// What a value becomes in C, by its signature code: as the bridge converts it. An int64 is truncated and wraps, and
// comes back as the nearest number; a string ends at its first NUL.
const CONVERSIONS = {
  i: value => value | 0,
  b: value => !!value,
  f: value => Math.fround(value),
  l: value => {
    const whole = Math.trunc(Number(value));
    if (!Number.isFinite(whole)) {
      return 0;
    }
    // -0 is 0 in C.
    if (whole >= -INT64_SPAN / 2 && whole < INT64_SPAN / 2) {
      return whole || 0;
    }
    // Past 2^63 a number is a multiple of 2^11, so this is exact.
    const wrapped = whole - Math.floor(whole / INT64_SPAN) * INT64_SPAN;
    return wrapped >= INT64_SPAN / 2 ? wrapped - INT64_SPAN : wrapped;
  },
  s: value => String(value).split('\0')[0],
  v: () => undefined,
};

// Each module's events, by event name: its values' shapes and the handlers subscribed to it, or why it can't be used.
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

// A value's shape, read from a signature at `at`: its code, or a record's fields in order, each [name, shape].
function readShape(signature, at) {
  if (signature[at] !== '{') {
    return {shape: signature[at], next: at + 1};
  }
  const fields = [];
  let next = at + 1;
  for (;;) {
    const colon = signature.indexOf(':', next);
    const field = readShape(signature, colon + 1);
    fields.push([signature.slice(next, colon), field.shape]);
    next = field.next + 1;
    if (signature[field.next] === '}') {
      return {shape: fields, next};
    }
  }
}

// A signature's result, null for none, and its parameters, as shapes.
function shapesOf(signature) {
  let result = null;
  let next = 1;
  if (signature[0] !== 'v') {
    ({shape: result, next} = readShape(signature, 0));
  }
  const parameters = [];
  for (next++; next < signature.length;) {
    const parameter = readShape(signature, next);
    parameters.push(parameter.shape);
    next = parameter.next;
  }
  return {result, parameters};
}

const isRecord = shape => Array.isArray(shape);

// A value's fields one by one, into `cells`, as the bridge takes them; each converted as it converts them when
// `convert`. `what` names the value for errors.
function flatten(shape, value, what, cells, convert) {
  if (!isRecord(shape)) {
    cells.push(convert ? CONVERSIONS[shape](value) : value);
    return;
  }
  if (typeof value !== 'object' || value === null) {
    throw new TypeError(
      `${what} is a record, not ${value === null ? 'null' : typeof value}`,
    );
  }
  for (const [field, fieldShape] of shape) {
    if (!(field in value)) {
      throw new TypeError(`${what} has no field ${field}`);
    }
    flatten(fieldShape, value[field], `${what}.${field}`, cells, convert);
  }
}

// A value built from its fields' values, from `cells[at]`.
function build(shape, cells, at) {
  if (!isRecord(shape)) {
    return {value: cells[at], next: at + 1};
  }
  const record = {};
  let next = at;
  for (const [field, fieldShape] of shape) {
    const built = build(fieldShape, cells, next);
    record[field] = built.value;
    next = built.next;
  }
  return {value: record, next};
}

// Values built from the fields' values the bridge passes, a record's one by one.
function built(shapes, cells) {
  const values = [];
  let next = 0;
  for (const shape of shapes) {
    const value = build(shape, cells, next);
    values.push(value.value);
    next = value.next;
  }
  return values;
}

// Values passed through C: each converted as the bridge converts it, a record built again from its fields.
function converted(shape, value, what) {
  const cells = [];
  flatten(shape, value, what, cells, true);
  return build(shape, cells, 0).value;
}

// The fields' values an event's values are, converted by its signature, as the bridge posts them.
function convertedValues(event, values) {
  const {parameters} = event.shapes;
  if (values.length !== parameters.length) {
    throw new TypeError(
      `${event.qualifiedName} is posted with ${parameters.length} value${parameters.length === 1 ? '' : 's'}, ` +
        `not ${values.length}`,
    );
  }
  const cells = [];
  parameters.forEach((shape, position) =>
    flatten(
      shape,
      values[position],
      `${event.qualifiedName}'s value`,
      cells,
      true,
    ),
  );
  return cells;
}

// A mock's method, called as the bridge calls the firmware's. `calls` counts the mock's calls running now.
function mockMethod(implementation, name, method, signature, calls) {
  const qualifiedName = `${name}.${method}`;
  const found = implementation[method];
  // A method a plain object inherits, like toString, isn't the mock's.
  if (typeof found !== 'function' || found === Object.prototype[method]) {
    throw new Error(`the mock of native module "${name}" has no ${method}()`);
  }
  const {result, parameters} = shapesOf(signature);
  return (...callArguments) => {
    if (callArguments.length !== parameters.length) {
      throw new TypeError(
        `${qualifiedName}() takes ${parameters.length} argument${parameters.length === 1 ? '' : 's'}, ` +
          `not ${callArguments.length}`,
      );
    }
    const convertedArguments = callArguments.map((argument, position) =>
      converted(
        parameters[position],
        argument,
        `${qualifiedName}()'s argument`,
      ),
    );
    calls.running++;
    try {
      const returned = found.apply(implementation, convertedArguments);
      return result === null
        ? undefined
        : converted(result, returned, `${qualifiedName}()'s result`);
    } finally {
      calls.running--;
    }
  };
}

// A method that calls the firmware through the bridge. A record goes as its fields, and comes back as an array of
// them; without one, the bridge takes the arguments as they are.
function bridgeMethod(host, index, name, method, signature) {
  const qualifiedName = `${name}.${method}`;
  const {result, parameters} = shapesOf(signature);
  if (!isRecord(result) && !parameters.some(isRecord)) {
    return host.call.bind(host, index);
  }
  return (...callArguments) => {
    if (callArguments.length !== parameters.length) {
      throw new TypeError(
        `${qualifiedName}() takes ${parameters.length} argument${parameters.length === 1 ? '' : 's'}, ` +
          `not ${callArguments.length}`,
      );
    }
    const cells = [];
    parameters.forEach((shape, position) =>
      flatten(
        shape,
        callArguments[position],
        `${qualifiedName}()'s argument`,
        cells,
        false,
      ),
    );
    const returned = host.call(index, ...cells);
    return isRecord(result) ? build(result, returned, 0).value : returned;
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
    const shapes = shapesOf(signature);
    if (!onBoard || !available) {
      events.set(event, {qualifiedName, shapes, handlers: new Set()});
      continue;
    }
    const index = eventIndices.get(event);
    if (!HANDLERS_BY_INDEX.has(index)) {
      HANDLERS_BY_INDEX.set(index, new Set());
    }
    events.set(event, {
      qualifiedName,
      shapes,
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
      if (!entry || !entry.shapes) {
        throw new Error(
          entry
            ? `${entry.qualifiedName} ${entry.reason}`
            : `native module "${name}" has no event ${event}`,
        );
      }
      const cells = convertedValues(entry, values);
      if (calls.running) {
        return false;
      }
      deliver(entry.handlers, cells);
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
    module[method] = bridgeMethod(
      host,
      methodIndices.get(method),
      name,
      method,
      signature,
    );
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
// @param {Function} handler  Called with the event's values; a record is an object of its fields.
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
  if (!entry.shapes) {
    throw new Error(`${entry.qualifiedName} ${entry.reason}`);
  }
  const latest = useRef(handler);
  latest.current = handler;
  useEffect(() => {
    // Handlers are given the fields' values; a record is built again from its own.
    const subscriber = (...cells) =>
      latest.current(...built(entry.shapes.parameters, cells));
    entry.handlers.add(subscriber);
    return () => {
      entry.handlers.delete(subscriber);
    };
  }, [entry]);
}
