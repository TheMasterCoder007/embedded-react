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

// uploader — writes ERHR-framed .erpkg containers to a device over a serial/USB port, and surfaces the
// device's log output coming back the other way (CDC is full-duplex, so one port carries both).
//
// `serialport` is an optional, lazily-loaded dependency: it has a native binding, so we don't force it
// on everyone who just wants the simulator or the AOT path. The first time you upload to a device, the
// CLI asks you to `npm i serialport` if it isn't present.

import {encodeFrame} from './protocol.mjs';

/** Loads the `serialport` package, with a friendly message if it isn't installed. */
async function loadSerialPort() {
  try {
    return await import('serialport');
  } catch {
    throw new Error(
      "on-device upload needs the 'serialport' package.\n" +
        '  Install it in your project:  npm i serialport\n' +
        '  (it has a native binding, so embedded-react keeps it optional.)',
    );
  }
}

/**
 * Opens a serial port and streams hot-reload uploads to it.
 *
 * @param {object}   o
 * @param {string}   o.device          Port path (e.g. /dev/tty.usbmodem1234 or COM5).
 * @param {number}   [o.baud]          Baud rate. Ignored by true USB-CDC links, required by UART bridges.
 * @param {(line: string) => void} [o.onLog]  Called with each line the device prints back.
 */
export class SerialUploader {
  constructor({device, baud = 460800, onLog, paced = true} = {}) {
    if (!device) throw new Error('SerialUploader: device path is required');
    this.device = device;
    this.baud = baud;
    this.onLog = onLog || (() => {});
    // paced=true (default): stream the frame in chunks, draining each, so the write is metered to roughly
    // the device's drain rate. Over USB this keeps the host from flooding the RX buffer while the running
    // app competes for the port; on a raw UART bridge (no flow control) it keeps the host under the line
    // rate so bytes aren't dropped. paced=false: stream the whole frame in one write, relying on the
    // link's own back-pressure — slightly less setup, but can stall mid-transfer when the device is busy.
    this.paced = paced;
    this._port = null;
    this._rxLine = '';
  }

  /** Opens the port. Resolves once it is ready to accept writes. */
  async open() {
    const {SerialPort} = await loadSerialPort();
    await new Promise((res, rej) => {
      this._port = new SerialPort(
        {path: this.device, baudRate: this.baud},
        err => (err ? rej(err) : res()),
      );
    });
    // Surface device logs line-by-line (the data arriving between/around our uploads).
    this._port.on('data', chunk => this._onData(chunk));
    this._port.on('error', err => this.onLog(`[serial error] ${err.message}`));
  }

  _onData(chunk) {
    this._rxLine += chunk.toString('utf8');
    let nl;
    while ((nl = this._rxLine.indexOf('\n')) >= 0) {
      const line = this._rxLine.slice(0, nl).replace(/\r$/, '');
      this._rxLine = this._rxLine.slice(nl + 1);
      if (line.length) this.onLog(line);
    }
  }

  _write(buf) {
    return new Promise((res, rej) =>
      this._port.write(buf, err => (err ? rej(err) : res())),
    );
  }

  _drain() {
    return new Promise((res, rej) =>
      this._port.drain(err => (err ? rej(err) : res())),
    );
  }

  /**
   * Frames an .erpkg and writes it to the device.
   *
   * Paced path (paced=true, the default): stream the frame in chunks, draining each, so the write is
   * metered to roughly the device's drain rate — over USB that keeps the host from flooding the RX buffer
   * while the running app competes for the port, and on a raw UART bridge (no flow control) it keeps the
   * host under the line rate so bytes aren't dropped.
   *
   * Fast path (paced=false): stream the whole frame in one write, relying on the link's own back-pressure.
   *
   * @param {Buffer|Uint8Array} erpkg  Container bytes (from packAppContainer).
   * @returns {Promise<number>} The number of framed bytes written.
   */
  async send(erpkg) {
    if (!this._port) throw new Error('SerialUploader: call open() first');
    const frame = encodeFrame(erpkg);

    if (!this.paced) {
      await this._write(frame); // rely on the link's back-pressure (USB NAK) to throttle us
      await this._drain();
      return frame.length;
    }

    const CHUNK = 16 * 1024;
    for (let off = 0; off < frame.length; off += CHUNK) {
      await this._write(frame.subarray(off, off + CHUNK));
      await this._drain(); // meter to ~the device's drain rate so its RX buffer isn't overrun
    }
    return frame.length;
  }

  /** Closes the port. */
  async close() {
    if (!this._port) return;
    await new Promise(res => this._port.close(() => res()));
    this._port = null;
  }
}

/**
 * Lists serial ports, best-effort, to help the user pick a --device. Returns [] if serialport is absent.
 *
 * @returns {Promise<Array<{path: string, manufacturer?: string}>>}
 */
export async function listSerialPorts() {
  try {
    const {SerialPort} = await loadSerialPort();
    return await SerialPort.list();
  } catch {
    return [];
  }
}
