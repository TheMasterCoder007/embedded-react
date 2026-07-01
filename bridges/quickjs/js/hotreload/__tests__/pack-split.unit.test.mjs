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

// Verifies the vendor/app split: the framework half is large and stable, the app half is tiny (the only
// thing a hot-reload frame carries), and the containers carry the right ERCF sections. Asserts on the
// bundled JS sources (a faithful proxy — if react/reconciler leaked into the app chunk, its source would
// balloon), so the test needs only esbuild, not the prebuilt sim wasm (which isn't present in CI). The
// real bytecode-size ratio is exercised end-to-end by the runtime/round-trip harness.

import {describe, it, expect} from 'vitest';
import {resolve} from 'node:path';
import {
  bundleVendorSource,
  bundleAppSource,
  emitBootContainer,
  emitAppFrame,
} from '../pack-split.mjs';
import {
  emitContainer,
  crc32,
  SECTION_BYTECODE,
  SECTION_ASSET_PACK,
  SECTION_VENDOR_BYTECODE,
} from '../../assets/emit-container.mjs';

const PKG = resolve(import.meta.dirname, '../..'); // .../bridges/quickjs/js
const REPO = resolve(PKG, '../../..');
const libSrc = resolve(PKG, 'src/embedded-react/index.js');
const nodePaths = [resolve(PKG, 'node_modules')];
const entry = resolve(REPO, 'demos/thermostat/index.jsx');
const projectRoot = resolve(REPO, 'demos/thermostat');

/** Parse an ERCF container into {version, crcOk, tag, sections:[{type,len}]}. */
function parseContainer(buf) {
  expect(buf.subarray(0, 4).toString('ascii')).toBe('ERCF');
  const version = buf.readUInt32LE(4);
  const crc = buf.readUInt32LE(8);
  const body = buf.subarray(12);
  const crcOk = crc32(body) === crc;
  let o = 12;
  const tagLen = buf.readUInt16LE(o);
  o += 2;
  const tag = buf.subarray(o, o + tagLen).toString('utf8');
  o += tagLen;
  const count = buf.readUInt32LE(o);
  o += 4;
  const sections = [];
  for (let i = 0; i < count; i++) {
    const type = buf.readUInt32LE(o);
    o += 4;
    const len = buf.readUInt32LE(o);
    o += 4;
    sections.push({type, len});
    o += len;
  }
  return {version, crcOk, tag, sections};
}

describe('pack-split: vendor/app split', () => {
  it('app chunk marks the shared deps EXTERNAL (require), reconciler NOT inlined', async () => {
    const {source} = await bundleAppSource({entry, projectRoot, nodePaths});
    // esbuild leaves the externals as require() calls resolved by the vendor registry at runtime.
    expect(source).toMatch(/require\(["']embedded-react["']\)/);
    expect(source).toMatch(/require\(["']react\/jsx-runtime["']\)/);
    // The reconciler is part of vendor, so it must NOT be inlined into the app chunk.
    expect(source).not.toContain('react-reconciler');
  });

  it('vendor chunk installs the module registry + require shim', async () => {
    const source = await bundleVendorSource({libSrc, nodePaths});
    expect(source).toContain('__er_modules');
    expect(source).toMatch(/globalThis\.require\s*=/);
  });

  it('vendor source dwarfs the app source (the split payoff)', async () => {
    const vendor = await bundleVendorSource({libSrc, nodePaths});
    const {source: app} = await bundleAppSource({
      entry,
      projectRoot,
      nodePaths,
    });
    // Vendor is the framework; the app is a small slice of the total (thermostat bytecode measured ~6%).
    expect(vendor.length).toBeGreaterThan(app.length * 4);
    const appShare = app.length / (vendor.length + app.length);
    expect(appShare).toBeLessThan(0.25);
  }, 30000);

  it('boot container carries vendor+assets+app (ordered); hot-reload frame carries app only', async () => {
    // Fake bytecode blobs — this test is about container framing, not compilation.
    const vendorBytecode = Buffer.alloc(900 * 1024, 1);
    const appBytecode = Buffer.alloc(50 * 1024, 2);
    const assetPack = Buffer.alloc(10 * 1024, 3);

    const boot = await emitBootContainer({
      vendorBytecode,
      appBytecode,
      assetPack,
    });
    const frame = await emitAppFrame({appBytecode, assetPack});

    const bootP = parseContainer(boot);
    expect(bootP.crcOk).toBe(true);
    const bootTypes = bootP.sections.map(s => s.type);
    expect(bootTypes).toContain(SECTION_VENDOR_BYTECODE);
    expect(bootTypes).toContain(SECTION_ASSET_PACK);
    expect(bootTypes).toContain(SECTION_BYTECODE);
    // Vendor must precede the app section (run-first ordering).
    expect(bootTypes.indexOf(SECTION_VENDOR_BYTECODE)).toBeLessThan(
      bootTypes.indexOf(SECTION_BYTECODE),
    );

    const frameP = parseContainer(frame);
    expect(frameP.crcOk).toBe(true);
    const frameTypes = frameP.sections.map(s => s.type);
    expect(frameTypes).not.toContain(SECTION_VENDOR_BYTECODE); // no vendor on the wire per edit
    expect(frameTypes).toContain(SECTION_BYTECODE);

    // The hot-reload frame is dramatically smaller than the boot artifact.
    expect(frame.length).toBeLessThan(boot.length / 3);
  });

  it('emitContainer stays backwards-compatible: no vendor section when none is given', () => {
    const fake = Buffer.from('not-real-bytecode-but-nonempty');
    const mono = emitContainer({bytecode: fake, qjsTag: 'v0.15.0'});
    const p = parseContainer(mono);
    expect(p.sections.map(s => s.type)).toEqual([SECTION_BYTECODE]);
    expect(p.sections.some(s => s.type === SECTION_VENDOR_BYTECODE)).toBe(
      false,
    );
  });
});
