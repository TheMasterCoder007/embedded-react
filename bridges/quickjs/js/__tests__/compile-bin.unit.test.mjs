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

// Locating the bytecode precompiler for `npm run pack` and the parity harness. Only the ER_COMPILE_BIN
// branches are asserted: whether the unset search finds anything depends on which build dirs exist on
// the machine, so it's a dev-box answer, not a test.

import {afterEach, describe, expect, it} from 'vitest';
import {fileURLToPath} from 'node:url';
import {findCompileBin, compileBinHelp} from '../compile-bin.mjs';

const THIS_FILE = fileURLToPath(import.meta.url);

afterEach(() => {
  delete process.env.ER_COMPILE_BIN;
});

describe('findCompileBin', () => {
  it('returns an ER_COMPILE_BIN override that exists', () => {
    process.env.ER_COMPILE_BIN = THIS_FILE;
    expect(findCompileBin()).toBe(THIS_FILE);
  });

  it('rejects an override pointing at nothing rather than passing it on', () => {
    process.env.ER_COMPILE_BIN = `${THIS_FILE}.nope`;
    expect(findCompileBin()).toBeNull();
  });

  it('ignores an empty override and falls back to the search dirs', () => {
    process.env.ER_COMPILE_BIN = '';
    // Either a real build dir has one or none does — both are valid; what matters is that the empty
    // string isn't handed back as a path.
    expect(findCompileBin()).not.toBe('');
  });
});

describe('compileBinHelp', () => {
  it('names the bad override instead of telling you to build what you already built', () => {
    process.env.ER_COMPILE_BIN = '/nope/er-bridge-quickjs-compile';
    const help = compileBinHelp();
    expect(help).toContain('ER_COMPILE_BIN');
    expect(help).toContain('/nope/er-bridge-quickjs-compile');
    expect(help).not.toContain('cmake --build');
  });

  it('gives build instructions when nothing is overridden', () => {
    const help = compileBinHelp();
    expect(help).toContain(
      'cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile',
    );
  });
});
