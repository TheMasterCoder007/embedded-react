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

// package-transfer — sends a Flow B package to a board, as ERHR frames, one at a time.
//
// The device writes each chunk straight into its staging slot, and flash stalls the chip while it does. So this sends
// one frame and waits for the reply, which keeps a UART with no flow control from losing bytes. The device's side is
// runtime/erb_package_reload.c, whose header lists the replies.

import {
  MSG_PACKAGE_BEGIN,
  MSG_PACKAGE_CHUNK,
  encodeFrame,
} from './protocol.mjs';

const CHUNK_OFFSET_BYTES = 4;
const BEGIN_RESEND_MS = 1000; // a board that is resetting, or holding a boot error on screen, misses a begin
const BEGIN_TIMEOUT_MS = 15000;
const CHUNK_TIMEOUT_MS = 2000;
const CHUNK_RESENDS = 5;
const LOAD_TIMEOUT_MS = 15000; // checking a large package from flash takes a slow board a few seconds

const READY_LINE = /erb reload: ready (\d+)/;
const GOT_LINE = /erb reload: got (\d+)/;

/** What a line from the device says, or null when it is only a log. */
export function parseReply(line) {
  const ready = READY_LINE.exec(line);
  if (ready) return {kind: 'ready', value: Number(ready[1])};
  const got = GOT_LINE.exec(line);
  if (got) return {kind: 'got', value: Number(got[1])};
  if (/dropped frame/i.test(line)) return {kind: 'dropped'};
  if (/hot reload:\s*ok\b/i.test(line)) return {kind: 'ok'};
  if (/hot reload:/i.test(line)) return {kind: 'rejected'};
  return null;
}

/** The frame that starts a package. */
export function beginFrame(packageBytes) {
  const payload = Buffer.alloc(CHUNK_OFFSET_BYTES);
  payload.writeUInt32LE(packageBytes, 0);
  return encodeFrame(payload, {type: MSG_PACKAGE_BEGIN});
}

/** The frame for the bytes of @p packageData at @p offset. */
export function chunkFrame(packageData, offset, chunkBytes) {
  const bytes = packageData.subarray(offset, offset + chunkBytes);
  const payload = Buffer.alloc(CHUNK_OFFSET_BYTES + bytes.length);
  payload.writeUInt32LE(offset, 0);
  bytes.copy(payload, CHUNK_OFFSET_BYTES);
  return encodeFrame(payload, {type: MSG_PACKAGE_CHUNK});
}

export class PackageTransfer {
  /**
   * @param {object} o
   * @param {(frame: Buffer) => Promise<unknown>} o.write  Writes a frame to the device.
   */
  constructor({
    write,
    beginResendMs = BEGIN_RESEND_MS,
    chunkTimeoutMs = CHUNK_TIMEOUT_MS,
    chunkResends = CHUNK_RESENDS,
    loadTimeoutMs = LOAD_TIMEOUT_MS,
  }) {
    this.write = write;
    this.beginResendMs = beginResendMs;
    this.chunkTimeoutMs = chunkTimeoutMs;
    this.chunkResends = chunkResends;
    this.loadTimeoutMs = loadTimeoutMs;
    this._replies = [];
    this._wake = null;
  }

  /** Takes a line from the device. Returns true when it is part of the transfer and not worth showing. */
  onLine(line) {
    const reply = parseReply(line);
    if (!reply) return false;
    this._replies.push(reply);
    if (this._wake) this._wake();
    return reply.kind === 'ready' || reply.kind === 'got';
  }

  /** The next reply, or null after @p timeoutMs. */
  async _nextReply(timeoutMs) {
    if (!this._replies.length) {
      await new Promise(resolve => {
        const timer = setTimeout(resolve, timeoutMs);
        this._wake = () => {
          clearTimeout(timer);
          resolve();
        };
      });
      this._wake = null;
    }
    return this._replies.shift() ?? null;
  }

  /** Starts the package on the device. Returns its chunk size, or the outcome when it never got that far. */
  async _begin(packageData, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    const frame = beginFrame(packageData.length);
    while (Date.now() < deadline) {
      await this.write(frame);
      const waitUntil = Math.min(Date.now() + this.beginResendMs, deadline);
      for (;;) {
        const reply = await this._nextReply(waitUntil - Date.now());
        if (!reply || reply.kind === 'dropped') break; // send it again
        if (reply.kind === 'ready') return {chunkBytes: reply.value};
        if (reply.kind === 'rejected') return {outcome: 'rejected'};
      }
    }
    return {outcome: 'timeout'};
  }

  /**
   * Sends one package and waits for the device to load it.
   *
   * @param {Buffer} packageData
   * @param {object} [o]
   * @param {number} [o.beginTimeoutMs]  How long a device may take to answer at all.
   * @returns {Promise<'ok'|'rejected'|'dropped'|'timeout'>}
   */
  async send(packageData, {beginTimeoutMs = BEGIN_TIMEOUT_MS} = {}) {
    this._replies = [];
    const begun = await this._begin(packageData, beginTimeoutMs);
    if (begun.outcome) return begun.outcome;
    const {chunkBytes} = begun;

    let offset = 0;
    let resends = 0;
    while (offset < packageData.length) {
      await this.write(chunkFrame(packageData, offset, chunkBytes));
      const reply = await this._awaitChunkReply();
      if (reply === 'ok' || reply === 'rejected') return reply;
      if (reply === null || reply === 'dropped') {
        if (++resends > this.chunkResends) return reply ?? 'timeout';
        continue;
      }
      // Where the device says it has got to, even if that is behind: a repeated begin starts it over.
      if (reply > offset) resends = 0;
      offset = reply;
    }

    for (;;) {
      const reply = await this._nextReply(this.loadTimeoutMs);
      if (!reply) return 'timeout';
      if (reply.kind === 'ok' || reply.kind === 'rejected') return reply.kind;
    }
  }

  /** What the device said about a chunk: its offset, 'ok', 'rejected', 'dropped', or null for nothing. */
  async _awaitChunkReply() {
    const waitUntil = Date.now() + this.chunkTimeoutMs;
    for (;;) {
      const reply = await this._nextReply(waitUntil - Date.now());
      if (!reply) return null;
      if (reply.kind === 'got') return reply.value;
      if (reply.kind !== 'ready') return reply.kind;
    }
  }
}
