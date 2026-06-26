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

// parity.mjs — Flow A ↔ Flow B render-parity harness (Phase 12).
//
// Renders the SAME demo through BOTH compile paths and asserts the framebuffers match pixel-for-pixel:
//
//   Flow A (interpreted): build.mjs bundles demos/<demo>/index.jsx → the QuickJS desktop host
//                         (examples/linux) renders it headlessly (ER_SHOT) to a BMP.
//   Flow B (AOT):         aot/compile.mjs compiles demos/<demo>/App.jsx → C, the AOT host
//                         (examples/linux-aot) is rebuilt and renders headlessly (ER_AOT_SHOT) to a BMP.
//
// Then the two BMPs are pixel-diffed. Because the engine, fonts, and backend are shared, a correct demo
// renders byte-identically through both paths — any difference is a real Flow A↔B divergence (this is
// exactly how the music-player animated-transform binding bug was caught). Optional taps drive both
// paths through the same interaction (ER_TAPS / ER_AOT_TAPS) so dynamic state is compared too.
//
// Responsive demos (e.g. thermostat) pick a layout from `screen.width`; each scenario sets a screen size
// that is fed to BOTH paths (ER_W/ER_H for Flow A's window, ER_AOT_SCREEN_W/H for Flow B's compile +
// window) so they render the same branch at the same dimensions.
//
// This is a DEV-MACHINE harness: it opens real SDL windows (the desktop backend has no headless renderer),
// so it needs a display. Run with `npm run parity` from bridges/quickjs/js.

import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {dirname, resolve, join} from 'node:path';
import {readFileSync, mkdirSync, existsSync} from 'node:fs';

const JS_DIR = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(JS_DIR, '../../..');

const FLOW_A_EXE = join(
  ROOT,
  'examples/linux/build/embedded-react-desktop.exe',
);
const FLOW_B_EXE = join(
  ROOT,
  'examples/linux-aot/build/embedded-react-desktop-aot.exe',
);
const FLOW_A_BUILD = join(ROOT, 'examples/linux/build');
const FLOW_B_BUILD = join(ROOT, 'examples/linux-aot/build');
const BUNDLE = join(JS_DIR, 'dist/app.bundle.js');
const BUILD_MJS = join(JS_DIR, 'build.mjs');
const COMPILE_MJS = join(JS_DIR, 'aot/compile.mjs');
const OUT_DIR = join(JS_DIR, 'dist/parity');

// Per-pixel tolerance: max allowed per-channel delta before a pixel counts as "differing", and the max
// fraction of differing pixels a scenario may have. Both paths drive the same engine, so parity is exact
// — these are a hair above zero only to absorb a stray AA pixel, not to paper over real divergence.
const CHANNEL_TOLERANCE = 16;
const MAX_DIFF_FRACTION = 0.0005; // 0.05%

// Scenarios. `screen` is the board size both paths render at; `taps` (optional) is the shared interaction
// in physical pixels, "x,y x,y …" (same string handed to ER_TAPS and ER_AOT_TAPS).
const SCENARIOS = [
  {demo: 'music-player', name: 'music-player', screen: {w: 800, h: 600}},
  // Tap the Play button → toggles to Pause + reveals the "Playing now" badge (dynamic state parity).
  {
    demo: 'music-player',
    name: 'music-player-playing',
    screen: {w: 800, h: 600},
    taps: '400,149',
  },
  // Thermostat is responsive: at a compact (<400px) width both paths compile/render the AOT-supported
  // compact branch — the only branch with Flow A↔B parity (the wide branch is Flow-A-only by design).
  {demo: 'thermostat', name: 'thermostat-compact', screen: {w: 320, h: 480}},
];

const ANSI = {
  red: '\x1b[31m',
  green: '\x1b[32m',
  dim: '\x1b[2m',
  reset: '\x1b[0m',
  bold: '\x1b[1m',
};

/** Runs a command, inheriting stdio for build/compile steps; throws on failure. */
function run(cmd, args, env) {
  execFileSync(cmd, args, {stdio: 'inherit', env: {...process.env, ...env}});
}

/** Reads a 24/32-bpp BMP into { w, h, px } with px as tightly-packed RGB (3 bytes/pixel), top-row first. */
function readBMP(path) {
  const b = readFileSync(path);
  const off = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const rawH = b.readInt32LE(22);
  const bpp = b.readUInt16LE(28);
  const h = Math.abs(rawH);
  const topDown = rawH < 0;
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  const bytesPerPx = bpp / 8;
  const px = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; y++) {
    const sy = topDown ? y : h - 1 - y;
    for (let x = 0; x < w; x++) {
      const i = off + sy * rowSize + x * bytesPerPx;
      const o = (y * w + x) * 3;
      px[o] = b[i + 2];
      px[o + 1] = b[i + 1];
      px[o + 2] = b[i];
    }
  }
  return {w, h, px};
}

/** Pixel-diffs two RGB images. Returns { ok, diff, total, fraction, maxDelta, bbox|null, reason? }. */
function diffImages(a, b) {
  if (a.w !== b.w || a.h !== b.h) {
    return {ok: false, reason: `size mismatch: ${a.w}x${a.h} vs ${b.w}x${b.h}`};
  }
  let diff = 0;
  let maxDelta = 0;
  let minx = Infinity;
  let miny = Infinity;
  let maxx = -1;
  let maxy = -1;
  for (let y = 0; y < a.h; y++) {
    for (let x = 0; x < a.w; x++) {
      const o = (y * a.w + x) * 3;
      const d = Math.max(
        Math.abs(a.px[o] - b.px[o]),
        Math.abs(a.px[o + 1] - b.px[o + 1]),
        Math.abs(a.px[o + 2] - b.px[o + 2]),
      );
      if (d > maxDelta) maxDelta = d;
      if (d > CHANNEL_TOLERANCE) {
        diff++;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
    }
  }
  const total = a.w * a.h;
  const fraction = diff / total;
  return {
    ok: fraction <= MAX_DIFF_FRACTION,
    diff,
    total,
    fraction,
    maxDelta,
    bbox: diff ? {minx, miny, maxx, maxy} : null,
  };
}

/** Renders one scenario through both flows and diffs them. Returns the diff result. */
function runScenario(s) {
  const outA = join(OUT_DIR, `${s.name}.flowA.bmp`);
  const outB = join(OUT_DIR, `${s.name}.flowB.bmp`);
  const screenEnv = {
    ER_AOT_SCREEN_W: String(s.screen.w),
    ER_AOT_SCREEN_H: String(s.screen.h),
  };

  // --- Flow A: bundle (interpreted) → render via the QuickJS host ---
  run(process.execPath, [BUILD_MJS, s.demo]);
  run(FLOW_A_EXE, [BUNDLE], {
    ER_SHOT: outA,
    ER_W: String(s.screen.w),
    ER_H: String(s.screen.h),
    ...(s.taps ? {ER_TAPS: s.taps} : {}),
  });

  // --- Flow B: compile (AOT) → rebuild the baked host → render ---
  run(process.execPath, [COMPILE_MJS, s.demo], screenEnv);
  run('cmake', ['--build', FLOW_B_BUILD], screenEnv);
  run(FLOW_B_EXE, [], {
    ER_AOT_SHOT: outB,
    ...screenEnv,
    ...(s.taps ? {ER_AOT_TAPS: s.taps} : {}),
  });

  return diffImages(readBMP(outA), readBMP(outB));
}

function main() {
  const filter = process.argv[2];
  const scenarios = filter
    ? SCENARIOS.filter(s => s.name.includes(filter) || s.demo === filter)
    : SCENARIOS;
  if (scenarios.length === 0) {
    console.error(
      `No scenarios match "${filter}". Known: ${SCENARIOS.map(s => s.name).join(', ')}`,
    );
    process.exit(2);
  }
  for (const exe of [FLOW_A_EXE, FLOW_B_EXE]) {
    if (!existsSync(exe)) {
      console.error(
        `Missing host exe: ${exe}\nBuild the desktop hosts first (cmake --build their build dirs).`,
      );
      process.exit(2);
    }
  }
  mkdirSync(OUT_DIR, {recursive: true});

  console.log(
    `${ANSI.bold}Flow A ↔ Flow B parity${ANSI.reset}  (${scenarios.length} scenario(s))\n`,
  );
  let failures = 0;
  for (const s of scenarios) {
    console.log(
      `${ANSI.dim}── ${s.name} @ ${s.screen.w}×${s.screen.h}${s.taps ? ` taps[${s.taps}]` : ''} ──${ANSI.reset}`,
    );
    let r;
    try {
      r = runScenario(s);
    } catch (e) {
      console.log(
        `${ANSI.red}✗ ${s.name}: harness error — ${e.message}${ANSI.reset}\n`,
      );
      failures++;
      continue;
    }
    if (r.ok && !r.reason) {
      const pct = (r.fraction * 100).toFixed(4);
      console.log(
        `${ANSI.green}✓ ${s.name}${ANSI.reset}  ${r.diff}/${r.total} px differ (${pct}%), maxΔ=${r.maxDelta}\n`,
      );
    } else {
      failures++;
      if (r.reason) {
        console.log(`${ANSI.red}✗ ${s.name}: ${r.reason}${ANSI.reset}\n`);
      } else {
        const pct = (r.fraction * 100).toFixed(4);
        const bb = r.bbox;
        console.log(
          `${ANSI.red}✗ ${s.name}${ANSI.reset}  ${r.diff}/${r.total} px differ (${pct}% > ${(MAX_DIFF_FRACTION * 100).toFixed(2)}%), ` +
            `maxΔ=${r.maxDelta}, bbox=(${bb.minx},${bb.miny})-(${bb.maxx},${bb.maxy})\n` +
            `${ANSI.dim}  see ${join(OUT_DIR, s.name)}.flow{A,B}.bmp${ANSI.reset}\n`,
        );
      }
    }
  }

  if (failures) {
    console.log(
      `${ANSI.red}${ANSI.bold}${failures} scenario(s) diverged.${ANSI.reset}`,
    );
    process.exit(1);
  }
  console.log(
    `${ANSI.green}${ANSI.bold}All scenarios render identically across Flow A and Flow B.${ANSI.reset}`,
  );
}

main();
