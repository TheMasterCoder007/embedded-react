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
//     type 0 = alignment padding (zero bytes — carries no payload, skipped by loaders)
//     type 1 = QuickJS bytecode (the app — run last)
//     type 2 = ERPK asset pack (registered before the app mounts)
//     type 3 = QuickJS vendor bytecode (react + reconciler + lib — run FIRST, before the app). Optional;
//              present only for the vendor/app split used by incremental hot reload. A container with no
//              type-3 section is a plain monolithic app, loaded exactly as before (backwards-compatible).
//
// Alignment contract: the engine reads asset pixels IN PLACE from wherever the container sits (e.g.
// memory-mapped flash), and the pack's internal pixel padding is computed relative to the pack's own
// first byte. Those pads only mean anything at the absolute addresses the engine reads if the pack
// itself starts on a 4-byte boundary — so the emitter inserts a type-0 padding section before the
// asset pack whenever needed to 4-align the pack's first byte RELATIVE TO THE CONTAINER, and the host
// must place the container at a 4-byte-aligned base address (linker scripts typically give 16; malloc
// guarantees it). Loaders bounds-walk every section and dispatch only on types they know, so old
// loaders skip the padding section untouched — no format version bump.

const FORMAT_VERSION = 1;

export const SECTION_PAD = 0;
export const SECTION_BYTECODE = 1;
export const SECTION_ASSET_PACK = 2;
export const SECTION_VENDOR_BYTECODE = 3;

/**
 * CRC-32/IEEE (zlib polynomial 0xEDB88320), computed without a lookup table to match er_crc32() in
 * runtime/er_container.c byte-for-byte — the one checksum of the whole delivery pipeline (this
 * container, the hot-reload transport frame that carries it, and the device's verification of both).
 * hotreload/protocol.mjs imports this rather than keeping its own.
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
 * @param {Buffer|Uint8Array} opts.bytecode        QuickJS bytecode blob (.qbc) for the app — required.
 * @param {Buffer|Uint8Array} [opts.vendorBytecode] QuickJS bytecode for the vendor chunk (react + reconciler
 *                                                  + lib). When present, the loader runs it BEFORE the app
 *                                                  (it installs the module registry the app require()s). Omit
 *                                                  for a monolithic app — output is then byte-identical to before.
 * @param {Buffer|Uint8Array} [opts.assetPack]     ERPK asset pack bytes; omitted/empty → no asset section.
 * @param {string} opts.qjsTag                     QuickJS release tag the bytecode targets (e.g. "v0.15.0").
 * @returns {Buffer} The container bytes.
 */
export function emitContainer({bytecode, vendorBytecode, assetPack, qjsTag}) {
  if (!bytecode || !bytecode.length)
    throw new Error('emitContainer: bytecode is required');
  if (!qjsTag) throw new Error('emitContainer: qjsTag is required');

  // Build the body (everything after the crc32 field) first, then prepend magic+version+crc.
  const body = new Writer();
  body.str(qjsTag);
  const sections = [];
  // Order matters: vendor runs first (registers modules), assets register, then the app runs last.
  if (vendorBytecode && vendorBytecode.length)
    sections.push([SECTION_VENDOR_BYTECODE, vendorBytecode]);
  if (assetPack && assetPack.length)
    sections.push([SECTION_ASSET_PACK, assetPack]);
  sections.push([SECTION_BYTECODE, bytecode]);

  // 4-align the asset pack's first byte relative to the container (see the alignment contract above)
  // by inserting a type-0 padding section ahead of it when the layout calls for one. Walk the layout
  // once to place pads, then write — section_count must already include them.
  const withPads = [];
  let off = 12 + 2 + Buffer.byteLength(qjsTag, 'utf8') + 4; // header + tag + section_count
  for (const [type, data] of sections) {
    if (type === SECTION_ASSET_PACK) {
      const pad = (4 - ((off + 8) % 4)) % 4; // +8 = this section's own type+len header
      if (pad) {
        withPads.push([SECTION_PAD, Buffer.alloc(pad)]);
        off += 8 + pad;
      }
    }
    withPads.push([type, data]);
    off += 8 + data.length;
  }

  body.u32(withPads.length);
  for (const [type, data] of withPads) {
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

export const SECTION_PROGRAM = 4;
export const SECTION_DEBUG = 5;

/**
 * Serializes a Flow B package into an ERCF container tagged `erb`: the asset pack, the program, then the debug
 * table. Every section starts 4-aligned in the container, since the runtime reads each in place.
 *
 * @param {object} opts
 * @param {Uint8Array} opts.program               The program section (aot/compiler/package/writer.mts).
 * @param {Uint8Array} [opts.assetPack]           ERPK asset pack bytes; omitted/empty → no asset section.
 * @param {Uint8Array} [opts.debug]               The debug section; omitted for a release build.
 * @returns {{bytes: Buffer, sections: Array<{type:number, offset:number, bytes:number}>}} The container, and
 *          where each non-padding section's data starts in it.
 */
export function emitPackageContainer({program, assetPack, debug}) {
  if (!program || !program.length)
    throw new Error('emitPackageContainer: program is required');

  const tag = 'erb';
  const sections = [];
  if (assetPack && assetPack.length)
    sections.push([SECTION_ASSET_PACK, assetPack]);
  sections.push([SECTION_PROGRAM, program]);
  if (debug && debug.length) sections.push([SECTION_DEBUG, debug]);

  const withPads = [];
  const placed = [];
  let off = 12 + 2 + Buffer.byteLength(tag, 'utf8') + 4; // header + tag + section_count
  for (const [type, data] of sections) {
    if ((off + 8) % 4) {
      // A padding section has its own 8-byte header, so its data has to cover what is left after it.
      const pad = (4 - ((off + 16) % 4)) % 4;
      withPads.push([SECTION_PAD, Buffer.alloc(pad)]);
      off += 8 + pad;
    }
    placed.push({type, offset: off + 8, bytes: data.length});
    withPads.push([type, data]);
    off += 8 + data.length;
  }

  const body = new Writer();
  body.str(tag);
  body.u32(withPads.length);
  for (const [type, data] of withPads) {
    body.u32(type);
    body.u32(data.length);
    body.bytes(data);
  }
  const bodyBytes = body.done();

  const head = new Writer();
  head.bytes(Buffer.from('ERCF', 'ascii'));
  head.u32(FORMAT_VERSION);
  head.u32(crc32(bodyBytes));
  return {bytes: Buffer.concat([head.done(), bodyBytes]), sections: placed};
}
