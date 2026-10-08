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

/*
 * Runs the AOT unit tests of the package at AOT_SAME_ROOT with its compiler wrapped by record.mjs (see
 * aot-same.mjs). One fork, so the records are appended one at a time.
 */
import {existsSync} from 'node:fs';
import {fileURLToPath} from 'node:url';

const root = process.env.AOT_SAME_ROOT;

export default {
  root,
  test: {
    include: ['aot/**/__tests__/**/*.unit.test.{js,mjs,mts}'],
    environment: 'node',
    pool: 'forks',
    poolOptions: {forks: {singleFork: true}},
  },
  resolve: {
    alias: [
      {
        // The entry is compile.mts; a commit from before it moved has compile.mjs.
        find: /^\.\.\/compile\.m[jt]s$/,
        replacement: fileURLToPath(new URL('./record.mjs', import.meta.url)),
      },
      {
        find: 'aot-same-real-compiler',
        replacement: existsSync(`${root}/aot/compile.mts`)
          ? `${root}/aot/compile.mts`
          : `${root}/aot/compile.mjs`,
      },
    ],
  },
  esbuild: {jsx: 'automatic'},
};
