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

// Unit tests for the ERCF config-container emitter (emit-container.mjs). These lock down the on-disk
// format and — crucially — that the JS CRC-32 matches the C loader's crc32_bytes() (er_runtime.c), so
// a container packed here verifies on a device instead of being rejected as corrupt.
import {describe, it, expect} from 'vitest';
import {
  emitContainer,
  crc32,
  SECTION_BYTECODE,
  SECTION_ASSET_PACK,
} from '../emit-container.mjs';

/** Re-reads a little-endian ERCF container into a structured object (mirrors the C loader's walk). */
function parseContainer(buf) {
  let off = 0;
  const u16 = () => {
    const v = buf.readUInt16LE(off);
    off += 2;
    return v;
  };
  const u32 = () => {
    const v = buf.readUInt32LE(off);
    off += 4;
    return v;
  };
  const magic = buf.toString('ascii', 0, 4);
  off = 4;
  const formatVersion = u32();
  const storedCrc = u32();
  const tagLen = u16();
  const qjsTag = buf.toString('utf8', off, off + tagLen);
  off += tagLen;
  const sectionCount = u32();
  const sections = [];
  for (let i = 0; i < sectionCount; i++) {
    const type = u32();
    const len = u32();
    const data = buf.subarray(off, off + len);
    off += len;
    sections.push({type, data});
  }
  return {magic, formatVersion, storedCrc, qjsTag, sections, end: off};
}

describe('crc32', () => {
  it('matches the canonical CRC-32/IEEE check value (so it matches the C loader)', () => {
    // "123456789" → 0xCBF43926 is the standard check vector for CRC-32/IEEE (poly 0xEDB88320, init
    // 0xFFFFFFFF, xorout 0xFFFFFFFF) — the exact variant er_runtime.c's crc32_bytes() computes.
    expect(crc32(Buffer.from('123456789', 'ascii'))).toBe(0xcbf43926);
  });

  it('returns the empty-input identity', () => {
    expect(crc32(Buffer.alloc(0))).toBe(0);
  });
});

describe('emitContainer', () => {
  const bytecode = Buffer.from([0xde, 0xad, 0xbe, 0xef, 0x01, 0x02]);
  const assetPack = Buffer.from('ERPK fake pack bytes', 'ascii');

  it('emits a well-formed container with assets (pack first, bytecode last)', () => {
    const c = emitContainer({bytecode, assetPack, qjsTag: 'v0.15.0'});
    const p = parseContainer(c);
    expect(p.magic).toBe('ERCF');
    expect(p.formatVersion).toBe(1);
    expect(p.qjsTag).toBe('v0.15.0');
    expect(p.end).toBe(c.length); // consumed every byte — no trailing slop
    expect(p.sections.map(s => s.type)).toEqual([
      SECTION_ASSET_PACK,
      SECTION_BYTECODE,
    ]);
    expect(Buffer.compare(p.sections[0].data, assetPack)).toBe(0);
    expect(Buffer.compare(p.sections[1].data, bytecode)).toBe(0);
  });

  it('stores a CRC over the bytes after the crc field, and it verifies', () => {
    const c = emitContainer({bytecode, assetPack, qjsTag: 'v0.15.0'});
    const p = parseContainer(c);
    expect(p.storedCrc).toBe(crc32(c.subarray(12)));
  });

  it('flipping any body byte breaks the CRC (integrity check)', () => {
    const c = emitContainer({bytecode, assetPack, qjsTag: 'v0.15.0'});
    const tampered = Buffer.from(c);
    tampered[tampered.length - 1] ^= 0xff;
    const p = parseContainer(tampered);
    expect(p.storedCrc).not.toBe(crc32(tampered.subarray(12)));
  });

  it('omits the asset section when there are no assets', () => {
    const c = emitContainer({bytecode, qjsTag: 'v0.15.0'});
    const p = parseContainer(c);
    expect(p.sections.map(s => s.type)).toEqual([SECTION_BYTECODE]);
  });

  it('rejects missing bytecode or tag', () => {
    expect(() => emitContainer({qjsTag: 'v0.15.0'})).toThrow();
    expect(() => emitContainer({bytecode})).toThrow();
  });
});
