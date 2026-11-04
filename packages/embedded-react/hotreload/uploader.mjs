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

const DEFAULT_BAUD = 460800;
const PACED_CHUNK_BYTES = 16 * 1024;

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
 * @param {object}   options
 * @param {string}   options.device          Port path (e.g. /dev/tty.usbmodem1234 or COM5).
 * @param {number}   [options.baud]          Baud rate. Ignored by true USB-CDC links, required by UART bridges.
 * @param {(line: string) => void} [options.onLog]  Called with each line the device prints back.
 */
export class SerialUploader {
  constructor({device, baud = DEFAULT_BAUD, onLog, paced = true} = {}) {
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
    this._pendingLine = '';
  }

  /** Opens the port. Resolves once it is ready to accept writes. */
  async open() {
    const {SerialPort} = await loadSerialPort();
    await new Promise((resolve, reject) => {
      this._port = new SerialPort(
        {path: this.device, baudRate: this.baud},
        error => (error ? reject(error) : resolve()),
      );
    });
    // Surface device logs line-by-line (the data arriving between/around our uploads).
    this._port.on('data', chunk => this._onData(chunk));
    this._port.on('error', error =>
      this.onLog(`[serial error] ${error.message}`),
    );
  }

  _onData(chunk) {
    this._pendingLine += chunk.toString('utf8');
    let newline;
    while ((newline = this._pendingLine.indexOf('\n')) >= 0) {
      const line = this._pendingLine.slice(0, newline).replace(/\r$/, '');
      this._pendingLine = this._pendingLine.slice(newline + 1);
      if (line.length) this.onLog(line);
    }
  }

  _write(data) {
    return new Promise((resolve, reject) =>
      this._port.write(data, error => (error ? reject(error) : resolve())),
    );
  }

  _drain() {
    return new Promise((resolve, reject) =>
      this._port.drain(error => (error ? reject(error) : resolve())),
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

    for (let offset = 0; offset < frame.length; offset += PACED_CHUNK_BYTES) {
      await this._write(frame.subarray(offset, offset + PACED_CHUNK_BYTES));
      await this._drain(); // meter to ~the device's drain rate so its RX buffer isn't overrun
    }
    return frame.length;
  }

  /** Writes a frame that is already encoded, for a transfer that waits for a reply to each. */
  async writeFrame(frame) {
    if (!this._port) throw new Error('SerialUploader: call open() first');
    await this._write(frame);
    await this._drain();
  }

  /** Closes the port. */
  async close() {
    if (!this._port) return;
    await new Promise(resolve => this._port.close(() => resolve()));
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

// The ESP32-S3/C3 built-in USB-Serial-JTAG enumerates with this fixed USB VID:PID, baked into the chip's
// ROM independently of the board vendor — so it's a stable signal for auto-detecting the hot-reload port.
// (The CH34x/CP210x/FTDI UART bridge used for *flashing* has a different id, e.g. 1a86:xxxx for WCH.)
export const ESP_USB_JTAG_VID = '303a';
export const ESP_USB_JTAG_PID = '1001';

const hex = value => (value == null ? '' : String(value).toLowerCase());

/** True if a `SerialPort.list()` entry is an ESP32-S3/C3 native USB-Serial-JTAG port. */
export function isEspJtagPort(port) {
  return (
    hex(port?.vendorId) === ESP_USB_JTAG_VID &&
    hex(port?.productId) === ESP_USB_JTAG_PID
  );
}

/**
 * On macOS a USB serial device appears as both `/dev/tty.*` (dial-in, blocks until carrier-detect) and
 * `/dev/cu.*` (call-out, the one to use for an outgoing link). `SerialPort.list()` reports the tty.* side;
 * map it to cu.* on darwin so an auto-detected port opens without hanging. No-op elsewhere.
 */
export function preferCalloutPath(path) {
  if (process.platform === 'darwin' && path && path.startsWith('/dev/tty.')) {
    return `/dev/cu.${path.slice('/dev/tty.'.length)}`;
  }
  return path;
}

/**
 * Enumerate serial ports and pick out the ESP32 USB-Serial-JTAG candidates for auto-detecting `--device`.
 * Distinguishes "serialport not installed" from "installed but nothing matched" so the CLI can advise.
 *
 * @returns {Promise<{matches: object[], all: object[], serialportMissing: boolean}>}
 */
export async function detectDevicePorts() {
  let SerialPort;
  try {
    ({SerialPort} = await import('serialport'));
  } catch {
    return {matches: [], all: [], serialportMissing: true};
  }
  let all = [];
  try {
    all = await SerialPort.list();
  } catch {
    all = [];
  }
  return {matches: all.filter(isEspJtagPort), all, serialportMissing: false};
}
