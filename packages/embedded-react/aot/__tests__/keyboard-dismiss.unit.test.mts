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

import {describe, expect, it} from 'vitest';
import {compilePackage} from '../compile.mts';
import {Table} from '../compiler/package/format.mts';
import {OP} from '../compiler/package/bytecode.mts';
import type {CompileOptions} from '../compiler/options.mts';

// Keyboard.dismiss(): the BLUR it lowers to, and what the compiler refuses. That the keyboard hides as Flow A's
// does is the frame tests' job (erb-frames, keyboard-dismiss).

const OPTIONS: CompileOptions = {
  filename: 'App.tsx',
  screen: {width: 240, height: 320},
};

/** An app with `handler` as a button's onPress body, and `input` as its TextInput. */
const appOf = (handler: string, input = '<TextInput />') =>
  "import {useState} from 'react';\n" +
  "import {Keyboard, Pressable, Text, TextInput, View} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const [count, setCount] = useState(0);\n' +
  '  return (\n' +
  '    <View>\n' +
  `      <Pressable onPress={() => { ${handler} }}><Text>{count}</Text></Pressable>\n` +
  `      ${input}\n` +
  '    </View>\n' +
  '  );\n' +
  '}\n';

const compile = (source: string) => compilePackage(source, 'app', OPTIONS);

/** The message a compile fails with. */
const failure = (source: string) => {
  try {
    compile(source);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

/**
 * How many BLUR instructions a program has.
 *
 * @param bytes  The program section.
 *
 * @returns The count.
 */
function blursOf(bytes: Uint8Array): number {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const u16 = (offset: number) => view.getUint16(offset, true);
  const u32 = (offset: number) => view.getUint32(offset, true);
  const directory = 24 + u16(18) * 16 + u16(20) * 8;
  let code = {offset: 0, length: 0};
  for (let index = 0; index < u16(22); index++) {
    const entry = directory + index * 12;
    if (u16(entry) === Table.code) {
      code = {offset: u32(entry + 4), length: u32(entry + 8)};
    }
  }
  const words = Object.values(OP);
  let blurs = 0;
  for (let at = 0; at < code.length / 4;) {
    const opcode = u32(code.offset + at * 4) & 0xff;
    if (opcode === OP.BLUR.code) blurs++;
    at += words.find(candidate => candidate.code === opcode)!.words;
  }
  return blurs;
}

describe('Keyboard.dismiss() in a package', () => {
  it('lowers to a BLUR', () => {
    const {program} = compile(appOf('Keyboard.dismiss();'));
    expect(blursOf(program)).toBe(1);
    expect(blursOf(compile(appOf('setCount(1);')).program)).toBe(0);
  });

  it('runs among other statements, and under a condition', () => {
    const {program} = compile(
      appOf('setCount(count + 1); if (count > 2) { Keyboard.dismiss(); }'),
    );
    expect(blursOf(program)).toBe(1);
  });

  it('compiles beside an onBlur, whose event waits for the handler to end', () => {
    const {program} = compile(
      appOf('Keyboard.dismiss();', '<TextInput onBlur={() => setCount(0)} />'),
    );
    expect(blursOf(program)).toBe(1);
  });

  it('refuses an argument', () => {
    expect(failure(appOf('Keyboard.dismiss(true);'))).toMatch(
      /Keyboard\.dismiss\(\) takes no arguments/,
    );
  });

  it('refuses the rest of Keyboard, pointing at onFocus and onBlur', () => {
    const message = failure(
      appOf("Keyboard.addListener('keyboardDidShow', () => setCount(1));"),
    );
    expect(message).toMatch(/Keyboard\.addListener\(\) is not in the subset/);
    expect(message).toMatch(/onFocus and onBlur/);
  });
});
