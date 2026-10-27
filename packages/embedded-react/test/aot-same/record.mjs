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
 * Stands in for aot/compile.mts while `npm run aot:baseline` runs the AOT tests: every compileSource call
 * is appended to the corpus (its arguments and the ER_AOT_* environment it ran under), then passed through
 * to the real compiler unchanged. vitest.config.mjs routes the tests' `../compile.mts` imports here.
 */
import {appendFileSync} from 'node:fs';
import * as real from 'aot-same-real-compiler';

export * from 'aot-same-real-compiler';

/** The running test's name, outermost describe first. */
function testName() {
  const parts = [];
  for (
    let test = globalThis.__vitest_worker__?.current;
    test;
    test = test.suite
  ) {
    if (test.name && !test.filepath) {
      parts.unshift(test.name); // a file is a suite too; leave its path out
    }
  }
  return parts.join(' > ') || null;
}

export function compileSource(src, demo, opts) {
  const env = Object.fromEntries(
    Object.entries(process.env).filter(([name]) => name.startsWith('ER_AOT_')),
  );
  // A left-out argument stays out (JSON drops undefined keys), so the replay hits compileSource's defaults.
  const record = {src, demo, opts, env, test: testName()};
  appendFileSync(process.env.AOT_SAME_CORPUS, JSON.stringify(record) + '\n');
  return real.compileSource(src, demo, opts);
}
