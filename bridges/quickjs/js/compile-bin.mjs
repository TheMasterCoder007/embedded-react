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

const SEARCH_DIRS = [
  'bridges/quickjs/build',
  'examples/linux/build/bridges/quickjs',
  'tools/simulator/build/bridges/quickjs',
];

/**
 * Returns the precompiler's path, or null when there isn't one.
 *
 * ER_COMPILE_BIN wins, but it still has to point at a file that exists: an override with a typo in it
 * used to sail through every caller's check and only fail later, deep in a build, as a bare ENOENT
 * from spawn. Returning null instead routes it to compileBinHelp(), which names the real problem.
 */
export function findCompileBin() {
  const override = process.env.ER_COMPILE_BIN;
  if (override) {
    return existsSync(override) ? override : null;
  }
  return (
    SEARCH_DIRS.map(d =>
      resolve(repoRoot, d, `er-bridge-quickjs-compile${exe}`),
    ).find(existsSync) || null
  );
}

/** What to print when findCompileBin() comes back null — a bad override is a different problem. */
export function compileBinHelp() {
  const override = process.env.ER_COMPILE_BIN;
  if (override) {
    return (
      `ER_COMPILE_BIN is set to "${override}", but there is no file there.\n` +
      'Point it at a built er-bridge-quickjs-compile, or unset it to search the usual build dirs.'
    );
  }
  return [
    'Bytecode precompiler (er-bridge-quickjs-compile) not found. Build it once:',
    '  cmake -S bridges/quickjs -B bridges/quickjs/build',
    '  cmake --build bridges/quickjs/build --target er-bridge-quickjs-compile',
    '(or set ER_COMPILE_BIN to the binary path)',
  ].join('\n');
}
