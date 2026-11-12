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

import {readFileSync} from 'node:fs';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {describe, it, expect} from 'vitest';
import {compilePackage, readNativeInterfaces} from '../compile.mts';
import {FunctionKind, Table, ValueType} from '../compiler/package/format.mts';
import {addNativeSignatures} from '../compiler/native-modules.mts';
import {OP} from '../compiler/package/bytecode.mts';
import type {CompileOptions} from '../compiler/options.mts';

// Native module calls: the imports table, and the CALLN each call becomes. Whether the runtime runs them is
// test/erb-frames' job (native-calls.tsx).

const FIXTURES = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../test/erb-frames/fixtures',
);

const OPTIONS: CompileOptions = {
  filename: 'App.tsx',
  screen: {width: 240, height: 320},
};

const SENSORS =
  'interface Sensors {\n' +
  '  read(channel: Int): number;\n' +
  '  add(left: Int, right: Int): Int;\n' +
  '  pressed(button: Int): boolean;\n' +
  '  count(): Int;\n' +
  '  setLed(on: boolean): void;\n' +
  '  scale: (value: number) => number;\n' +
  '  name(): string;\n' +
  '  level?(): number;\n' +
  '  mode(index: Int): Int;\n' +
  '  mode(on: boolean): Int;\n' +
  '}\n' +
  "const Sensors = nativeModule<Sensors>('sensors', 2);\n";

/** An app with the Sensors module, `body` before the App's return and `handler` as a button's onPress body. */
const appOf = (handler: string, body = '', text = '{n}') =>
  "import {useEffect, useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, nativeModule} from 'embedded-react';\n" +
  "import type {Int} from 'embedded-react';\n" +
  SENSORS +
  'export function App() {\n' +
  '  const [n, setN] = useState(0);\n' +
  '  const [f, setF] = useState(0.0);\n' +
  '  const [on, setOn] = useState(false);\n' +
  body +
  `  return <Pressable onPress={() => { ${handler} }}><Text>${text}</Text></Pressable>;\n` +
  '}\n';

const compile = (source: string, options: CompileOptions = {}) =>
  compilePackage(source, 'app', {...OPTIONS, ...options});

/** The message a compile fails with. */
const failure = (source: string, options: CompileOptions = {}) => {
  try {
    compile(source, options);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

/** The program's tables, its strings, and each function's instructions. */
function readProgram(bytes: Uint8Array) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = (offset: number) => view.getUint16(offset, true);
  const u32 = (offset: number) => view.getUint32(offset, true);
  const directory = 24 + u16(18) * 16 + u16(20) * 8;
  const tables = new Map<number, {offset: number; length: number}>();
  for (let index = 0; index < u16(22); index++) {
    const entry = directory + index * 12;
    tables.set(u16(entry), {offset: u32(entry + 4), length: u32(entry + 8)});
  }
  const strings = tables.get(Table.strings)!;
  const string = (index: number) => {
    const start = strings.offset + u32(strings.offset + 4 + index * 4);
    return Buffer.from(
      bytes.subarray(start, bytes.indexOf(0, start)),
    ).toString();
  };
  const imports = tables.get(Table.imports);
  const importList = Array.from(
    {length: imports ? imports.length / 8 : 0},
    (_, index) => {
      const at = imports!.offset + index * 8;
      return `${string(u16(at))} v${u16(at + 2)} ${string(u16(at + 4))}`;
    },
  );
  const nativeEvents = tables.get(Table.nativeEvents);
  const eventList = Array.from(
    {length: nativeEvents ? nativeEvents.length / 8 : 0},
    (_, index) => {
      const at = nativeEvents!.offset + index * 8;
      return {
        event: `${string(u16(at))} v${u16(at + 2)} ${string(u16(at + 4))}`,
        handler: u16(at + 6),
      };
    },
  );
  const functions = tables.get(Table.functions)!;
  const types = tables.get(Table.types);
  const code = tables.get(Table.code)!;
  const functionList = Array.from(
    {length: functions.length / 16},
    (_, index) => {
      const record = functions.offset + index * 16;
      const start = u32(record);
      const ops: {name: string; a: number; b: number; bx: number}[] = [];
      for (let at = start; at < start + u32(record + 4);) {
        const word = u32(code.offset + at * 4);
        const name = OP_NAMES[word & 0xff];
        ops.push({
          name,
          a: (word >>> 8) & 0xff,
          b: (word >>> 16) & 0xff,
          bx: word >>> 16,
        });
        at += OP[name].words;
      }
      const parameters = bytes[record + 13];
      return {
        kind: bytes[record + 15],
        parameterTypes: Array.from(
          bytes.subarray(
            types!.offset + u32(record + 8),
            types!.offset + u32(record + 8) + parameters,
          ),
        ),
        ops,
      };
    },
  );
  return {
    imports: importList,
    events: eventList,
    functions: functionList,
    ops: functionList.flatMap(programFunction => programFunction.ops),
  };
}

describe('native module calls', () => {
  it('imports each method called as module.method, with its version and signature', () => {
    const {imports} = readProgram(
      compile(
        appOf(
          'setF(Sensors.read(Sensors.count())); setN(Sensors.add(n, 2)); ' +
            'setOn(Sensors.pressed(1)); Sensors.setLed(on); setF(Sensors.read(3)); setF(Sensors.scale(f));',
        ),
      ).program,
    );
    expect(imports).toEqual([
      'sensors.count v2 i:',
      'sensors.read v2 f:i',
      'sensors.add v2 i:ii',
      'sensors.pressed v2 b:i',
      'sensors.setLed v2 v:b',
      'sensors.scale v2 f:f',
    ]);
  });

  it('passes the arguments in consecutive registers and reads the result after them', () => {
    const {ops} = readProgram(
      compile(appOf('setN(Sensors.add(n, 2));')).program,
    );
    const call = ops.findIndex(op => op.name === 'CALLN');
    expect(ops.slice(call - 2, call).map(op => [op.name, op.a])).toEqual([
      ['MOVE', ops[call].a],
      ['MOVE', ops[call].a + 1],
    ]);
    expect(ops[call + 1]).toMatchObject({name: 'MOVE', b: ops[call].a + 2});
  });

  it('drops the result of a call made as a statement', () => {
    const {ops} = readProgram(compile(appOf('Sensors.count();')).program);
    const call = ops.findIndex(op => op.name === 'CALLN');
    expect(ops[call + 1].name).not.toBe('MOVE');
  });

  it('passes a whole number as a number', () => {
    const {ops} = readProgram(
      compile(appOf('setF(Sensors.scale(n));')).program,
    );
    expect(ops.map(op => op.name)).toContain('ITOF');
  });

  it('calls a method in an effect, a timer and an updater', () => {
    expect(() =>
      compile(
        appOf(
          'setN(value => Sensors.add(value, 1));',
          '  useEffect(() => { setOn(Sensors.pressed(0)); ' +
            'const id = setInterval(() => setF(Sensors.read(1)), 100); return () => clearInterval(id); }, []);\n',
        ),
      ),
    ).not.toThrow();
  });

  it('reads the interface from the local file the app imports it from', () => {
    const file = join(FIXTURES, 'native-calls.tsx');
    const source = readFileSync(file, 'utf8');
    const nativeInterfaces = readNativeInterfaces(source, FIXTURES);
    expect(Object.keys(nativeInterfaces)).toEqual(['./native-probe']);
    const {imports} = readProgram(
      compile(source, {filename: file, nativeInterfaces}).program,
    );
    expect(imports).toContain('probe.half v1 f:i');
    expect(failure(source, {filename: file})).toMatch(
      /can't find the interface Probe/,
    );
  });
});

describe('native module calls it refuses', () => {
  it('refuses a call in render', () => {
    expect(failure(appOf('', '', '{Sensors.read(1)}'))).toMatch(
      /Sensors\.read\(\) is called in render/,
    );
    expect(
      failure(
        appOf('', '  const v = useMemo(() => Sensors.count(), []);\n', '{v}'),
      ),
    ).toMatch(/is called in render/);
  });

  it('refuses a call in a value render works out with a loop', () => {
    expect(
      failure(
        appOf(
          '',
          '  const v = useMemo(() => { let sum = 0; for (let i = 0; i < 2; i++) sum += Sensors.count(); return sum + n; }, [n]);\n',
          '{v}',
        ),
      ),
    ).toMatch(/Sensors\.count\(\) is called in render/);
    const total =
      'function total() { let sum = 0; for (let i = 0; i < 2; i++) sum += Sensors.count(); return sum; }\n';
    expect(
      failure(
        appOf('', '', '{total()}').replace(
          'export function',
          total + 'export function',
        ),
      ),
    ).toMatch(/Sensors\.count\(\) is called in render/);
    expect(() =>
      compile(
        appOf('setN(total());').replace(
          'export function',
          total + 'export function',
        ),
      ),
    ).not.toThrow();
  });

  it('refuses what the interface declares but a call cannot pass yet', () => {
    expect(failure(appOf('Sensors.name();'))).toMatch(
      /Sensors\.name returns a string, which native modules don't pass yet/,
    );
    expect(failure(appOf('setF(Sensors.level());'))).toMatch(
      /Sensors\.level is optional/,
    );
    expect(failure(appOf('Sensors.calibrate();'))).toMatch(
      /Sensors has no method calibrate/,
    );
    expect(failure(appOf('setN(Sensors.mode(1));'))).toMatch(
      /Sensors\.mode is overloaded, but a native method has one signature/,
    );
  });

  it('refuses arguments that do not fit the parameters', () => {
    expect(failure(appOf('setF(Sensors.read(f));'))).toMatch(
      /Sensors\.read\(\) takes an Int here/,
    );
    expect(failure(appOf('Sensors.setLed(n);'))).toMatch(
      /Sensors\.setLed\(\) takes a boolean here/,
    );
    expect(failure(appOf('setF(Sensors.read());'))).toMatch(
      /takes 1 argument, not 0/,
    );
  });

  it('refuses a void result used as a value', () => {
    expect(failure(appOf('const x = Sensors.setLed(on);'))).toMatch(
      /Sensors\.setLed\(\) returns nothing/,
    );
  });

  it('refuses the module read as a value', () => {
    expect(failure(appOf('const s = Sensors;'))).toMatch(
      /the native module Sensors can only have its methods called/,
    );
  });

  it('refuses a declaration it cannot read', () => {
    const declared = (declaration: string) =>
      appOf('Sensors.count();').replace(
        "const Sensors = nativeModule<Sensors>('sensors', 2);",
        declaration,
      );
    expect(
      failure(declared("const Sensors = nativeModule('sensors', 2);")),
    ).toMatch(/needs the interface that declares the module/);
    expect(
      failure(declared("let Sensors = nativeModule<Sensors>('sensors', 2);")),
    ).toMatch(/must be bound to a const/);
    expect(
      failure(
        declared("const Sensors = nativeModule<Sensors>('sensors', 1.5);"),
      ),
    ).toMatch(/version is a whole number from 0 to 65535/);
    expect(
      failure(
        declared("const Sensors = nativeModule<Sensors>('my.sensors', 1);"),
      ),
    ).toMatch(/"my\.sensors" isn't a native module name/);
    expect(
      failure(
        declared(
          "const Sensors = nativeModule<Sensors>('sensors', 2);\n" +
            "const Other = nativeModule<Sensors>('sensors', 3);",
        ),
      ),
    ).toMatch(/the native module "sensors" is declared twice/);
    expect(
      failure(declared("const Sensors = nativeModule<Missing>('sensors', 2);")),
    ).toMatch(/can't find the interface Missing/);
  });

  it('refuses a method named __proto__', () => {
    const source = appOf('Sensors.count();').replace(
      SENSORS,
      'interface Sensors { count(): Int; __proto__(): Int; }\n' +
        "const Sensors = nativeModule<Sensors>('sensors', 2);\n",
    );
    expect(failure(source)).toMatch(
      /Sensors has a method named __proto__, which a native module can't have/,
    );
  });

  it('refuses nativeModule() inside a component', () => {
    expect(
      failure(
        appOf(
          'local.count();',
          "  const local = nativeModule<Sensors>('local', 1);\n",
        ),
      ),
    ).toMatch(/nativeModule\(\) is called outside module level/);
  });

  it('refuses a method with more parameters than Flow A passes', () => {
    const params = Array.from({length: 17}, (_, index) => `p${index}: Int`);
    const source = appOf(
      `Sensors.wide(${params.map(() => '1').join(', ')});`,
    ).replace(
      SENSORS,
      `interface Sensors { wide(${params.join(', ')}): Int; }\n` +
        "const Sensors = nativeModule<Sensors>('sensors', 2);\n",
    );
    expect(failure(source)).toMatch(
      /Sensors\.wide takes more than 16 parameters/,
    );
  });

  it('refuses more native methods than a package holds', () => {
    const methods = Array.from({length: 33}, (_, index) => `m${index}(): Int;`);
    const calls = methods.map((_, index) => `Sensors.m${index}();`).join(' ');
    const source = appOf(calls).replace(
      SENSORS,
      `interface Sensors { ${methods.join(' ')} }\n` +
        "const Sensors = nativeModule<Sensors>('sensors', 2);\n",
    );
    expect(failure(source)).toMatch(/at most 32 native methods/);
  });
});

const EVENTS =
  'interface Sensors {\n' +
  '  start(): void;\n' +
  '  events: {\n' +
  '    reading(celsius: number, channel: Int, ok: boolean): void;\n' +
  '    tick(): void;\n' +
  '    name(text: string): void;\n' +
  '    count(): Int;\n' +
  '  };\n' +
  '}\n' +
  "const Sensors = nativeModule<Sensors>('sensors', 2);\n";

/** An app with the Sensors module and its events, `body` at the top of the App and `children` in its view. */
const eventsApp = (body: string, children = '', components = '') =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, View, nativeModule, useNativeEvent} from 'embedded-react';\n" +
  "import type {Int} from 'embedded-react';\n" +
  EVENTS +
  components +
  'export function App() {\n' +
  '  const [n, setN] = useState(0);\n' +
  '  const [f, setF] = useState(0.0);\n' +
  '  const [on, setOn] = useState(false);\n' +
  '  const [items, setItems] = useState([1]);\n' +
  body +
  '  return <Pressable onPress={() => { setOn(v => !v); setItems(list => [...list, 2]); }}>' +
  `<View><Text>{n} {f}</Text>${children}</View></Pressable>;\n` +
  '}\n';

/** A component that subscribes to tick, counting it. */
const TICKER =
  'function Ticker() {\n' +
  '  const [ticks, setTicks] = useState(0);\n' +
  "  useNativeEvent(Sensors, 'tick', () => setTicks(value => value + 1));\n" +
  '  return <Text>{ticks}</Text>;\n' +
  '}\n';

describe('native events', () => {
  it('lists each subscription with its event and a handler entered with its values', () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp(
          "  useNativeEvent(Sensors, 'reading', (celsius, channel, ok) => { if (ok) { setF(celsius); setN(channel); } });\n" +
            "  useNativeEvent(Sensors, 'tick', () => setN(value => value + 1));\n",
        ),
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual([
      'sensors.reading v2 v:fib',
      'sensors.tick v2 v:',
    ]);
    const [reading, tick] = events.map(entry => functions[entry.handler]);
    expect(reading.kind).toBe(FunctionKind.nativeEvent);
    expect(reading.parameterTypes).toEqual([
      ValueType.float,
      ValueType.int,
      ValueType.bool,
    ]);
    expect(tick.kind).toBe(FunctionKind.nativeEvent);
    expect(tick.parameterTypes).toEqual([]);
  });

  it('takes fewer parameters than the event has values', () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp(
          "  useNativeEvent(Sensors, 'reading', celsius => setF(celsius));\n",
        ),
      ).program,
    );
    expect(functions[events[0].handler].parameterTypes).toHaveLength(3);
  });

  it("checks a component's conditions before it runs a handler, and the App's runs at once", () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp(
          "  useNativeEvent(Sensors, 'tick', () => setN(value => value + 1));\n",
          '{on && <Ticker/>}',
          TICKER,
        ),
      ).program,
    );
    const handlers = events.map(entry => functions[entry.handler]);
    expect(handlers).toHaveLength(2);
    const [child, app] = handlers;
    expect(child.ops.map(op => op.name)).toContain('JF');
    expect(app.ops.map(op => op.name)).not.toContain('JF');
  });

  it('subscribes once for each row of a list', () => {
    const {events} = readProgram(
      compile(
        eventsApp('', '{items.map(item => <Ticker key={item}/>)}', TICKER),
        {listCap: 3},
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual(
      Array(3).fill('sensors.tick v2 v:'),
    );
  });

  it("gives Flow A each method's and event's signature", () => {
    const signed = addNativeSignatures(eventsApp(''), {});
    expect(signed).toContain(
      "nativeModule<Sensors>('sensors', 2, " +
        JSON.stringify({
          methods: {start: 'v:'},
          events: {
            reading: 'v:fib',
            tick: 'v:',
            name: {
              reason: "takes a string, which native modules don't pass yet",
            },
            count: {reason: 'returns a value, but an event returns nothing'},
          },
        }) +
        ')',
    );
  });
});

describe('native events it refuses', () => {
  const subscribed = (call: string) => eventsApp(`  ${call};\n`);

  it('refuses an event the interface does not declare or cannot pass', () => {
    expect(
      failure(subscribed("useNativeEvent(Sensors, 'done', () => {})")),
    ).toMatch(/Sensors has no event done/);
    expect(
      failure(subscribed("useNativeEvent(Sensors, 'name', () => {})")),
    ).toMatch(/Sensors's event name takes a string/);
    expect(
      failure(subscribed("useNativeEvent(Sensors, 'count', () => {})")),
    ).toMatch(/Sensors's event count returns a value/);
  });

  it('refuses a call it cannot read', () => {
    expect(failure(subscribed("useNativeEvent(n, 'tick', () => {})"))).toMatch(
      /takes a native module, an event name and a handler/,
    );
    expect(failure(subscribed("useNativeEvent(Sensors, 'tick')"))).toMatch(
      /takes a native module, an event name and a handler/,
    );
    const event = "'tick'";
    expect(
      failure(
        eventsApp(
          `  const which = ${event};\n  useNativeEvent(Sensors, which, () => {});\n`,
        ),
      ),
    ).toMatch(/event is named by a string literal/);
    expect(
      failure(
        eventsApp(
          "  const handle = () => {};\n  useNativeEvent(Sensors, 'tick', handle);\n",
        ),
      ),
    ).toMatch(/handler must be an inline function/);
    expect(
      failure(subscribed("useNativeEvent(Sensors, 'tick', (extra) => {})")),
    ).toMatch(/has 0 values, but the handler takes 1/);
    expect(
      failure(
        subscribed("useNativeEvent(Sensors, 'reading', ({x}) => setF(x))"),
      ),
    ).toMatch(/handler takes plain parameters/);
  });

  it('refuses a subscription anywhere but the top of a component', () => {
    expect(
      failure(
        eventsApp("  on && useNativeEvent(Sensors, 'tick', () => {});\n"),
      ),
    ).toMatch(/useNativeEvent\(\) is called inside a condition/);
    expect(
      failure(
        eventsApp(
          '',
          '',
          'function Ticker() {\n' +
            '  const [ticks, setTicks] = useState(0);\n' +
            "  const subscribe = () => useNativeEvent(Sensors, 'tick', () => setTicks(1));\n" +
            '  return <Text>{ticks}</Text>;\n' +
            '}\n',
        ).replace('</View>', '<Ticker/></View>'),
      ),
    ).toMatch(/useNativeEvent\(\) is called inside a condition/);
  });

  it('refuses events declared as anything but an object of methods', () => {
    const declared = (events: string) =>
      subscribed("useNativeEvent(Sensors, 'tick', () => {})").replace(
        EVENTS,
        `interface Sensors { ${events} }\n` +
          "const Sensors = nativeModule<Sensors>('sensors', 2);\n",
      );
    expect(failure(declared('events: number;'))).toMatch(
      /Sensors's events aren't an object type of events/,
    );
    expect(failure(declared('events(): void;'))).toMatch(
      /Sensors's events aren't an object type of events/,
    );
    expect(failure(declared('events: {__proto__(): void};'))).toMatch(
      /Sensors has an event named __proto__/,
    );
  });

  it('refuses more subscriptions than a package holds', () => {
    expect(
      failure(
        eventsApp('', '{items.map(item => <Ticker key={item}/>)}', TICKER),
        {listCap: 33},
      ),
    ).toMatch(/subscribes to native events 33 times, but a package holds 32/);
  });
});
