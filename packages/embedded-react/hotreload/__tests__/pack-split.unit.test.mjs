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

const PACKAGE_DIRECTORY = resolve(import.meta.dirname, '../..'); // .../packages/embedded-react
const REPO = resolve(PACKAGE_DIRECTORY, '../..');
const librarySource = resolve(PACKAGE_DIRECTORY, 'src/embedded-react/index.js');
const nodePaths = [resolve(PACKAGE_DIRECTORY, 'node_modules')];
const entry = resolve(REPO, 'demos/thermostat/index.jsx');
const projectRoot = resolve(REPO, 'demos/thermostat');

// The container header: magic, version and CRC, then the tag the CRC covers from.
const CONTAINER_MAGIC_BYTES = 4;
const CONTAINER_VERSION_AT = 4;
const CONTAINER_CRC_AT = 8;
const CONTAINER_TAG_AT = 12;

/** Parse an ERCF container into {version, crcOk, tag, sections:[{type,length}]}. */
function parseContainer(bytes) {
  expect(bytes.subarray(0, CONTAINER_MAGIC_BYTES).toString('ascii')).toBe(
    'ERCF',
  );
  const version = bytes.readUInt32LE(CONTAINER_VERSION_AT);
  const storedCrc = bytes.readUInt32LE(CONTAINER_CRC_AT);
  const body = bytes.subarray(CONTAINER_TAG_AT);
  const crcOk = crc32(body) === storedCrc;
  let offset = CONTAINER_TAG_AT;
  const tagLength = bytes.readUInt16LE(offset);
  offset += 2;
  const tag = bytes.subarray(offset, offset + tagLength).toString('utf8');
  offset += tagLength;
  const count = bytes.readUInt32LE(offset);
  offset += 4;
  const sections = [];
  for (let index = 0; index < count; index++) {
    const type = bytes.readUInt32LE(offset);
    offset += 4;
    const length = bytes.readUInt32LE(offset);
    offset += 4;
    sections.push({type, length});
    offset += length;
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
    const source = await bundleVendorSource({libSrc: librarySource, nodePaths});
    expect(source).toContain('__er_modules');
    expect(source).toMatch(/globalThis\.require\s*=/);
  });

  it('vendor source dwarfs the app source (the split payoff)', async () => {
    const vendor = await bundleVendorSource({libSrc: librarySource, nodePaths});
    const {source: app} = await bundleAppSource({
      entry,
      projectRoot,
      nodePaths,
    });
    // Vendor is the framework; the app is a small slice of the total
    expect(vendor.length).toBeGreaterThan(app.length * 2.5);
    const appShare = app.length / (vendor.length + app.length);
    expect(appShare).toBeLessThan(0.3);
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

    const bootContainer = parseContainer(boot);
    expect(bootContainer.crcOk).toBe(true);
    const bootTypes = bootContainer.sections.map(section => section.type);
    expect(bootTypes).toContain(SECTION_VENDOR_BYTECODE);
    expect(bootTypes).toContain(SECTION_ASSET_PACK);
    expect(bootTypes).toContain(SECTION_BYTECODE);
    // Vendor must precede the app section (run-first ordering).
    expect(bootTypes.indexOf(SECTION_VENDOR_BYTECODE)).toBeLessThan(
      bootTypes.indexOf(SECTION_BYTECODE),
    );

    const frameContainer = parseContainer(frame);
    expect(frameContainer.crcOk).toBe(true);
    const frameTypes = frameContainer.sections.map(section => section.type);
    expect(frameTypes).not.toContain(SECTION_VENDOR_BYTECODE); // no vendor on the wire per edit
    expect(frameTypes).toContain(SECTION_BYTECODE);

    // The hot-reload frame is dramatically smaller than the boot artifact.
    expect(frame.length).toBeLessThan(boot.length / 3);
  });

  it('emitContainer stays backwards-compatible: no vendor section when none is given', () => {
    const fake = Buffer.from('not-real-bytecode-but-nonempty');
    const mono = emitContainer({bytecode: fake, qjsTag: 'v0.15.0'});
    const parsed = parseContainer(mono);
    expect(parsed.sections.map(section => section.type)).toEqual([
      SECTION_BYTECODE,
    ]);
    expect(
      parsed.sections.some(section => section.type === SECTION_VENDOR_BYTECODE),
    ).toBe(false);
  });
});
