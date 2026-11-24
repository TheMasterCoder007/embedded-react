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

// Every page that drives the browser simulator binds its functions by name. This checks each binding
// against the functions the simulator declares and exports, and the number of arguments it passes.
import {describe, expect, it} from 'vitest';
import {readFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const repositoryRoot = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../..',
);
const read = (path: string) =>
  readFileSync(resolve(repositoryRoot, path), 'utf8');

const HOSTS = [
  'website/src/components/Playground/useSimulator.ts',
  'tools/web-sim/index.html',
  'packages/embedded-react/qjsc-wasm.mts',
];

/** The Flow B simulator's host, which calls its functions as the module's own. */
const FLOW_B_HOST = 'packages/embedded-react/aot-sim/worker.mts';

// Function name → parameter count, from the headers' prototypes.
const declared = new Map<string, number>();
for (const header of [
  'backends/web/web_backend.h',
  'backends/web/aot_backend.h',
]) {
  for (const [, name, parameters] of read(header).matchAll(
    /^\s*(?:const\s+)?\w+\*?\s+(er_web_\w+)\(([^)]*)\);\s*$/gm,
  )) {
    const list = parameters.trim();
    declared.set(name, list === 'void' ? 0 : list.split(',').length);
  }
}

const exported = new Set(
  [...read('tools/web-sim/CMakeLists.txt').matchAll(/_(er_web_\w+)/g)].map(
    ([, name]) => name,
  ),
);

/**
 * How many arguments a call passes.
 *
 * @param source  The source the call is in.
 * @param from  Where its arguments start, after the opening parenthesis.
 *
 * @returns The count. A comma after the last argument adds none.
 */
function argumentCount(source: string, from: number): number {
  const passed = [''];
  let depth = 0;
  for (let at = from; at < source.length; at++) {
    const character = source[at];
    if (character === ')' && depth === 0) break;
    if ('([{'.includes(character)) depth++;
    if (')]}'.includes(character)) depth--;
    if (character === ',' && depth === 0) passed.push('');
    else passed[passed.length - 1] += character;
  }
  return passed.filter(argument => argument.trim()).length;
}

describe('browser simulator bindings', () => {
  it('reads the declared and exported functions', () => {
    expect(declared.size).toBeGreaterThan(0);
    expect([...declared.keys()].sort()).toEqual([...exported].sort());
  });

  it.each(HOSTS)('%s binds functions the simulator has', host => {
    const source = read(host);
    const bindings = [
      ...source.matchAll(
        /cwrap\(\s*'(er_web_\w+)',\s*(?:'number'|null),\s*\[([^\]]*)\]/g,
      ),
    ];
    expect(bindings.length).toBeGreaterThan(0);
    for (const [, name, argumentTypes] of bindings) {
      expect(exported, name).toContain(name);
      const argumentCount = [...argumentTypes.matchAll(/'number'/g)].length;
      expect(argumentCount, name).toBe(declared.get(name));
    }
    for (const [, name] of source.matchAll(/\b_(er_web_\w+)/g)) {
      expect(exported, name).toContain(name);
    }
  });

  it('the Flow B host calls functions the simulator has', () => {
    const source = read(FLOW_B_HOST);
    const calls = [...source.matchAll(/simulator\._(er_web_\w+)\(/g)];
    expect(calls.length).toBeGreaterThan(0);
    for (const call of calls) {
      const name = call[1];
      expect(exported, name).toContain(name);
      expect(argumentCount(source, call.index + call[0].length), name).toBe(
        declared.get(name),
      );
    }
  });
});
