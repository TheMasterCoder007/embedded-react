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

// ERHR — the embedded-react hot-reload transport frame.
//
// One self-describing frame that carries an .erpkg (ERCF container) over a byte stream — USB-CDC, a
// USB-Serial-JTAG console, or a UART. It is the *transport* envelope, a layer ABOVE the ERCF container:
// ERCF's own CRC proves the config is internally consistent; this frame's CRC proves the bytes arrived
// intact over the wire (a transfer can truncate/corrupt the ERCF header itself, which ERCF's body CRC
// does not cover). The magic + length + CRC also let the firmware parser *resync*: on a shared in-band
// channel (classic ESP32 with one UART-bridge port) the device's own log text is interleaved with
// uploads, so the receiver scans for "ERHR" and validates before touching the runtime.
//
// Little-endian (matches the LE hosts). Header is 16 bytes:
//
//   magic "ERHR"   (4 bytes)
//   proto_version  u8  = 1
//   msg_type       u8  (1 = LOAD_CONTAINER)
//   flags          u16 = 0   (reserved)
//   payload_len    u32       — bytes of .erpkg that follow the header
//   payload_crc    u32       — CRC-32/IEEE over the payload (same polynomial as ERCF / the C loader)
//   payload        bytes[payload_len]   — the .erpkg (ERCF container)
//
// The mirror of this lives in the firmware parser (bridges/quickjs/er_hotreload.c); the two MUST stay
// byte-compatible. crc32 is shared with emit-container.mjs so all three CRCs are the one algorithm.

import {crc32} from '../assets/emit-container.mjs';

export const ERHR_MAGIC = 'ERHR';
export const ERHR_PROTO_VERSION = 1;
export const ERHR_HEADER_SIZE = 16;

/** Frame message types (room to grow: ping/ack/reset later without a version bump). */
export const MSG_LOAD_CONTAINER = 1;

/**
 * Encodes an .erpkg into an ERHR transport frame ready to write to the device's byte stream.
 *
 * @param {Buffer|Uint8Array} erpkg  The ERCF container bytes (from emitContainer).
 * @param {object}            [opts]
 * @param {number}            [opts.type]  Message type (default MSG_LOAD_CONTAINER).
 * @returns {Buffer} The framed bytes (16-byte header + payload).
 */
export function encodeFrame(erpkg, {type = MSG_LOAD_CONTAINER} = {}) {
  if (!erpkg || !erpkg.length)
    throw new Error('encodeFrame: payload (.erpkg) is required');
  const payload = Buffer.from(erpkg);
  const header = Buffer.alloc(ERHR_HEADER_SIZE);
  header.write(ERHR_MAGIC, 0, 'ascii');
  header.writeUInt8(ERHR_PROTO_VERSION, 4);
  header.writeUInt8(type & 0xff, 5);
  header.writeUInt16LE(0, 6); // flags (reserved)
  header.writeUInt32LE(payload.length >>> 0, 8);
  header.writeUInt32LE(crc32(payload), 12);
  return Buffer.concat([header, payload]);
}

/**
 * A streaming decoder mirroring the firmware parser: feed it arbitrary chunks of a byte stream and it
 * scans for the ERHR magic, parses the header, collects the payload, and verifies the CRC. Used by the
 * host's own tests (to prove host/firmware agreement) and usable for a loopback/echo device.
 *
 * Emits via the callbacks passed to the constructor; tolerant of leading/interleaved non-frame bytes.
 */
export class FrameDecoder {
  /**
   * @param {object}   handlers
   * @param {(payload: Buffer, meta: {type: number}) => void} [handlers.onFrame]  A complete, CRC-valid frame.
   * @param {(reason: string) => void}                        [handlers.onError]  A framed payload failed validation.
   */
  constructor({onFrame, onError, maxPayload = 32 * 1024 * 1024} = {}) {
    this.onFrame = onFrame || (() => {});
    this.onError = onError || (() => {});
    this.maxPayload = maxPayload; // guard against a coincidental "ERHR" in noise claiming a huge length
    this._reset();
  }

  _reset() {
    this._state = 'scan'; // scan magic → header → payload
    this._window = Buffer.alloc(4); // rolling 4-byte window for magic resync
    this._winLen = 0;
    this._header = Buffer.alloc(ERHR_HEADER_SIZE);
    this._headerGot = 4; // magic already in _header once matched
    this._payloadLen = 0;
    this._payloadCrc = 0;
    this._type = 0;
    this._payload = null;
    this._payloadGot = 0;
  }

  /** Feed a chunk of received bytes. May complete zero, one, or several frames. */
  push(chunk) {
    const data = Buffer.from(chunk);
    for (let i = 0; i < data.length; i++) this._byte(data[i]);
  }

  _byte(b) {
    if (this._state === 'scan') {
      // Slide the byte into a 4-byte window; a full match transitions to header collection. This
      // naturally handles partial matches and overlaps without backtracking.
      if (this._winLen < 4) {
        this._window[this._winLen++] = b;
      } else {
        this._window.copyWithin(0, 1);
        this._window[3] = b;
      }
      if (this._winLen === 4 && this._window.toString('ascii') === ERHR_MAGIC) {
        this._window.copy(this._header, 0, 0, 4);
        this._headerGot = 4;
        this._winLen = 0;
        this._state = 'header';
      }
      return;
    }
    if (this._state === 'header') {
      this._header[this._headerGot++] = b;
      if (this._headerGot < ERHR_HEADER_SIZE) return;
      const version = this._header.readUInt8(4);
      this._type = this._header.readUInt8(5);
      this._payloadLen = this._header.readUInt32LE(8);
      this._payloadCrc = this._header.readUInt32LE(12);
      if (version !== ERHR_PROTO_VERSION) {
        this.onError(`unsupported protocol version ${version}`);
        this._reset();
        return;
      }
      if (this._payloadLen === 0) {
        this.onError('empty payload');
        this._reset();
        return;
      }
      if (this._payloadLen > this.maxPayload) {
        // Almost certainly a false magic in noise — drop it and keep scanning, don't allocate.
        this.onError(`payload too large (${this._payloadLen} bytes)`);
        this._reset();
        return;
      }
      this._payload = Buffer.alloc(this._payloadLen);
      this._payloadGot = 0;
      this._state = 'payload';
      return;
    }
    // payload
    this._payload[this._payloadGot++] = b;
    if (this._payloadGot < this._payloadLen) return;
    const got = crc32(this._payload);
    if (got !== this._payloadCrc) {
      this.onError(
        `payload CRC mismatch (got ${got.toString(16)}, want ${this._payloadCrc.toString(16)})`,
      );
      this._reset();
      return;
    }
    const payload = this._payload;
    const type = this._type;
    this._reset();
    this.onFrame(payload, {type});
  }
}
