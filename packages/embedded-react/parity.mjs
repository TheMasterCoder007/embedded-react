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
//   Flow A (interpreted): pack-container.mjs bundles demos/<demo>/index.jsx, precompiles it to QuickJS
//                         bytecode and bakes its images/fonts into dist/app.erpkg → the QuickJS desktop
//                         host (examples/linux) renders that headlessly (ER_SHOT) to a BMP.
//   Flow B (AOT):         aot/compile.mts compiles demos/<demo>/App.jsx → dist/app.erpkg, and the AOT host
//                         (examples/linux-aot) loads it and renders headlessly (ER_AOT_SHOT) to a BMP.
//
// Flow A renders from a CONTAINER, not from the bare dist/app.bundle.js, because the desktop host only
// registers baked assets when it loads one (examples/linux ships no assets of its own — it mirrors the
// device's uploaded-config model). Handed a plain .js path it loads source and nothing else, so every
// <Image> in the demo would simply be absent from Flow A's frame while Flow B — whose package carries the
// assets aot/compile.mts bakes — painted it. That also puts Flow A on the bytecode the
// MCU actually runs, which is the path this harness means to model.
//
// Then the two BMPs are pixel-diffed. Because the engine, fonts, and backend are shared, a correct demo
// renders byte-identically through both paths — any difference is a real Flow A↔B divergence (this is
// exactly how an animated-transform binding bug was once caught). Optional taps drive both
// paths through the same interaction (ER_TAPS / ER_AOT_TAPS), so the dynamic state is compared too.
//
// Responsive demos (e.g. thermostat) pick a layout from `screen.width`; each scenario sets a screen size
// fed to BOTH paths (ER_W/ER_H for Flow A's window, ER_AOT_SCREEN_W/H for Flow B's compile +
// window) so they render the same branch at the same dimensions.
//
// Both hosts are run with ER_NO_HIDPI=1 so a Retina display can't scale that size out from under them.
// Both flows read "the screen" at runtime, and a 2x framebuffer would have each lay out to fill it, not to
// the scenario's size — and Flow B's package, compiled for that size alone, would refuse it.
// The rendered frames are size-checked below to keep them from silently regressing.
//
// This is a DEV-MACHINE harness: it opens real SDL windows (the desktop backend has no headless renderer),
// so it needs a display. Run with `npm run parity` from packages/embedded-react.

import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {dirname, resolve, join} from 'node:path';
import {readFileSync, mkdirSync, existsSync} from 'node:fs';

import {findCompileBin, compileBinHelp} from './compile-bin.mjs';

const PACKAGE_DIR = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(PACKAGE_DIR, '../..');

// CMake only appends .exe on Windows, so the harness has to ask (same as test/runtime/run.mjs).
const EXE_SUFFIX = process.platform === 'win32' ? '.exe' : '';
const FLOW_A_EXE = join(
  ROOT,
  `examples/linux/build/embedded-react-desktop${EXE_SUFFIX}`,
);
const FLOW_B_EXE = join(
  ROOT,
  `examples/linux-aot/build/embedded-react-desktop-aot${EXE_SUFFIX}`,
);
const FLOW_B_BUILD = join(ROOT, 'examples/linux-aot/build');
// Flow A's pack and Flow B's compile both write here, each read before the other runs.
const CONTAINER = join(PACKAGE_DIR, 'dist/app.erpkg');
const PACK_MJS = join(PACKAGE_DIR, 'pack-container.mjs');
const AOT_COMPILER = join(PACKAGE_DIR, 'aot/compile.mts');
const OUT_DIR = join(PACKAGE_DIR, 'dist/parity');

// Per-pixel tolerance: max allowed per-channel delta before a pixel counts as "differing", and the max
// fraction of differing pixels a scenario may have. Both paths drive the same engine, so parity is exact
// — these are a hair above zero only to absorb a stray AA pixel, not to paper over real divergence.
const CHANNEL_TOLERANCE = 16;
const MAX_DIFF_FRACTION = 0.0005; // 0.05%

// BMP header fields, by byte offset.
const BMP_PIXEL_DATA_OFFSET_AT = 10;
const BMP_WIDTH_AT = 18;
const BMP_HEIGHT_AT = 22;
const BMP_BITS_PER_PIXEL_AT = 28;
const RGB_BYTES = 3;

// Scenarios. `screen` is the board size both paths render at; `taps` (optional) is the shared interaction
// in physical pixels, "x,y x,y …" (same string handed to ER_TAPS and ER_AOT_TAPS).
const SCENARIOS = [
  // Watch face at its native RP2040 panel — an asset-free View/Text pager, so its initial frame is a clean
  // base-render parity check (the swipe is a touch-drag, which the tap harness can't drive).
  {demo: 'watch-face', name: 'watch-face', screen: {width: 240, height: 280}},
  // Thermostat is responsive: 320×480 falls below both the split (≥760 and landscape) and stack (≥600 tall)
  // thresholds, so both paths compile/render its `solo` branch — the only one with Flow A↔B parity (the
  // split and stack branches are Flow-A-only by design).
  {
    demo: 'thermostat',
    name: 'thermostat-solo',
    screen: {width: 320, height: 480},
  },
];

const ANSI = {
  red: '\x1b[31m',
  green: '\x1b[32m',
  dim: '\x1b[2m',
  reset: '\x1b[0m',
  bold: '\x1b[1m',
};

/** Runs a command, inheriting stdio for build/compile steps; throws on failure. */
function run(command, args, environment) {
  execFileSync(command, args, {
    stdio: 'inherit',
    env: {...process.env, ...environment},
  });
}

/** Reads a 24/32-bpp BMP into { width, height, pixels } with pixels as tightly-packed RGB (3 bytes/pixel), top-row first. */
function readBMP(path) {
  const bmp = readFileSync(path);
  const pixelDataOffset = bmp.readUInt32LE(BMP_PIXEL_DATA_OFFSET_AT);
  const width = bmp.readInt32LE(BMP_WIDTH_AT);
  const rawHeight = bmp.readInt32LE(BMP_HEIGHT_AT);
  const bitsPerPixel = bmp.readUInt16LE(BMP_BITS_PER_PIXEL_AT);
  const height = Math.abs(rawHeight);
  const topDown = rawHeight < 0;
  const rowSize = Math.floor((bitsPerPixel * width + 31) / 32) * 4;
  const bytesPerPixel = bitsPerPixel / 8;
  const pixels = Buffer.alloc(width * height * RGB_BYTES);
  for (let y = 0; y < height; y++) {
    const sourceRow = topDown ? y : height - 1 - y;
    for (let x = 0; x < width; x++) {
      const sourceIndex =
        pixelDataOffset + sourceRow * rowSize + x * bytesPerPixel;
      const pixelIndex = (y * width + x) * RGB_BYTES;
      pixels[pixelIndex] = bmp[sourceIndex + 2];
      pixels[pixelIndex + 1] = bmp[sourceIndex + 1];
      pixels[pixelIndex + 2] = bmp[sourceIndex];
    }
  }
  return {width, height, pixels};
}

/** Pixel-diffs two RGB images. Returns { ok, differingPixels, total, fraction, maxDelta, boundingBox|null, reason? }. */
function diffImages(imageA, imageB) {
  if (imageA.width !== imageB.width || imageA.height !== imageB.height) {
    return {
      ok: false,
      reason: `size mismatch: ${imageA.width}x${imageA.height} vs ${imageB.width}x${imageB.height}`,
    };
  }
  let differingPixels = 0;
  let maxDelta = 0;
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -1;
  let maxY = -1;
  for (let y = 0; y < imageA.height; y++) {
    for (let x = 0; x < imageA.width; x++) {
      const pixelIndex = (y * imageA.width + x) * RGB_BYTES;
      const delta = Math.max(
        Math.abs(imageA.pixels[pixelIndex] - imageB.pixels[pixelIndex]),
        Math.abs(imageA.pixels[pixelIndex + 1] - imageB.pixels[pixelIndex + 1]),
        Math.abs(imageA.pixels[pixelIndex + 2] - imageB.pixels[pixelIndex + 2]),
      );
      if (delta > maxDelta) maxDelta = delta;
      if (delta > CHANNEL_TOLERANCE) {
        differingPixels++;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
  }
  const total = imageA.width * imageA.height;
  const fraction = differingPixels / total;
  return {
    ok: fraction <= MAX_DIFF_FRACTION,
    differingPixels,
    total,
    fraction,
    maxDelta,
    boundingBox: differingPixels ? {minX, minY, maxX, maxY} : null,
  };
}

/** Renders one scenario through both flows and diffs them. Returns the diff result. */
function runScenario(scenario) {
  const shotPathA = join(OUT_DIR, `${scenario.name}.flowA.bmp`);
  const shotPathB = join(OUT_DIR, `${scenario.name}.flowB.bmp`);
  const screenEnv = {
    ER_AOT_SCREEN_W: String(scenario.screen.width),
    ER_AOT_SCREEN_H: String(scenario.screen.height),
  };

  // --- Flow A: pack a container (bytecode + baked assets) → render via the QuickJS host ---
  run(process.execPath, [PACK_MJS, scenario.demo]);
  run(FLOW_A_EXE, [CONTAINER], {
    ER_SHOT: shotPathA,
    ER_W: String(scenario.screen.width),
    ER_H: String(scenario.screen.height),
    ER_NO_HIDPI: '1',
    ...(scenario.taps ? {ER_TAPS: scenario.taps} : {}),
  });

  // --- Flow B: compile (AOT) → load the package in the host → render ---
  run(process.execPath, [AOT_COMPILER, scenario.demo], screenEnv);
  run(FLOW_B_EXE, [CONTAINER], {
    ER_AOT_SHOT: shotPathB,
    ...screenEnv,
    ER_NO_HIDPI: '1',
    ...(scenario.taps ? {ER_AOT_TAPS: scenario.taps} : {}),
  });

  const imageA = readBMP(shotPathA);
  const imageB = readBMP(shotPathB);
  // Guard the premise of the whole comparison: both flows must have rendered the scenario's board size.
  // A frame that isn't that size means the host ignored ER_NO_HIDPI (or the size env vars), and the diff
  // below would be comparing two different layouts — a loud "wrong size" beats a 75% pixel divergence.
  for (const [flow, image] of [
    ['A', imageA],
    ['B', imageB],
  ]) {
    if (
      image.width !== scenario.screen.width ||
      image.height !== scenario.screen.height
    ) {
      return {
        ok: false,
        reason:
          `Flow ${flow} rendered ${image.width}x${image.height}, expected ${scenario.screen.width}x${scenario.screen.height} — ` +
          `the host scaled the framebuffer (HiDPI?), so the flows laid out at different sizes`,
      };
    }
  }
  return diffImages(imageA, imageB);
}

function main() {
  const filter = process.argv[2];
  const scenarios = filter
    ? SCENARIOS.filter(
        scenario => scenario.name.includes(filter) || scenario.demo === filter,
      )
    : SCENARIOS;
  if (scenarios.length === 0) {
    console.error(
      `No scenarios match "${filter}". Known: ${SCENARIOS.map(scenario => scenario.name).join(', ')}`,
    );
    process.exit(2);
  }
  for (const hostExe of [FLOW_A_EXE, FLOW_B_EXE]) {
    if (!existsSync(hostExe)) {
      console.error(
        `Missing host exe: ${hostExe}\nBuild the desktop hosts first (cmake --build their build dirs).`,
      );
      process.exit(2);
    }
  }
  // Packing Flow A's container needs the bytecode precompiler too — check it here rather than letting
  // the first scenario die halfway through with a build error.
  if (!findCompileBin()) {
    console.error(compileBinHelp());
    process.exit(2);
  }
  mkdirSync(OUT_DIR, {recursive: true});
  run('cmake', ['--build', FLOW_B_BUILD]);

  console.log(
    `${ANSI.bold}Flow A ↔ Flow B parity${ANSI.reset}  (${scenarios.length} scenario(s))\n`,
  );
  let failures = 0;
  for (const scenario of scenarios) {
    console.log(
      `${ANSI.dim}── ${scenario.name} @ ${scenario.screen.width}×${scenario.screen.height}${scenario.taps ? ` taps[${scenario.taps}]` : ''} ──${ANSI.reset}`,
    );
    let result;
    try {
      result = runScenario(scenario);
    } catch (error) {
      console.log(
        `${ANSI.red}✗ ${scenario.name}: harness error — ${error.message}${ANSI.reset}\n`,
      );
      failures++;
      continue;
    }
    if (result.ok && !result.reason) {
      const percent = (result.fraction * 100).toFixed(4);
      console.log(
        `${ANSI.green}✓ ${scenario.name}${ANSI.reset}  ${result.differingPixels}/${result.total} px differ (${percent}%), maxΔ=${result.maxDelta}\n`,
      );
    } else {
      failures++;
      if (result.reason) {
        console.log(
          `${ANSI.red}✗ ${scenario.name}: ${result.reason}${ANSI.reset}\n`,
        );
      } else {
        const percent = (result.fraction * 100).toFixed(4);
        const boundingBox = result.boundingBox;
        console.log(
          `${ANSI.red}✗ ${scenario.name}${ANSI.reset}  ${result.differingPixels}/${result.total} px differ (${percent}% > ${(MAX_DIFF_FRACTION * 100).toFixed(2)}%), ` +
            `maxΔ=${result.maxDelta}, bbox=(${boundingBox.minX},${boundingBox.minY})-(${boundingBox.maxX},${boundingBox.maxY})\n` +
            `${ANSI.dim}  see ${join(OUT_DIR, scenario.name)}.flow{A,B}.bmp${ANSI.reset}\n`,
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
