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

import {describe, it, expect} from 'vitest';
import {compilePackage} from '../compile.mts';
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {nodesOf} from '../compiler/ir/nodes.mts';
import {nodeReads} from '../compiler/ir/reads.mts';

// Custom hooks, inlined at each call.

const SCREEN = {screen: {width: 240, height: 320}};

const HEAD =
  "import {useCallback, useEffect, useMemo, useState} from 'react';\n" +
  "import {Pressable, Text, View} from 'embedded-react';\n";

/** A counter hook that returns its state and a callback that bumps it. */
const COUNTER =
  'function useCounter(start, step = 1) {\n' +
  '  const [n, setN] = useState(start);\n' +
  '  const inc = useCallback(() => setN(n + step), [n]);\n' +
  '  return {n, inc};\n}\n';

/** An app module with `before` at module level and `body` as the App's body. */
const appOf = (before: string, body: string) =>
  `${HEAD}${before}\nexport function App() {\n${body}\n}`;

const compile = (source: string) => compilePackage(source, 'app', SCREEN);
const lower = (source: string) =>
  lowerApp(source, resolveOptions(SCREEN), new Map(), []);

/** Each state's JS name and slot name. */
const statesOf = (source: string) =>
  lower(source).state.map(state => [state.name, state.slotName]);

/** The slots each node reads, for the nodes that read any. */
const readsOf = (source: string) =>
  nodesOf(lower(source).root)
    .map(node => [...(nodeReads(node) ?? [])].map(state => state.slotName))
    .filter(reads => reads.length);

describe('custom hooks', () => {
  it('gives each call state of its own, under the names it destructures', () => {
    const source = appOf(
      COUNTER,
      '  const {n, inc} = useCounter(5);\n' +
        '  const {n: m, inc: incM} = useCounter(0, 2);\n' +
        '  return <View><Pressable onPress={inc}><Text>{n}</Text></Pressable>' +
        '<Pressable onPress={incM}><Text>{m}</Text></Pressable></View>;',
    );
    expect(statesOf(source)).toEqual([
      ['n', 'n'],
      ['m', 'm'],
    ]);
    expect(readsOf(source)).toEqual([['n'], ['m']]);
    // The step argument folds into each call's callback.
    const handlers = lower(source).output.handlers.map(handler =>
      JSON.stringify(handler.callback.body),
    );
    expect(handlers[0]).toContain('"value":1');
    expect(handlers[1]).toContain('"value":2');
  });

  it('reads a result bound to one name by key', () => {
    const source = appOf(
      COUNTER,
      '  const counter = useCounter(5);\n' +
        '  return <Pressable onPress={counter.inc}><Text>{counter.n}</Text></Pressable>;',
    );
    expect(statesOf(source)).toEqual([['counter:n', 'counter:n']]);
    expect(() =>
      compile(
        appOf(
          COUNTER,
          '  const counter = useCounter(5);\n' +
            '  const copy = counter;\n' +
            '  return <Text>{counter.n}</Text>;',
        ),
      ),
    ).toThrow(/the result of hook "useCounter" can only be read by key/);
  });

  it('binds an array result, a value result and a hook that returns a hook call', () => {
    const before =
      'function useToggle(on) {\n' +
      '  const [value, setValue] = useState(on);\n' +
      '  const flip = useCallback(() => setValue(!value), [value]);\n' +
      '  return [value, flip];\n}\n' +
      'function useDouble(x) { return x * 2; }\n' +
      'const useLabel = start => useState(start);\n';
    const source = appOf(
      before,
      '  const [on, flip] = useToggle(false);\n' +
        '  const [n, setN] = useState(1);\n' +
        '  const doubled = useDouble(n);\n' +
        "  const [label, setLabel] = useLabel('a');\n" +
        '  return <View><Pressable onPress={flip}><Text>{on ? doubled : 0}</Text></Pressable>' +
        "<Pressable onPress={() => setLabel(label + 'b')}><Text>{label}</Text></Pressable></View>;",
    );
    expect(statesOf(source)).toEqual([
      ['on', 'on'],
      ['n', 'n'],
      ['label', 'label'],
    ]);
    expect(readsOf(source)).toEqual([['on', 'n'], ['label']]);
  });

  it('names the locals a call does not bind after the hook and the call', () => {
    const source = appOf(
      'function useAnswer() { const [n, setN] = useState(42); return n > 40; }\n',
      '  const big = useAnswer();\n  const again = useAnswer();\n' +
        '  return <Text>{big ? again : false}</Text>;',
    );
    expect(statesOf(source)).toEqual([
      ['n@useAnswer#1', 'n@useAnswer#1'],
      ['n@useAnswer#2', 'n@useAnswer#2'],
    ]);
  });

  it('gives each component instance state of its own', () => {
    const source = appOf(
      COUNTER +
        'function Row({label}) {\n' +
        '  const {n, inc} = useCounter(0);\n' +
        '  return <Pressable onPress={inc}><Text>{label} {n}</Text></Pressable>;\n}\n',
      '  return <View><Row label="a"/><Row label="b"/></View>;',
    );
    expect(statesOf(source)).toEqual([
      ['n', 'c0_n'],
      ['n', 'c1_n'],
    ]);
  });

  it('inlines a hook that calls hooks, and effects and memos in a hook', () => {
    const before =
      COUNTER +
      'function usePair() {\n' +
      '  const a = useCounter(1);\n  const b = useCounter(10);\n' +
      '  return {total: a.n + b.n, incA: a.inc, incB: b.inc};\n}\n' +
      'function useTicker(ms) {\n' +
      '  const [ticks, setTicks] = useState(0);\n' +
      '  useEffect(() => {\n' +
      '    const id = setInterval(() => setTicks(v => v + 1), ms);\n' +
      '    return () => clearInterval(id);\n  }, []);\n' +
      '  const half = useMemo(() => ticks / 2, [ticks]);\n' +
      '  return half;\n}\n';
    const source = appOf(
      before,
      '  const {total, incA, incB} = usePair();\n' +
        '  const half = useTicker(500);\n' +
        '  return <View><Pressable onPress={incA}><Text>{total}</Text></Pressable>' +
        '<Pressable onPress={incB}><Text>{half}</Text></Pressable></View>;',
    );
    expect(statesOf(source)).toEqual([
      ['a:n@usePair#1', 'a:n@usePair#1'],
      ['b:n@usePair#1', 'b:n@usePair#1'],
      ['ticks@useTicker#1', 'ticks@useTicker#1'],
    ]);
    expect(readsOf(source)).toEqual([
      ['a:n@usePair#1', 'b:n@usePair#1'],
      ['ticks@useTicker#1'],
    ]);
    expect(lower(source).output.timerFunctions).toHaveLength(1);
  });

  it("reads an argument as the caller has it, beside a hook local of the caller's name", () => {
    const source = appOf(
      'function useShifted(value) { const n = 100; return useMemo(() => value + n, [value]); }\n',
      '  const [n, setN] = useState(1);\n' +
        '  const shifted = useShifted(n);\n' +
        '  return <Pressable onPress={() => setN(n + 1)}><Text>{shifted}</Text></Pressable>;',
    );
    expect(readsOf(source)).toEqual([['n']]);
  });

  it('refuses a call that is not at the top of a component or hook', () => {
    expect(() =>
      compile(
        appOf(
          COUNTER,
          '  const [x] = useState(1);\n' +
            '  if (x) { useCounter(1); }\n' +
            '  return <Text>x</Text>;',
        ),
      ),
    ).toThrow(
      /hook "useCounter" must be called at the top of a component or another hook/,
    );
    expect(() =>
      compile(
        appOf(
          COUNTER,
          '  return <Pressable onPress={() => useCounter(1)}><Text>x</Text></Pressable>;',
        ),
      ),
    ).toThrow(/must be called at the top/);
  });

  it('refuses recursion, early returns and parameters it would have to set', () => {
    expect(() =>
      compile(
        appOf(
          'function useA() { useB(); return 1; }\nfunction useB() { useA(); }\n',
          '  useA();\n  return <Text>x</Text>;',
        ),
      ),
    ).toThrow(/hook "useA" calls itself/);
    expect(() =>
      compile(
        appOf(
          'function useEarly(x) { if (x) return 1; return 2; }\n',
          '  const v = useEarly(true);\n  return <Text>{v}</Text>;',
        ),
      ),
    ).toThrow(/hook "useEarly" returns before its end/);
    expect(() =>
      compile(
        appOf(
          'function useBump(x) { x = x + 1; return x; }\n',
          '  const v = useBump(1);\n  return <Text>{v}</Text>;',
        ),
      ),
    ).toThrow(/hook "useBump" sets its parameter "x"/);
    expect(() =>
      compile(
        appOf(
          'function usePoint({x}) { return x; }\n',
          '  const v = usePoint({x: 1});\n  return <Text>{v}</Text>;',
        ),
      ),
    ).toThrow(/hook "usePoint" can only take named parameters/);
  });

  it('refuses a name the hook reads from the module that the caller hides', () => {
    expect(() =>
      compile(
        appOf(
          'const STEP = 2;\nfunction useStep() { const [n] = useState(STEP); return n; }\n',
          '  const STEP = 5;\n  const v = useStep();\n  return <Text>{v}{STEP}</Text>;',
        ),
      ),
    ).toThrow(/hook "useStep" reads "STEP", which App declares too/);
    expect(() =>
      compile(
        appOf(
          'function useLater(read) { return useMemo(() => { const n = 1; return read() + n; }, []); }\n',
          '  const [n] = useState(3);\n' +
            '  const v = useLater(() => n);\n  return <Text>{v}</Text>;',
        ),
      ),
    ).toThrow(
      /an argument to hook "useLater" reads "n", which useLater declares too/,
    );
  });

  it('refuses a key the result does not have', () => {
    expect(() =>
      compile(
        appOf(
          COUNTER,
          '  const {n, reset} = useCounter(1);\n  return <Text>{n}</Text>;',
        ),
      ),
    ).toThrow(/hook "useCounter" returns no "reset"/);
  });
});
