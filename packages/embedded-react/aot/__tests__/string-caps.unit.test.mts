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
import {lowerApp} from '../compiler/lower.mts';
import {resolveOptions} from '../compiler/options.mts';
import {internedStates} from '../compiler/package/enums.mts';
import {stringCaps} from '../compiler/package/string-caps.mts';
import type {ListState, ScalarState} from '../compiler/types.mts';

// Each string buffer is sized from the values that reach it; one built from itself takes the default.

const DEFAULT_CAP = 47;

/** An app module around a component body and the JSX it returns. */
const appOf = (body: string, jsx: string): string =>
  "import {useState} from 'react';\n" +
  "import {Pressable, Text, TextInput, View} from 'embedded-react';\n" +
  `export function App() {\n${body}\n  return ${jsx};\n}`;

/** The caps of an app's string states, list fields and locals. */
function capsOf(body: string, jsx: string) {
  const app = lowerApp(appOf(body, jsx), resolveOptions({}), new Map(), []);
  const caps = stringCaps(app, internedStates(app), DEFAULT_CAP);
  const stateNamed = (name: string) =>
    app.state.find(state => state.name === name)!;
  return {
    state: (name: string) => caps.state(stateNamed(name) as ScalarState),
    field: (name: string, key: string) =>
      caps.field(stateNamed(name) as ListState, key),
    local: (name: string) => caps.local(name),
  };
}

describe('string caps', () => {
  it('fits a state to its initial value and each write', () => {
    const caps = capsOf(
      "  const [count, setCount] = useState(0);\n  const [label, setLabel] = useState('none yet');",
      "<Pressable onPress={() => { setLabel('count ' + count); setCount(count + 1); }}><Text>{label}</Text></Pressable>",
    );
    // 'count ' and an int's 11 characters.
    expect(caps.state('label')).toBe(17);
  });

  it('counts the most a number prints, with toFixed and padStart', () => {
    const caps = capsOf(
      "  const [level, setLevel] = useState(0.5);\n  const [count, setCount] = useState(0);\n  const [label, setLabel] = useState('');",
      '<Pressable onPress={() => { setLabel(`${level.toFixed(1)}° ${String(count).padStart(14, "0")}`); setLevel(level + 1); setCount(count + 1); }}><Text>{label}</Text></Pressable>',
    );
    // 22 digits and '.5', '° ' (3 bytes), then the int padded to 14.
    expect(caps.state('label')).toBe(24 + 3 + 14);
  });

  it("fits a list's string field to its rows and appends", () => {
    const caps = capsOf(
      "  const [items, setItems] = useState([{label: 'first one', n: 1}]);",
      "<Pressable onPress={() => setItems([...items, {label: 'row ' + items.length, n: 2}])}><Text>{items.length}</Text></Pressable>",
    );
    expect(caps.field('items', 'label')).toBe(15);
  });

  it('fits a local to its value, and a state to what it copies', () => {
    const caps = capsOf(
      "  const [count, setCount] = useState(0);\n  const [label, setLabel] = useState('');",
      '<Pressable onPress={() => { const line = `#${count}`; setLabel(line); setCount(count + 1); }}><Text>{label}</Text></Pressable>',
    );
    expect(caps.local('local_line')).toBe(12);
    expect(caps.state('label')).toBe(12);
  });

  it("holds a <TextInput>'s whole text, unless a slice cuts it", () => {
    const caps = capsOf(
      "  const [name, setName] = useState('');\n  const [short, setShort] = useState('');",
      '<TextInput onChangeText={text => { setName(text); setShort(text.slice(0, 20)); }} />',
    );
    expect(caps.state('name')).toBe(255);
    // Up to 3 bytes for each of 20 characters the text could hold.
    expect(caps.state('short')).toBe(60);
  });

  it('gives a string built from itself the default cap', () => {
    const caps = capsOf(
      "  const [log, setLog] = useState('');\n  const [copy, setCopy] = useState('');",
      "<Pressable onPress={() => { setLog(log + '.'); setCopy(log); }}><Text>{log}{copy}</Text></Pressable>",
    );
    expect(caps.state('log')).toBe(DEFAULT_CAP);
    expect(caps.state('copy')).toBe(DEFAULT_CAP);
  });

  it('bounds a string built from itself that a slice keeps short', () => {
    const caps = capsOf(
      "  const [log, setLog] = useState('');",
      "<Pressable onPress={() => setLog(log.slice(-39) + '.')}><Text>{log}</Text></Pressable>",
    );
    expect(caps.state('log')).toBe(40);
  });

  it('bounds a string padded from itself by the pad width', () => {
    const caps = capsOf(
      "  const [log, setLog] = useState('');",
      "<Pressable onPress={() => setLog(log.padStart(100, '.'))}><Text>{log}</Text></Pressable>",
    );
    expect(caps.state('log')).toBe(100);
  });

  it('does not count strings that only copy each other as built from themselves', () => {
    const long = 'x'.repeat(60);
    const caps = capsOf(
      `  const [first, setFirst] = useState('${long}');\n  const [second, setSecond] = useState('');`,
      '<Pressable onPress={() => { setSecond(first); setFirst(second); }}><Text>{first}{second}</Text></Pressable>',
    );
    expect(caps.state('first')).toBe(60);
    expect(caps.state('second')).toBe(60);
  });

  it('finds a row handler that appends to its own field built from itself', () => {
    const caps = capsOf(
      "  const [items, setItems] = useState([{t: 'a'}]);",
      "<View>{items.map((item, index) => <Pressable key={index} onPress={() => setItems([...items, {t: item.t + '!'}])}><Text>{item.t}</Text></Pressable>)}</View>",
    );
    expect(caps.field('items', 't')).toBe(DEFAULT_CAP);
  });
});
