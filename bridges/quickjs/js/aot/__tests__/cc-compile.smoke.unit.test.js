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

import { describe, it, expect } from 'vitest';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { compileSource, bakeSvgArtifacts } from '../compile.mjs';

// AOT codegen text is unit-tested by regex, but a regex can't catch a generated call that no longer matches
// an engine signature (e.g., a stale er_node_set_vector_ops arity). This smoke test closes that gap: it
// actually runs a C compiler over the generated app.gen.c. It targets the thermostat's compact (240×320)
// branch because that exercises the newest emission — a baked <Svg source> conic face (an ERVectorGradient
// table) layered under a state-driven arc. The compile step skips when no C compiler is present (so the
// suite still passes in a toolchain-less environment), but the bake+compile-to-C step always runs.
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../..');
const demosDir = join(root, 'demos');
const engineInc = join(root, 'engine', 'include');
const engineCore = join(root, 'engine', 'core');

/** Bake the thermostat's <Svg source> imports and AOT-compile its compact (240×320) branch to C. */
async function emitThermostat() {
  const src = readFileSync(join(demosDir, 'thermostat', 'App.jsx'), 'utf8');
  const svgArtifacts = await bakeSvgArtifacts(src, join(demosDir, 'thermostat'));
  return compileSource(src, 'thermostat', { svgArtifacts, screen: { width: 240, height: 320 }, filename: 'demos/thermostat/App.jsx' });
}

/** First working C compiler from a small candidate list, or null. Tries the repo's MinGW first. */
function findCC() {
  for (const cc of ['C:\\mingw32\\bin\\gcc.exe', 'gcc', 'cc', 'clang']) {
    try {
      if (spawnSync(cc, ['--version'], { stdio: 'ignore' }).status === 0) return cc;
    } catch {
      /* not on PATH — try the next */
    }
  }
  return null;
}
const CC = findCC();

describe('AOT generated C compiles', () => {
  it('emits the thermostat compact dial (baked <Svg source> + conic gradient table)', async () => {
    const r = await emitThermostat();
    expect(r.c).toContain('static const ERVectorGradient s_svg0_grads[] = {');
    expect(r.c).toContain('.type = 3'); // the conic ghost track
    expect(r.c).toMatch(/er_node_set_vector_ops\(n\d+, s_svg0_ops, \d+, s_svg0_paints, \d+, s_svg0_grads, 1\);/);
  });

  (CC ? it : it.skip)(`the generated C passes a C compiler syntax check (${CC || 'no cc found'})`, async () => {
    const r = await emitThermostat();
    const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-'));
    try {
      writeFileSync(join(dir, 'app.gen.c'), r.c);
      writeFileSync(join(dir, 'app.gen.h'), r.h);
      // -fsyntax-only: the struct (ERVectorGradient) + signature (er_node_set_vector_ops) are unconditional
      // in the engine headers, so this validates the codegen against the real API with no gradient flags.
      const res = spawnSync(CC, ['-fsyntax-only', '-I', engineInc, '-I', engineCore, join(dir, 'app.gen.c')], { encoding: 'utf8' });
      expect(res.stderr || '').toBe('');
      expect(res.status).toBe(0);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
