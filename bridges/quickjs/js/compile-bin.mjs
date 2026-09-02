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

// compile-bin.mjs — finds the QuickJS bytecode precompiler (er-bridge-quickjs-compile).
//
// pack-container.mjs needs it to turn a bundle into the bytecode a container carries, and parity.mjs
// preflights it because Flow A renders from a packed container. Both look in the same build dirs and
// honour the same ER_COMPILE_BIN override, so the search lives here instead of in each caller.

import {existsSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const exe = process.platform === 'win32' ? '.exe' : '';

/** Returns the precompiler's path (ER_COMPILE_BIN wins), or null when it hasn't been built. */
export function findCompileBin() {
  return (
    process.env.ER_COMPILE_BIN ||
    [
      resolve(
        repoRoot,
        'bridges/quickjs/build',
        `er-bridge-quickjs-compile${exe}`,
      ),
      resolve(
        repoRoot,
        'examples/linux/build/bridges/quickjs',
        `er-bridge-quickjs-compile${exe}`,
      ),
      resolve(
        repoRoot,
        'tools/simulator/build/bridges/quickjs',
        `er-bridge-quickjs-compile${exe}`,
      ),
    ].find(existsSync) ||
    null
  );
}

/** What to print when findCompileBin() comes back null. */
export const COMPILE_BIN_HELP = [
  'Bytecode precompiler (er-bridge-quickjs-compile) not found. Build it once:',
  '  cmake -S bridges/quickjs -B bridges/quickjs/build',
  '  cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile',
  '(or set ER_COMPILE_BIN to the binary path)',
].join('\n');
