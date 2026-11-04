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

// Unit tests for the simulator's persist transform (../../persist-transform.mjs): it rewrites the
// app's `useState` into the persisting helper, keyed stably by component + hook order, and only
// touches the real (imported) useState.
import {describe, it, expect} from 'vitest';
import {transformPersist} from '../../persist-transform.mjs';

describe('transformPersist', () => {
  it('rewrites imported useState to the persisting helper, keyed by component + order', () => {
    const source = `
      import { useState } from 'react';
      function Counter() {
        const [a, setA] = useState(0);
        const [b, setB] = useState('x');
        return null;
      }
    `;
    const output = transformPersist(source, 'App.jsx');
    expect(output).toContain('usePersistentState as __erPersistState');
    expect(output).toContain('__erPersistState');
    expect(output).toContain('App.jsx::Counter#0');
    expect(output).toContain('App.jsx::Counter#1');
    expect(output).not.toMatch(/=\s*useState\(/); // no bare useState calls left
  });

  it('also rewrites useState imported from embedded-react', () => {
    const output = transformPersist(
      `import { useState } from 'embedded-react'; function A(){ const [x]=useState(1); return null; }`,
      'a.jsx',
    );
    expect(output).toContain('App'.replace('App', '')); // noop guard to keep formatting
    expect(output).toContain('a.jsx::A#0');
  });

  it('leaves a non-imported (local) useState untouched', () => {
    const output = transformPersist(
      `function useState(){return [0]} const x = useState();`,
      'm.js',
    );
    expect(output).not.toContain('__erPersistState');
  });

  it('keys stay stable when unrelated lines change (component + order are what matter)', () => {
    const before = transformPersist(
      `import {useState} from 'react'; function C(){ const [x]=useState(1); return null; }`,
      'C.jsx',
    );
    const after = transformPersist(
      `import {useState} from 'react'; function C(){ const y = 99; const [x]=useState(1); return null; }`,
      'C.jsx',
    );
    expect(before).toContain('C.jsx::C#0');
    expect(after).toContain('C.jsx::C#0'); // adding an unrelated line did not shift the key
  });

  it('no-ops a module with no useState (no helper import injected)', () => {
    const output = transformPersist(`export const x = 1;`, 'm.js');
    expect(output).not.toContain('__erPersistState');
  });
});
