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

// ERCF — the embedded-react config container: one self-describing blob holding an app's compiled
// QuickJS bytecode plus its asset pack, with a version stamp and an integrity CRC. This is the
// universal "a config" unit the device-agnostic loader (er_runtime_load_container, in the bridge)
// verifies and runs — on the desktop demo, an ESP32-S3 partition, or an STM32H7 config region.
//
// Two CRCs are NOT the same thing: this internal CRC32 is embedded-react's own integrity check
// (catches a corrupt/partially-written config). A bootloader's transfer/flash CRC (e.g. SREC_CAT on
// the STM32H7) is a separate, project-specific step layered on top of this .erpkg by the firmware's
// upload toolchain.
//
// Little-endian (matches the LE hosts: x86 desktop, ESP32-S3, STM32H7). Format:
//
//   magic "ERCF" (4 bytes)
//   format_version u32 = 1
//   crc32          u32   — CRC-32/IEEE over every byte AFTER this field (offset 12 to end)
//   qjs_tag        u16 len + bytes   — QuickJS release the bytecode targets (loader rejects mismatch)
//   section_count  u32
//   sections[section_count]: type u32, len u32, bytes
//     type 1 = QuickJS bytecode (run last)
//     type 2 = ERPK asset pack (registered before the app mounts)

const FORMAT_VERSION = 1;

export const SECTION_BYTECODE = 1;
export const SECTION_ASSET_PACK = 2;

/**
 * CRC-32/IEEE (zlib polynomial 0xEDB88320), computed without a lookup table to match the C loader's
 * crc32_bytes() byte-for-byte.
 *
 * @param {Buffer|Uint8Array} buf  Bytes to checksum.
 * @returns {number} The CRC as an unsigned 32-bit integer.
 */
export function crc32(buf) {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc ^= buf[i];
    for (let b = 0; b < 8; b++) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/** Accumulates little-endian binary chunks (mirrors emit-pack.mjs's Writer). */
class Writer {
  constructor() {
    this.chunks = [];
  }
  u16(v) {
    const b = Buffer.alloc(2);
    b.writeUInt16LE(v & 0xffff, 0);
    this.chunks.push(b);
  }
  u32(v) {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(v >>> 0, 0);
    this.chunks.push(b);
  }
  str(s) {
    const b = Buffer.from(s, 'utf8');
    this.u16(b.length);
    this.chunks.push(b);
  }
  bytes(buf) {
    this.chunks.push(Buffer.from(buf));
  }
  done() {
    return Buffer.concat(this.chunks);
  }
}

/**
 * Serializes a compiled app + assets into an ERCF container.
 *
 * @param {object} opts
 * @param {Buffer|Uint8Array} opts.bytecode    QuickJS bytecode blob (.qbc) — required.
 * @param {Buffer|Uint8Array} [opts.assetPack] ERPK asset pack bytes; omitted/empty → no asset section.
 * @param {string} opts.qjsTag                 QuickJS release tag the bytecode targets (e.g. "v0.15.0").
 * @returns {Buffer} The container bytes.
 */
export function emitContainer({ bytecode, assetPack, qjsTag }) {
  if (!bytecode || !bytecode.length) throw new Error('emitContainer: bytecode is required');
  if (!qjsTag) throw new Error('emitContainer: qjsTag is required');

  // Build the body (everything after the crc32 field) first, then prepend magic+version+crc.
  const body = new Writer();
  body.str(qjsTag);
  const sections = [];
  if (assetPack && assetPack.length) sections.push([SECTION_ASSET_PACK, assetPack]);
  sections.push([SECTION_BYTECODE, bytecode]);
  body.u32(sections.length);
  for (const [type, data] of sections) {
    body.u32(type);
    body.u32(data.length);
    body.bytes(data);
  }
  const bodyBytes = body.done();

  const head = new Writer();
  head.bytes(Buffer.from('ERCF', 'ascii'));
  head.u32(FORMAT_VERSION);
  head.u32(crc32(bodyBytes));
  return Buffer.concat([head.done(), bodyBytes]);
}
