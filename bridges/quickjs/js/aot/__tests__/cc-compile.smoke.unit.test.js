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
 * AOT codegen text is unit-tested by regex, but a regex can't catch a generated call that no longer matches
 * an engine signature (e.g., a stale er_node_set_vector_ops arity). This smoke test closes that gap: it
 * actually runs a C compiler over the generated app.gen.c. It targets the thermostat's solo (240×320)
 * branch because that exercises a broad slice of the emission. The compiler step skips when no C compiler
 * is present (so the suite still passes in a toolchain-less environment), but the compile-to-C always runs.
 */

import {describe, it, expect} from 'vitest';
import {readFileSync, writeFileSync, mkdtempSync, rmSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
import {compileSource, bakeSvgArtifacts} from '../compile.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../..');
const demosDir = join(root, 'demos');
const engineInc = join(root, 'engine', 'include');
const engineCore = join(root, 'engine', 'core');

/** Bake the thermostat's <Svg source> imports and AOT-compile its compact (240×320) branch to C. */
async function emitThermostat() {
  const src = readFileSync(join(demosDir, 'thermostat', 'App.jsx'), 'utf8');
  const svgArtifacts = await bakeSvgArtifacts(
    src,
    join(demosDir, 'thermostat'),
  );
  return compileSource(src, 'thermostat', {
    svgArtifacts,
    screen: {width: 240, height: 320},
    filename: 'demos/thermostat/App.jsx',
  });
}

/** First, working C compiler from a small candidate list, or null. Tries the repo's MinGW first. */
function findCC() {
  for (const cc of ['C:\\mingw32\\bin\\gcc.exe', 'gcc', 'cc', 'clang']) {
    try {
      if (spawnSync(cc, ['--version'], {stdio: 'ignore'}).status === 0)
        return cc;
    } catch {
      /* not on PATH — try the next */
    }
  }
  return null;
}
const CC = findCC();

describe('AOT generated C compiles', () => {
  it('emits the thermostat solo dial as a native, state-driven arc node', async () => {
    const r = await emitThermostat();
    expect(r.c).toContain('er_node_create(ER_NODE_ARC)');
    expect(r.c).toContain('p.arc_range ='); // AUTO's two-setpoint band
    expect(r.c).toContain('p.arc_value ='); // the setpoint drives it directly
    expect(r.c).toContain('ER_EVENT_VALUE_CHANGE');
    expect(r.c).not.toMatch(/static void build_svg\d+\(void\)/); // no hand-built op-tape any more
  });

  // The dial moved off <Svg>, so keep an explicit vector fixture: this is what catches a generated call
  // that no longer matches an engine signature (e.g. a stale er_node_set_vector_ops arity).
  it('still emits a valid vector op-tape for an <Svg> app', () => {
    const r = compileSource(
      `import { View, Svg, Path, Circle } from 'embedded-react';
       export function App() {
         return (
           <View style={{ flex: 1 }}>
             <Svg width={100} height={100}>
               <Path d="M 10 90 A 40 40 0 1 1 90 90" stroke="#f4a261" strokeWidth={8} fill="none" />
               <Circle cx={50} cy={50} r={12} fill="#16202f" stroke="#f4a261" strokeWidth={3} />
             </Svg>
           </View>
         );
       }`,
      'svg',
    );
    expect(r.c).toMatch(/static const float s_svg\d+_ops\[\]/); // baked tape (a fully static <Svg>)
    expect(r.c).toMatch(
      /er_node_set_vector_ops\(n\d+, s_svg\d+_ops, \d+, s_svg\d+_paints, \d+/,
    );
  });

  (CC ? it : it.skip)(
    `the generated C passes a C compiler syntax check (${CC || 'no cc found'})`,
    async () => {
      const r = await emitThermostat();
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        // -fsyntax-only: the struct (ERVectorGradient) + signature (er_node_set_vector_ops) are unconditional
        // in the engine headers, so this validates the codegen against the real API with no gradient flags.
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {
            encoding: 'utf8',
          },
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );

  (CC ? it : it.skip)(
    `a <Dial> app (state + animated value + onChange) passes the C syntax check (${CC || 'no cc found'})`,
    () => {
      const r = compileSource(
        `import { useState } from 'react';
         import { View, Dial, useAnimatedValue } from 'embedded-react';
         export function App() {
           const [temp, setTemp] = useState(21);
           const level = useAnimatedValue(0);
           return (
             <View style={{ flex: 1 }}>
               <Dial value={temp} min={10} max={30} step={0.5} cap="round" knob="circle" adjustable
                     indicatorColor={temp > 25 ? '#ff4040' : '#ff8800'} onChange={(v) => setTemp(v)}
                     style={{ width: 200, height: 200 }} />
               <Dial value={level} max={100} segments={8} gapAngle={3}
                     indicatorGradient={{ type: 'conic', stops: [{ color: '#0000ff' }, { color: '#ff0000' }] }} />
             </View>
           );
         }`,
        'dial',
      );
      const dir = mkdtempSync(join(tmpdir(), 'er-aot-cc-dial-'));
      try {
        writeFileSync(join(dir, 'app.gen.c'), r.c);
        writeFileSync(join(dir, 'app.gen.h'), r.h);
        const res = spawnSync(
          CC,
          [
            '-fsyntax-only',
            '-I',
            engineInc,
            '-I',
            engineCore,
            join(dir, 'app.gen.c'),
          ],
          {encoding: 'utf8'},
        );
        expect(res.stderr || '').toBe('');
        expect(res.status).toBe(0);
      } finally {
        rmSync(dir, {recursive: true, force: true});
      }
    },
  );
});
