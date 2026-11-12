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

import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {describe, it, expect} from 'vitest';
import {compilePackage} from '../compile.mts';
import {
  FunctionKind,
  NONE,
  Table,
  ValueType,
} from '../compiler/package/format.mts';
import {addNativeSignatures} from '../compiler/native-modules.mts';
import {readNativeFiles} from '../compiler/native-files.mts';
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
  '  name(): number[];\n' +
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
      const module = u16(at + 6);
      return (
        `${string(u16(at))} v${u16(at + 2)} ${string(u16(at + 4))}` +
        (module === NONE ? '' : ` (optional ${module})`)
      );
    },
  );
  const nativeEvents = tables.get(Table.nativeEvents);
  const eventList = Array.from(
    {length: nativeEvents ? nativeEvents.length / 12 : 0},
    (_, index) => {
      const at = nativeEvents!.offset + index * 12;
      return {
        event: `${string(u16(at))} v${u16(at + 2)} ${string(u16(at + 4))}`,
        handler: u16(at + 6),
        module: u16(at + 8),
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
      const ops: {
        name: string;
        a: number;
        b: number;
        bx: number;
        extension?: number;
      }[] = [];
      for (let at = start; at < start + u32(record + 4);) {
        const word = u32(code.offset + at * 4);
        const name = OP_NAMES[word & 0xff];
        ops.push({
          name,
          a: (word >>> 8) & 0xff,
          b: (word >>> 16) & 0xff,
          bx: word >>> 16,
          ...(OP[name].words === 2
            ? {extension: u32(code.offset + (at + 1) * 4)}
            : {}),
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
    const nativeFiles = readNativeFiles(source, FIXTURES);
    expect(Object.keys(nativeFiles)).toEqual(['./native-probe']);
    const {imports} = readProgram(
      compile(source, {filename: file, nativeFiles}).program,
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
      /Sensors\.name returns a type native modules don't pass/,
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
    expect(failure(source)).toMatch(/Sensors\.wide takes more than 16 values/);
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
  '    name(text: number[]): void;\n' +
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

  it("runs an event's subscribers from one function, reading whether each is mounted before any runs", () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp(
          "  useNativeEvent(Sensors, 'tick', () => setN(value => value + 1));\n",
          '{on && <Ticker/>}',
          TICKER,
        ),
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual(['sensors.tick v2 v:']);
    const names = functions[events[0].handler].ops.map(op => op.name);
    // The child's test is kept, then each body runs; only the child's is skipped when its test is false.
    expect(names.filter(name => name === 'JF')).toHaveLength(1);
    expect(names.indexOf('JF')).toBeLessThan(names.indexOf('SET32'));
  });

  it("leaves only a subscriber's own body on an early return", () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp(
          "  useNativeEvent(Sensors, 'reading', celsius => { if (celsius < 0) return; setF(celsius); });\n" +
            "  useNativeEvent(Sensors, 'reading', () => setN(value => value + 1));\n",
        ),
      ).program,
    );
    const names = functions[events[0].handler].ops.map(op => op.name);
    expect(names.filter(name => name === 'RET')).toEqual(['RET']);
    expect(names.at(-1)).toBe('RET');
  });

  it("subscribes each row of a list in its event's function", () => {
    const {events, functions} = readProgram(
      compile(
        eventsApp('', '{items.map(item => <Ticker key={item}/>)}', TICKER),
        {listCap: 3},
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual(['sensors.tick v2 v:']);
    const names = functions[events[0].handler].ops.map(op => op.name);
    expect(names.filter(name => name === 'JF')).toHaveLength(3);
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
              reason: "takes a type native modules don't pass",
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
    ).toMatch(/Sensors's event name takes a type native modules don't pass/);
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

const WIFI =
  'interface Wifi {\n' +
  '  connect(channel: Int): boolean;\n' +
  '  strength(): Int;\n' +
  '  name(): number[];\n' +
  '  events: {found(rssi: Int): void; lost(): void};\n' +
  '}\n' +
  "const Wifi = nativeModule.optional<Wifi>('wifi', 3);\n";

/** An app with the Sensors module and the optional Wifi module. */
const optionalApp = (body: string, children = '', handler = '') =>
  eventsApp(
    body,
    `${children}<Pressable onPress={() => { ${handler} }}><Text>go</Text></Pressable>`,
    WIFI,
  );

describe('optional native modules', () => {
  it('lists every method and event of an optional module, used or not, with its module', () => {
    const {imports, events} = readProgram(
      compile(
        optionalApp(
          "  useNativeEvent(Wifi, 'found', rssi => setN(rssi));\n" +
            "  useNativeEvent(Sensors, 'tick', () => setN(value => value + 1));\n",
          '',
          'Sensors.start(); setOn(Wifi.connect(n));',
        ),
      ).program,
    );
    expect(imports).toEqual([
      'wifi.connect v3 b:i (optional 0)',
      'wifi.strength v3 i: (optional 0)',
      'sensors.start v2 v:',
    ]);
    expect(
      events.map(entry => [entry.event, entry.handler === NONE, entry.module]),
    ).toEqual([
      ['wifi.found v3 v:i', false, 0],
      ['sensors.tick v2 v:', false, NONE],
      ['wifi.lost v3 v:', true, 0],
    ]);
  });

  it('reads available once at boot, into a value render and handlers read', () => {
    const {functions} = readProgram(
      compile(
        optionalApp(
          '',
          "{Wifi.available && <Text>wifi</Text>}<Text>{Wifi.available ? 'on' : 'off'}</Text>",
          'if (Wifi.available) { setN(Wifi.strength()); }',
        ),
      ).program,
    );
    const reads = functions.filter(programFunction =>
      programFunction.ops.some(op => op.name === 'AVAIL'),
    );
    expect(reads.map(programFunction => programFunction.kind)).toEqual([
      FunctionKind.boot,
    ]);
    expect(reads[0].ops.find(op => op.name === 'AVAIL')!.bx).toBe(0);
  });

  it('numbers the optional modules in the order they are declared', () => {
    const source = optionalApp(
      '',
      '{Gps.available && <Text>gps</Text>}',
    ).replace(
      WIFI,
      WIFI +
        'interface Gps { fix(): boolean; }\n' +
        "const Gps = nativeModule.optional<Gps>('gps', 1);\n",
    );
    const {imports, functions} = readProgram(compile(source).program);
    expect(imports).toContain('gps.fix v1 b: (optional 1)');
    const boot = functions.find(
      programFunction => programFunction.kind === FunctionKind.boot,
    )!;
    expect(boot.ops.find(op => op.name === 'AVAIL')!.bx).toBe(1);
  });

  it('gives Flow A the signatures of nativeModule.optional too', () => {
    const signed = addNativeSignatures(optionalApp(''), {});
    expect(signed).toContain(
      "nativeModule.optional<Wifi>('wifi', 3, " +
        JSON.stringify({
          methods: {
            connect: 'b:i',
            strength: 'i:',
            name: {
              reason: "returns a type native modules don't pass",
            },
          },
          events: {found: 'v:i', lost: 'v:'},
        }) +
        ')',
    );
  });
});

describe('optional native modules it refuses', () => {
  it('refuses available on a module the firmware must have', () => {
    expect(
      failure(eventsApp('', '{Sensors.available && <Text>s</Text>}')),
    ).toMatch(/Sensors is always available, since the firmware must have it/);
  });

  it('refuses a method named available', () => {
    expect(
      failure(
        optionalApp('').replace(
          'strength(): Int;',
          'strength(): Int;\n  available(): boolean;',
        ),
      ),
    ).toMatch(/Wifi has a method named available/);
  });

  it('refuses an optional module with nothing to look for', () => {
    expect(
      failure(
        optionalApp('').replace(
          WIFI,
          'interface Wifi { name(): number[]; }\n' +
            "const Wifi = nativeModule.optional<Wifi>('wifi', 3);\n",
        ),
      ),
    ).toMatch(/"wifi" has no methods or events to look for/);
  });

  it('refuses nativeModule.optional() inside a component', () => {
    expect(
      failure(
        eventsApp(
          "  const Gps = nativeModule.optional<Wifi>('gps', 1);\n",
          '',
          WIFI,
        ),
      ),
    ).toMatch(/nativeModule\(\) is called outside module level/);
  });

  it('counts every method of an optional module toward the imports a package holds', () => {
    const methods = Array.from({length: 33}, (_, index) => `m${index}(): Int;`);
    const source = optionalApp('').replace(
      WIFI,
      `interface Wifi { ${methods.join(' ')} }\n` +
        "const Wifi = nativeModule.optional<Wifi>('wifi', 3);\n",
    );
    expect(failure(source)).toMatch(/at most 32 native methods/);
  });
});

const WORDS =
  'interface Words {\n' +
  '  greet(name: string): string;\n' +
  '  length(text: string): Int;\n' +
  '  shift(seconds: Int, stamp: Int64): Int64;\n' +
  '  events: {named(name: string, stamp: Int64): void};\n' +
  '}\n' +
  "const Words = nativeModule<Words>('words', 1);\n";

/** An app with the Words module: `handler` as a button's onPress body, and `body` at the top of the App. */
const wordsApp = (handler: string, body = '') =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, nativeModule, useNativeEvent} from 'embedded-react';\n" +
  "import type {Int, Int64} from 'embedded-react';\n" +
  WORDS +
  'export function App() {\n' +
  "  const [s, setS] = useState('x');\n" +
  '  const [n, setN] = useState(0);\n' +
  '  const [stamp, setStamp] = useState(4000000000);\n' +
  body +
  `  return <Pressable onPress={() => { ${handler} }}><Text>{s} {n} {stamp}</Text></Pressable>;\n` +
  '}\n';

describe('native strings and Int64s', () => {
  it('signs a string s and an Int64 l', () => {
    const {imports} = readProgram(
      compile(
        wordsApp(
          'setS(Words.greet(s)); setN(Words.length(s)); setStamp(Words.shift(n, stamp));',
        ),
      ).program,
    );
    expect(imports).toEqual([
      'words.greet v1 s:s',
      'words.length v1 i:s',
      'words.shift v1 l:il',
    ]);
  });

  it("copies each call's string result into a buffer of its own", () => {
    const {ops} = readProgram(
      compile(wordsApp('setN(Words.length(Words.greet(Words.greet(s))));'))
        .program,
    );
    const calls = ops.filter(op => op.name === 'CALLNS');
    expect(calls).toHaveLength(2);
    expect(calls[0].extension).not.toBe(calls[1].extension);
    expect(ops.filter(op => op.name === 'CALLN')).toHaveLength(1);
  });

  it('passes an Int64 on an even register, after a gap', () => {
    const {ops} = readProgram(
      compile(wordsApp('setStamp(Words.shift(n, stamp));')).program,
    );
    const call = ops.findIndex(op => op.name === 'CALLN');
    const [seconds, stamp] = ops.slice(call - 2, call);
    expect(seconds.a).toBe(ops[call].a);
    expect(stamp.a % 2).toBe(0);
    expect(stamp.a).toBe(ops[call].a + 1 + ((ops[call].a + 1) % 2));
  });

  it("enters an event's handler with a string, then an Int64 on an even register", () => {
    const {events, functions} = readProgram(
      compile(
        wordsApp(
          '',
          "  useNativeEvent(Words, 'named', (name, at) => { setS(name); setStamp(at); });\n",
        ),
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual(['words.named v1 v:sl']);
    expect(functions[events[0].handler].parameterTypes).toEqual([
      ValueType.string,
      ValueType.int,
      ValueType.int64,
      ValueType.pair,
    ]);
  });

  it('gives Flow A the signatures', () => {
    expect(addNativeSignatures(wordsApp(''), {})).toContain(
      "nativeModule<Words>('words', 1, " +
        JSON.stringify({
          methods: {greet: 's:s', length: 'i:s', shift: 'l:il'},
          events: {named: 'v:sl'},
        }) +
        ')',
    );
  });
});

describe('native strings and Int64s it refuses', () => {
  it('refuses text built in the call, and a value of another type', () => {
    expect(failure(wordsApp('setS(Words.greet(`a${n}`));'))).toMatch(
      /Words\.greet\(\) takes a string, not text built in the call/,
    );
    expect(failure(wordsApp("setS(Words.greet(s + '!'));"))).toMatch(
      /takes a string, not text built in the call/,
    );
    expect(failure(wordsApp('setS(Words.greet(n));'))).toMatch(
      /Words\.greet\(\) takes a string here/,
    );
    expect(failure(wordsApp('setStamp(Words.shift(n, 1.5));'))).toMatch(
      /Words\.shift\(\) takes an Int64 here/,
    );
    expect(failure(wordsApp('setStamp(Words.shift(stamp, stamp));'))).toMatch(
      /a 64-bit value .* cannot be used here/,
    );
  });
});

const MAPS =
  'interface Point { x: Int; y: Int; }\n' +
  'type Fix = { name: string; at: Int64; spot: Point; ok: boolean };\n' +
  'interface Maps {\n' +
  '  move(from: Point, step: Int): Point;\n' +
  '  fix(): Fix;\n' +
  '  pair(): {first: string; second: string};\n' +
  '  events: {found(where: Fix, count: Int): void};\n' +
  '}\n' +
  "const Maps = nativeModule<Maps>('maps', 1);\n";

/** An app with the Maps module: `handler` as a button's onPress body, and `body` at the top of the App. */
const mapsApp = (handler: string, body = '', module = MAPS) =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, nativeModule, useNativeEvent} from 'embedded-react';\n" +
  "import type {Int, Int64} from 'embedded-react';\n" +
  module +
  'export function App() {\n' +
  '  const [p, setP] = useState({x: 0, y: 0});\n' +
  "  const [s, setS] = useState('x');\n" +
  '  const [n, setN] = useState(0);\n' +
  '  const [stamp, setStamp] = useState(4000000000);\n' +
  body +
  `  return <Pressable onPress={() => { ${handler} }}><Text>{p.x} {p.y} {s} {n} {stamp}</Text></Pressable>;\n` +
  '}\n';

describe('native records', () => {
  it('signs a record by its fields, in order', () => {
    const {imports} = readProgram(
      compile(
        mapsApp(
          'setP(Maps.move(p, 1)); const fix = Maps.fix(); setS(fix.name);',
        ),
      ).program,
    );
    expect(imports).toEqual([
      'maps.move v1 {x:i,y:i}:{x:i,y:i}i',
      'maps.fix v1 {name:s,at:l,spot:{x:i,y:i},ok:b}:',
    ]);
  });

  it("passes a record's fields one by one, from an object state or one built in the call", () => {
    const {ops} = readProgram(
      compile(
        mapsApp('setP(Maps.move(p, 1)); setP(Maps.move({y: n, x: 2}, n));'),
      ).program,
    );
    expect(ops.filter(op => op.name === 'CALLN')).toHaveLength(2);
  });

  it('keeps a record in a const, destructured or not, and in an object state', () => {
    const {ops} = readProgram(
      compile(
        mapsApp(
          'const fix = Maps.fix(); setS(fix.name); setN(fix.spot.y); ' +
            'const {at, spot: {x}} = Maps.fix(); setStamp(at); setN(x); ' +
            'setP(Maps.fix().spot); setN(Maps.fix().spot.y);',
        ),
      ).program,
    );
    expect(ops.filter(op => op.name === 'CALLNS')).toHaveLength(4);
  });

  it("copies a record's strings into buffers of its own, one after another", () => {
    const {ops} = readProgram(
      compile(mapsApp('const pair = Maps.pair(); setS(pair.second);')).program,
    );
    expect(ops.filter(op => op.name === 'CALLNS')).toHaveLength(1);
  });

  it("enters an event's handler with a record's fields, which it may destructure", () => {
    const {events, functions} = readProgram(
      compile(
        mapsApp(
          '',
          "  useNativeEvent(Maps, 'found', (where, count) => { setS(where.name); setN(count + where.spot.y); });\n" +
            "  useNativeEvent(Maps, 'found', ({at, spot}) => { setStamp(at); setP(spot); });\n",
        ),
      ).program,
    );
    expect(events.map(entry => entry.event)).toEqual([
      'maps.found v1 v:{name:s,at:l,spot:{x:i,y:i},ok:b}i',
    ]);
    expect(functions[events[0].handler].parameterTypes).toEqual([
      ValueType.string,
      ValueType.int,
      ValueType.int64,
      ValueType.pair,
      ValueType.int,
      ValueType.int,
      ValueType.bool,
      ValueType.int,
    ]);
  });

  it('gives Flow A the signatures', () => {
    expect(addNativeSignatures(mapsApp(''), {})).toContain(
      "nativeModule<Maps>('maps', 1, " +
        JSON.stringify({
          methods: {
            move: '{x:i,y:i}:{x:i,y:i}i',
            fix: '{name:s,at:l,spot:{x:i,y:i},ok:b}:',
            pair: '{first:s,second:s}:',
          },
          events: {found: 'v:{name:s,at:l,spot:{x:i,y:i},ok:b}i'},
        }) +
        ')',
    );
  });
});

describe('native records it refuses', () => {
  it('refuses a record read as a plain value', () => {
    for (const handler of [
      'setN(Maps.move(p, 1) ? 1 : 0);',
      'setN(Maps.move(p, 1));',
      'setN(1 + Maps.fix().spot.x);',
    ]) {
      expect(failure(mapsApp(handler))).toMatch(
        /Maps\.(move|fix)\(\) returns a record, which only a const or an object state keeps/,
      );
    }
  });

  it('refuses an argument without every field, or of another type', () => {
    expect(failure(mapsApp('setP(Maps.move({x: 1}, 2));'))).toMatch(
      /the record Maps\.move\(\) takes has a field y, which this object doesn't/,
    );
    expect(failure(mapsApp('setP(Maps.move(n, 2));'))).toMatch(
      /Maps\.move\(\) takes a record here/,
    );
    expect(failure(mapsApp('setP(Maps.move({x: 1.5, y: 0}, 2));'))).toMatch(
      /Maps\.move\(\) takes an Int as x/,
    );
  });

  it('refuses a record kept in a let', () => {
    expect(failure(mapsApp('let fix = Maps.fix(); setS(fix.name);'))).toMatch(
      /an object in a handler must be a `const`/,
    );
  });

  it("refuses a record the signature can't hold", () => {
    const refused = (fields: string, call = 'Odd.get()') =>
      failure(
        mapsApp(
          `const f = ${call};`,
          '',
          MAPS +
            `type Shape = ${fields};\n` +
            'interface Odd { get(): Shape; }\n' +
            "const Odd = nativeModule<Odd>('odd', 1);\n",
        ),
      );
    expect(refused('{x?: Int}')).toMatch(
      /Odd\.get returns a record whose field x is optional/,
    );
    expect(refused('{x: Int; go(): void}')).toMatch(
      /returns a record with a member that is not a plain field/,
    );
    expect(refused('{$x: Int}')).toMatch(
      /a field named \$x, which a native signature can't name/,
    );
    expect(refused('{}')).toMatch(/returns an empty record/);
    expect(refused('{next: Shape}')).toMatch(
      /whose field next is the record Shape, which holds itself/,
    );
    expect(refused('{x: Int[]}')).toMatch(
      /whose field x is a type native modules don't pass/,
    );
    expect(
      refused(
        `{${Array.from({length: 17}, (_, index) => `f${index}: Int`).join('; ')}}`,
      ),
    ).toMatch(/returns more than 16 values, counting each field of its record/);
  });
});

describe('native modules from other files and packages', () => {
  /** A folder of files, by their paths in it. */
  const folderOf = (files: Record<string, string>) => {
    const directory = mkdtempSync(join(tmpdir(), 'native-files-'));
    for (const [path, source] of Object.entries(files)) {
      mkdirSync(dirname(join(directory, path)), {recursive: true});
      writeFileSync(join(directory, path), source);
    }
    return directory;
  };

  const CAN =
    "import {nativeModule} from 'embedded-react';\n" +
    "import type {Int} from 'embedded-react';\n" +
    'export interface Can { send(id: Int): boolean; }\n' +
    "export const Can = nativeModule<Can>('can', 1);\n";

  /** An app that imports Can by a path and sends with it. */
  const canApp = (from: string) =>
    "import {useState} from 'react';\n" +
    "import {Pressable, Text} from 'embedded-react';\n" +
    `import {Can} from '${from}';\n` +
    'export function App() {\n' +
    '  const [on, setOn] = useState(false);\n' +
    "  return <Pressable onPress={() => setOn(Can.send(1))}><Text>{on ? 'sent' : 'idle'}</Text></Pressable>;\n" +
    '}\n';

  /** The imports an app in a folder compiles to, its files read as the CLI reads them. */
  const importsOf = (directory: string, source: string) =>
    readProgram(
      compile(source, {
        filename: join(directory, 'App.tsx'),
        nativeFiles: readNativeFiles(source, directory),
      }).program,
    ).imports;

  it('imports a module from a package, with its interface in a file beside it', () => {
    const file = join(FIXTURES, 'native-package.tsx');
    const source = readFileSync(file, 'utf8');
    const nativeFiles = readNativeFiles(source, FIXTURES);
    expect(nativeFiles['probe-device'].path).toBe(
      'node_modules/probe-device/index.ts',
    );
    expect(Object.keys(nativeFiles['probe-device'].imports)).toEqual([
      './probe',
    ]);
    expect(
      readProgram(compile(source, {filename: file, nativeFiles}).program)
        .imports,
    ).toEqual([
      'radio.tune v1 b:i (optional 0)',
      'probe.next v1 i:',
      'probe.add v1 i:ii',
      'probe.start v1 v:i',
    ]);
  });

  it('imports a module from a local file, by the path NodeNext writes too', () => {
    const directory = folderOf({
      'device.ts':
        "export type * from './types';\n" +
        "export type {Reading} from './types';\n" +
        CAN,
      'types.ts': 'export interface Reading { id: number; }\n',
    });
    expect(importsOf(directory, canApp('./device'))).toEqual([
      'can.send v1 b:i',
    ]);
    expect(importsOf(directory, canApp('./device.js'))).toEqual([
      'can.send v1 b:i',
    ]);
  });

  it("finds a package's file by its exports, its conditions and a subpath, or by its main", () => {
    const directory = folderOf({
      'node_modules/by-exports/package.json': JSON.stringify({
        exports: {
          '.': {types: './index.d.ts', import: './src/index.ts'},
          './*': './src/missing/*.ts',
          './buses/*': './src/buses/*.ts',
        },
      }),
      'node_modules/by-exports/src/index.ts': CAN,
      'node_modules/by-exports/src/buses/can.ts': CAN,
      'node_modules/@acme/by-main/package.json': JSON.stringify({
        main: 'device',
      }),
      'node_modules/@acme/by-main/device.ts': CAN,
    });
    for (const from of [
      'by-exports',
      'by-exports/buses/can',
      '@acme/by-main',
    ]) {
      expect(importsOf(directory, canApp(from))).toEqual(['can.send v1 b:i']);
    }
  });

  it('reads no file an export points at outside its package', () => {
    const directory = folderOf({
      'outside.ts': CAN,
      'node_modules/escapes/package.json': JSON.stringify({
        exports: {
          '.': '../../outside.ts',
          './up/*': './*/../../../outside.ts',
          './modules/*': './node_modules/*.ts',
        },
      }),
      'node_modules/escapes/node_modules/can.ts': CAN,
    });
    for (const from of ['escapes', 'escapes/up/x', 'escapes/modules/can']) {
      expect(readNativeFiles(canApp(from), directory)).toEqual({});
    }
  });

  it('follows a workspace link to its package, from a folder of the app', () => {
    const directory = folderOf({
      'device/package.json': JSON.stringify({exports: './index.ts'}),
      'device/index.ts': CAN,
      'app/.keep': '',
    });
    mkdirSync(join(directory, 'node_modules'));
    symlinkSync(
      join(directory, 'device'),
      join(directory, 'node_modules', 'device'),
      'junction',
    );
    const app = join(directory, 'app');
    expect(readNativeFiles(canApp('device'), app).device.path).toBe(
      '../device/index.ts',
    );
    expect(importsOf(app, canApp('device'))).toEqual(['can.send v1 b:i']);
  });

  it("keeps one module imported twice, and numbers optional ones after the app's own", () => {
    const directory = folderOf({
      'device.ts':
        CAN.replace('nativeModule<Can>', 'nativeModule.optional<Can>') +
        'export {Can as Bus};\n',
    });
    const source = canApp('./device')
      .replace(
        "import {Can} from './device';",
        "import {Can, Bus} from './device';\n" +
          'interface Gps { fix(): boolean; }\n' +
          "const Gps = nativeModule.optional<Gps>('gps', 1);",
      )
      .replace('Can.send(1)', 'Can.send(1) && Bus.send(2) && Gps.fix()')
      .replace(
        "import {Pressable, Text} from 'embedded-react';",
        "import {Pressable, Text, nativeModule} from 'embedded-react';",
      );
    expect(importsOf(directory, source)).toEqual([
      'gps.fix v1 b: (optional 0)',
      'can.send v1 b:i (optional 1)',
    ]);
  });

  it("imports a module from the file the app's own module takes its interface from", () => {
    const directory = folderOf({
      'device.ts': CAN + 'export interface Gps { fix(): boolean; }\n',
    });
    const source = canApp('./device')
      .replace(
        "import {Can} from './device';",
        "import {Can, type Gps} from './device';\n" +
          "const Gps = nativeModule<Gps>('gps', 1);",
      )
      .replace('Can.send(1)', 'Can.send(1) && Gps.fix()')
      .replace(
        "import {Pressable, Text} from 'embedded-react';",
        "import {Pressable, Text, nativeModule} from 'embedded-react';",
      );
    expect(importsOf(directory, source)).toEqual([
      'can.send v1 b:i',
      'gps.fix v1 b:',
    ]);
  });

  it('leaves a file that declares no module to the rest of the compiler', () => {
    const directory = folderOf({'helpers.ts': 'export const answer = 42;\n'});
    expect(readNativeFiles(canApp('./helpers'), directory)).toEqual({});
  });
});

describe('native modules from other files and packages it refuses', () => {
  const folderOf = (files: Record<string, string>) => {
    const directory = mkdtempSync(join(tmpdir(), 'native-files-'));
    for (const [path, source] of Object.entries(files)) {
      mkdirSync(dirname(join(directory, path)), {recursive: true});
      writeFileSync(join(directory, path), source);
    }
    return directory;
  };

  const app = (from: string) =>
    "import {Pressable, Text} from 'embedded-react';\n" +
    `import {Can} from '${from}';\n` +
    'export function App() {\n' +
    '  return <Pressable onPress={() => { Can.send(1); }}><Text>send</Text></Pressable>;\n' +
    '}\n';

  const failureIn = (directory: string, source: string) =>
    failure(source, {
      filename: join(directory, 'App.tsx'),
      nativeFiles: readNativeFiles(source, directory),
    });

  it('refuses a package that ships only JavaScript', () => {
    const directory = folderOf({
      'node_modules/built/package.json': JSON.stringify({
        exports: {types: './index.d.ts', default: './index.js'},
      }),
      'node_modules/built/index.js':
        "import {nativeModule} from 'embedded-react';\n" +
        "export const Can = nativeModule('can', 1);\n",
      'node_modules/built/index.d.ts': 'export declare const Can: unknown;\n',
    });
    expect(failureIn(directory, app('built'))).toMatch(
      /built is JavaScript or a \.d\.ts, which a native module isn't read from \(node_modules\/built\/index\.js\)/,
    );
  });

  it('refuses a package whose export is a .jsx file', () => {
    const directory = folderOf({
      'node_modules/built/package.json': JSON.stringify({
        exports: './index.jsx',
      }),
      'node_modules/built/index.jsx':
        "import {nativeModule} from 'embedded-react';\n" +
        "export const Can = nativeModule('can', 1);\n",
    });
    expect(failureIn(directory, app('built'))).toMatch(
      /built is JavaScript or a \.d\.ts/,
    );
  });

  it("pins an error in the package's file to the app's import, and says where it is", () => {
    const directory = folderOf({
      'node_modules/broken/package.json': JSON.stringify({
        exports: './index.ts',
      }),
      'node_modules/broken/index.ts':
        "import {nativeModule} from 'embedded-react';\n" +
        "export const Can = nativeModule<Missing>('can', 1);\n",
    });
    const source = app('broken');
    let thrown: (Error & {aotLoc?: {line: number}}) | undefined;
    try {
      compile(source, {
        filename: join(directory, 'App.tsx'),
        nativeFiles: readNativeFiles(source, directory),
      });
    } catch (error) {
      thrown = error as typeof thrown;
    }
    expect(thrown?.message).toMatch(
      /can't find the interface Missing \(in node_modules\/broken\/index\.ts:2:20\)/,
    );
    expect(thrown?.aotLoc?.line).toBe(2);
  });

  it('refuses a module passed on from another file', () => {
    const can =
      "import {nativeModule} from 'embedded-react';\n" +
      'export interface Can { send(id: number): boolean; }\n' +
      "export const Can = nativeModule<Can>('can', 1);\n";
    for (const passes of [
      "export * from './can';\n",
      "export {Can} from './can';\n",
    ]) {
      const directory = folderOf({'device.ts': passes, 'can.ts': can});
      expect(failureIn(directory, app('./device'))).toMatch(
        /\.\/device passes Can on from another file, and a native module isn't followed further/,
      );
    }
    const chain = folderOf({
      'device.ts': "export * from './buses';\n",
      'buses.ts': "export {Can} from './can';\n",
      'can.ts': can,
    });
    expect(failureIn(chain, app('./device'))).toMatch(
      /\.\/device passes Can on from another file/,
    );
    const loop = folderOf({
      'device.ts': "export * from './other';\n",
      'other.ts': "export * from './device';\n",
    });
    expect(readNativeFiles(app('./device'), loop)).toEqual({});
    const plain = folderOf({
      'device.ts': "export * from './helpers';\n",
      'helpers.ts': 'export const answer = 42;\n',
    });
    expect(readNativeFiles(app('./device'), plain)).toEqual({});
  });

  it('refuses one module declared both in the app and in a file it imports', () => {
    const directory = folderOf({
      'device.ts':
        "import {nativeModule} from 'embedded-react';\n" +
        'export interface Can { send(id: number): boolean; }\n' +
        "export const Can = nativeModule<Can>('can', 1);\n",
    });
    const source = app('./device')
      .replace(
        "import {Can} from './device';",
        "import {Can} from './device';\n" +
          'interface Bus { send(id: number): boolean; }\n' +
          "const Bus = nativeModule<Bus>('can', 1);",
      )
      .replace(
        "import {Pressable, Text} from 'embedded-react';",
        "import {Pressable, Text, nativeModule} from 'embedded-react';",
      );
    expect(failureIn(directory, source)).toMatch(
      /the native module "can" is declared twice/,
    );
  });
});
