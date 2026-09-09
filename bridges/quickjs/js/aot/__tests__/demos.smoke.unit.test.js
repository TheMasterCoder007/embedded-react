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
    // Its swipe pager is a PanResponder, lowered onto the engine's own responder negotiation.
    expect(r.c).toContain('ER_QUERY_START_SHOULD_SET');
    expect(r.c).toContain('ER_EVENT_RESPONDER_MOVE');
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
    expect(r.c).toContain('er_node_create(ER_NODE_ARC)');
    expect(r.c).not.toMatch(/build_svg\d+\(/);
    expect(r.c).toContain('p.arc_range ='); // AUTO's two-setpoint band is state-driven
    expect(r.c).toContain('ER_EVENT_VALUE_CHANGE'); // the drag is native; JS only stores what it reports
    expect(r.handlers).toBeGreaterThan(0);
  });
});

// Each board example pins the demo its app.gen.c must come from, in two places that have to agree: the
// `#ifndef ER_AOT_DEMO_<demo>` guard and the regenerate command in its own error text. Point the guard at
// a scratch demo while testing on hardware, and the example stops building for everyone — with an error
// telling them to run the command that does not satisfy it.
describe('board example demo guards', () => {
  const exampleSrc = rel =>
    readFileSync(resolve(demosDir, '..', 'examples', rel), 'utf8');

  it.each([
    ['esp32/esp32-2432s028r/main/main.c', 'thermostat'],
    ['rp2040/rp2040-touch-lcd-1.69/main.c', 'watch-face'],
  ])('%s guards on the %s marker', (rel, demo) => {
    const src = exampleSrc(rel);
    const marker = src.match(/#ifndef (ER_AOT_DEMO_\w+)/)?.[1];
    const command = src.match(/npm run aot -- ([\w-]+)/)?.[1];
    expect(command).toBe(demo);
    // The marker is the demo name with every non-identifier character mapped to '_' (see app.gen.h).
    expect(marker).toBe(`ER_AOT_DEMO_${demo.replace(/[^A-Za-z0-9_]/g, '_')}`);
  });
});
