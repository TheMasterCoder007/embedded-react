import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compileSource } from '../compile.mjs';

// Regression guard: the AOT-targeted demos must keep compiling end-to-end (no thrown "AOT: …"). This is
// the cheap counterpart to a full compile-and-screenshot harness — it would have caught any compiler change
// that broke a demo's codegen during development. (The thermostat's WIDE branch is Flow A-only; the AOT
// compiles its COMPACT branch, selected here via the 240×320 screen.)
const demosDir = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../../demos');
const appSrc = (demo) => readFileSync(resolve(demosDir, demo, 'App.jsx'), 'utf8');

describe('AOT demo compile smoke', () => {
  it('compiles the music-player demo', () => {
    const r = compileSource(appSrc('music-player'), 'music-player', { filename: 'demos/music-player/App.jsx' });
    expect(r.c).toContain('void er_app_build(int screen_w, int screen_h)');
    expect(r.nodes).toBeGreaterThan(0);
  });

  it('compiles the thermostat demo for a 240×320 (CYD) screen — the compact dial branch', () => {
    const r = compileSource(appSrc('thermostat'), 'thermostat', { screen: { width: 240, height: 320 }, filename: 'demos/thermostat/App.jsx' });
    expect(r.c).toContain('void er_app_build(int screen_w, int screen_h)');
    expect(r.c).toContain('build_svg0'); // the state-driven dial
    expect(r.c).toContain('er_cb_onDrag'); // touch-drag handler
    expect(r.handlers).toBeGreaterThan(0);
  });
});
