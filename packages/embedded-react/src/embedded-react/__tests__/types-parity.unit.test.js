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
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';

// Guards the shipped public types against the runtime: every value exported from index.js must have a
// matching value export in index.d.ts, and vice versa. `npm run typecheck` only proves the .d.ts compiles
// — it can't notice a NEW runtime export that nobody typed (it just resolves to `any`). This catches that:
// add a component to index.js and forget index.d.ts (or remove one and forget the type), and this fails.
//
// Only VALUE exports are compared. Type-only exports of the .d.ts (interface / type) have no runtime
// counterpart and are intentionally ignored.

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(HERE, '..');

/** Strip // line and block comments so an `export` inside a comment is never mistaken for a real one. */
const stripComments = src =>
  src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

/** Value-export names of the runtime entry (index.js): `export { A, B as C } from '...'` + direct decls. */
function runtimeExports(src) {
  const code = stripComments(src);
  const names = new Set();
  // Re-export blocks: export { A, B as C } from './x';  and local export { A, B };
  for (const m of code.matchAll(/export\s*\{([^}]*)\}/g)) {
    for (const part of m[1].split(',')) {
      const name = part
        .trim()
        .split(/\s+as\s+/)
        .pop()
        ?.trim();
      if (name) names.add(name);
    }
  }
  // Direct declarations: export const/let/var/function/class/async function NAME.
  for (const m of code.matchAll(
    /export\s+(?:async\s+)?(?:const|let|var|function|class)\s+([A-Za-z_$][\w$]*)/g,
  )) {
    names.add(m[1]);
  }
  // `export * from` can't be enumerated statically — fail loudly rather than silently under-report.
  if (/export\s+\*\s+from/.test(code)) {
    throw new Error(
      'index.js uses `export * from` — the parity test cannot enumerate those names statically.',
    );
  }
  return names;
}

/** Value-export names of the declarations (index.d.ts): const / function / class only — NOT interface/type. */
function declaredValueExports(src) {
  const code = stripComments(src);
  const names = new Set();
  for (const m of code.matchAll(
    /export\s+(?:declare\s+)?(?:const|function|class)\s+([A-Za-z_$][\w$]*)/g,
  )) {
    names.add(m[1]);
  }
  return names;
}

describe('embedded-react public types match the runtime surface', () => {
  const js = readFileSync(resolve(SRC, 'index.js'), 'utf8');
  const dts = readFileSync(resolve(SRC, 'index.d.ts'), 'utf8');
  const runtime = runtimeExports(js);
  const declared = declaredValueExports(dts);

  it('parses a non-trivial set of exports from both files (sanity)', () => {
    expect(runtime.size).toBeGreaterThan(10);
    expect(declared.size).toBeGreaterThan(10);
  });

  it('has no runtime export missing from index.d.ts', () => {
    const missing = [...runtime].filter(n => !declared.has(n)).sort();
    expect(
      missing,
      `index.js exports these with no value declaration in index.d.ts: ${missing.join(', ')}`,
    ).toEqual([]);
  });

  it('has no index.d.ts value export absent from the runtime', () => {
    const extra = [...declared].filter(n => !runtime.has(n)).sort();
    expect(
      extra,
      `index.d.ts declares these as values but index.js does not export them (make them \`type\`/\`interface\`, or export them): ${extra.join(', ')}`,
    ).toEqual([]);
  });
});
