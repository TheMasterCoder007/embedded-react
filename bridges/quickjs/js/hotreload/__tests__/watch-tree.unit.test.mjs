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

// The dev loop's recursive watcher must fire for edits under src/** on every platform — including ones
// without fs.watch's native {recursive:true}. We test the per-directory fallback DIRECTLY (not watchTree),
// so the fallback path is exercised even here where native recursion works.

import {describe, it, expect} from 'vitest';
import {mkdtempSync, mkdirSync, writeFileSync, rmSync} from 'node:fs';
import {realpathSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {watchTreePolyfill} from '../watch-tree.mjs';

const sleep = ms => new Promise(r => setTimeout(r, ms));
// Poll instead of a fixed wait — fs.watch delivery latency varies by platform/CI load.
async function waitFor(pred, timeoutMs = 4000, stepMs = 50) {
  for (let waited = 0; waited < timeoutMs; waited += stepMs) {
    if (pred()) return true;
    await sleep(stepMs);
  }
  return pred();
}

describe('watchTree recursive polyfill', () => {
  it('fires for an edit in a nested subdirectory (the case a root-only watcher misses)', async () => {
    const root = realpathSync(mkdtempSync(join(tmpdir(), 'er-watch-')));
    mkdirSync(join(root, 'src', 'components'), {recursive: true});
    const seen = [];
    const w = watchTreePolyfill(root, (_e, name) => name && seen.push(name));
    try {
      await sleep(150); // let the per-dir watchers settle
      writeFileSync(
        join(root, 'src', 'components', 'Button.jsx'),
        'export default 1',
      );
      await waitFor(() => seen.includes('Button.jsx'));
      expect(seen).toContain('Button.jsx');
    } finally {
      w.close();
      rmSync(root, {recursive: true, force: true});
    }
  });

  it('picks up a directory created after watching started', async () => {
    const root = realpathSync(mkdtempSync(join(tmpdir(), 'er-watch-')));
    const seen = [];
    const w = watchTreePolyfill(root, (_e, name) => name && seen.push(name));
    try {
      await sleep(150);
      mkdirSync(join(root, 'late'));
      await sleep(250); // allow the root watcher to hook the new dir
      writeFileSync(join(root, 'late', 'z.jsx'), 'x');
      await waitFor(() => seen.includes('z.jsx'));
      expect(seen).toContain('z.jsx');
    } finally {
      w.close();
      rmSync(root, {recursive: true, force: true});
    }
  });

  it('close() stops delivering events', async () => {
    const root = realpathSync(mkdtempSync(join(tmpdir(), 'er-watch-')));
    let count = 0;
    const w = watchTreePolyfill(root, () => count++);
    await sleep(100);
    w.close();
    const before = count;
    writeFileSync(join(root, 'after.jsx'), 'x');
    await sleep(300);
    expect(count).toBe(before);
    rmSync(root, {recursive: true, force: true});
  });
});
