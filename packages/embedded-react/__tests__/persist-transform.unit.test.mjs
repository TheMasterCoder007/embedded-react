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
import {shouldPersist, transformPersist} from '../persist-transform.mjs';

// Regression guard for the "works in the repo, crashes after publish" stack overflow: the persist
// transform must rewrite ONLY the consumer's own app files, never anything in node_modules. In a
// published install the library sits at <project>/node_modules/embedded-react — i.e. UNDER the project
// root — so a project-root check alone wrongly transformed the library's own usePersistentState,
// making it recurse into itself ("Maximum call stack size exceeded").
describe('shouldPersist', () => {
  const root = '/home/me/my-app';

  it("transforms the app's own source under the project root", () => {
    expect(shouldPersist('/home/me/my-app/App.jsx', root)).toBe(true);
    expect(shouldPersist('/home/me/my-app/src/screens/Home.jsx', root)).toBe(
      true,
    );
  });

  it('does NOT transform dependencies in node_modules (the bug)', () => {
    expect(
      shouldPersist(
        '/home/me/my-app/node_modules/embedded-react/src/embedded-react/usePersistentState.js',
        root,
      ),
    ).toBe(false);
    expect(
      shouldPersist('/home/me/my-app/node_modules/react/index.js', root),
    ).toBe(false);
  });

  it('does NOT transform files outside the project root', () => {
    expect(shouldPersist('/home/me/other/App.jsx', root)).toBe(false);
  });

  it('handles Windows backslash paths', () => {
    const winRoot = 'C:/Users/me/my-app';
    expect(shouldPersist('C:\\Users\\me\\my-app\\App.jsx', winRoot)).toBe(true);
    expect(
      shouldPersist(
        'C:\\Users\\me\\my-app\\node_modules\\embedded-react\\sim.js',
        winRoot,
      ),
    ).toBe(false);
  });
});

// The dev hot-reload transform runs on TypeScript app files too (the create-embedded-react --ts
// template). It must parse TS syntax and leave the type annotations in place for esbuild's ts/tsx
// loader to strip downstream — not choke on them (the jsx-only parser did).
describe('transformPersist with TypeScript', () => {
  it('rewrites useState in a .tsx file and preserves JSX + type syntax', () => {
    const out = transformPersist(
      `import {useState} from 'react';
       type Props = {label: string};
       export function App({label}: Props) {
         const [count, setCount] = useState<number>(0);
         return <text>{label} {count}</text>;
       }`,
      'App.tsx',
    );
    expect(out).toContain('__erPersistState'); // useState was rewritten
    expect(out).toContain('Props'); // TS type kept for esbuild to strip
    expect(out).toContain('<text>'); // JSX preserved
  });

  it('parses a plain .ts file (no JSX) with type assertions', () => {
    const out = transformPersist(
      `export const n: number = <number>1; export type T = string;`,
      'util.ts',
    );
    expect(out).toContain('export type T'); // parsed and re-emitted, not a throw
  });
});
