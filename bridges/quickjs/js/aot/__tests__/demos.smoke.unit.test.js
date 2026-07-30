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
import {readFileSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {compileSource, bakeSvgArtifacts} from '../compile.mjs';

// Regression guard: the AOT-targeted demos must keep compiling end-to-end (no thrown "AOT: …"). This is
// the cheap counterpart to a full compile-and-screenshot harness — it would have caught any compiler change
// that broke a demo's codegen during development. (The thermostat's WIDE branch is Flow A-only; the AOT
// compiles its COMPACT branch, selected here via the 240×320 screen.)
const demosDir = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../../../demos',
);
const appSrc = demo => readFileSync(resolve(demosDir, demo, 'App.jsx'), 'utf8');

describe('AOT demo compile smoke', () => {
  it('compiles the watch-face demo for the RP2040 240×280 panel', () => {
    const r = compileSource(appSrc('watch-face'), 'watch-face', {
      screen: {width: 240, height: 280},
      filename: 'demos/watch-face/App.jsx',
    });
    expect(r.c).toContain('void er_app_build(int screen_w, int screen_h)');
    expect(r.nodes).toBeGreaterThan(0);
    expect(r.c).toContain('static void er_timer_fn_1(void);');
  });

  it('compiles the thermostat demo for a 240×320 (CYD) screen — the solo dial branch', async () => {
    const src = appSrc('thermostat');
    const svgArtifacts = await bakeSvgArtifacts(
      src,
      resolve(demosDir, 'thermostat'),
    );
    const r = compileSource(src, 'thermostat', {
      screen: {width: 240, height: 320},
      filename: 'demos/thermostat/App.jsx',
      svgArtifacts,
    });
    expect(r.c).toContain('void er_app_build(int screen_w, int screen_h)');
    expect(r.c).toMatch(/build_svg\d+\(/); // the state-driven dial (track + progress arc + handle)
    expect(r.c).toContain('cosf('); // handle geometry lowered to trig, not a state-driven <Path d>
    expect(r.c).toContain('er_cb_onDrag'); // touch-drag handler
    expect(r.handlers).toBeGreaterThan(0);
  });
});
