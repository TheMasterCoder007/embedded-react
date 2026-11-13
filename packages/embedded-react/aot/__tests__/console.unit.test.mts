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

// console.log / warn / error: the line each call builds, and the FMTLOG that sends it. Whether the runtime hands it
// to the host is runtime/tests' job (erb_vm_test.c).

const OPTIONS: CompileOptions = {
  filename: 'App.tsx',
  screen: {width: 240, height: 320},
};

/** An app with `handler` as a button's onPress body, and `body` before the App's return. */
const appOf = (handler: string, body = '') =>
  "import {useEffect, useState} from 'react';\n" +
  "import {Pressable, Text} from 'embedded-react';\n" +
  'export function App() {\n' +
  '  const [count, setCount] = useState(0);\n' +
  "  const [name, setName] = useState('ada');\n" +
  '  const [on, setOn] = useState(false);\n' +
  body +
  `  return <Pressable onPress={() => { ${handler} }}><Text>{count}{name}</Text></Pressable>;\n` +
  '}\n';

const compile = (source: string, options: CompileOptions = {}) =>
  compilePackage(source, 'app', {...OPTIONS, ...options});

/** The message a compile fails with. */
const failure = (source: string) => {
  try {
    compile(source);
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('it compiled');
};

const OP_NAMES = Object.fromEntries(
  Object.entries(OP).map(([name, {code}]) => [code, name]),
);

/**
 * The lines a program logs: the pieces each is built from, a literal as its text and a value as its opcode.
 *
 * @param bytes  The program section.
 *
 * @returns Each FMTLOG's pieces, in code order.
 */
function linesOf(bytes: Uint8Array): string[][] {
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
  const code = tables.get(Table.code)!;
  const lines: string[][] = [];
  let pieces: string[] = [];
  for (let at = 0; at < code.length / 4;) {
    const word = u32(code.offset + at * 4);
    const name = OP_NAMES[word & 0xff];
    if (name === 'FMTLOG') {
      lines.push(pieces);
      pieces = [];
    } else if (name === 'FMTK') {
      pieces.push(string(word >>> 16));
    } else if (name.startsWith('FMT')) {
      // Another FMT that ends a text starts the next one.
      if (['FMTSET', 'FMTTEXT', 'FMTSPAN'].includes(name)) pieces = [];
      else pieces.push(name);
    }
    at += OP[name as keyof typeof OP].words;
  }
  return lines;
}

describe('console in a package', () => {
  it('sends console.log, warn and error to the host log', () => {
    const {program} = compile(
      appOf("console.log('tap'); console.warn('low'); console.error('bad');"),
    );
    expect(linesOf(program)).toEqual([['tap'], ['low'], ['bad']]);
  });

  it('prints each argument as JS does, a space between them', () => {
    const {program} = compile(
      appOf("console.log('count', count, name, on, `n=${count + 1}`);"),
    );
    expect(linesOf(program)).toEqual([
      ['count', ' ', 'FMTI', ' ', 'FMTE', ' ', 'FMTB', ' ', 'n=', 'FMTI'],
    ]);
  });

  it('logs an empty line for no arguments', () => {
    expect(linesOf(compile(appOf('console.log();')).program)).toEqual([[]]);
  });

  it('logs beside a state change, in an effect and in a timer', () => {
    const {program} = compile(
      appOf(
        "setCount(count + 1); console.log('tapped');",
        "  useEffect(() => { console.log('mounted'); const id = setInterval(() => console.log('tick'), 1000); " +
          'return () => clearInterval(id); }, []);\n',
      ),
    );
    expect(linesOf(program).flat().sort()).toEqual([
      'mounted',
      'tapped',
      'tick',
    ]);
  });

  it('leaves the calls out of a release package', () => {
    const source = (handler: string) =>
      appOf(`setCount(count + 1); ${handler}`);
    const logged = compile(source("console.log('count', count);"), {
      release: true,
    });
    expect(linesOf(logged.program)).toEqual([]);
    expect(logged.program).toEqual(
      compile(source(''), {release: true}).program,
    );
  });

  it('refuses a console method Flow A does not have', () => {
    expect(failure(appOf("console.table('x');"))).toContain(
      'console.table is not supported',
    );
  });

  it('leaves a binding named console alone', () => {
    for (const binding of [
      '  const [console, setConsole] = useState(0);\n',
      '  const console = {log: 1};\n',
    ]) {
      expect(failure(appOf("console.log('tap');", binding))).toContain(
        'a handler statement must be a state setter',
      );
    }
    expect(failure(appOf("const console = 1; console.log('tap');"))).toContain(
      'a handler statement must be a state setter',
    );
  });

  it('refuses an object or a list, constant or not', () => {
    for (const argument of ['{x: 1}', '[1, 2]', 'POINT', 'point']) {
      expect(
        failure(
          appOf(
            `console.log(${argument});`,
            '  const [point, setPoint] = useState({x: 1, y: 2});\n',
          ).replace(
            'export function',
            'const POINT = {x: 1};\nexport function',
          ),
        ),
      ).toMatch(/cannot print an object or a list|is an object/);
    }
  });

  it('refuses a spread argument', () => {
    expect(failure(appOf('console.log(...[1, 2]);'))).toContain(
      'console.* cannot take a spread',
    );
  });
});
