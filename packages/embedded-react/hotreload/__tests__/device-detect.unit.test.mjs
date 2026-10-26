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

// Auto-detect for `embedded-react dev --device`: pick the ESP32 USB-Serial-JTAG port by its fixed USB
// VID:PID and, on macOS, hand back the call-out (cu.) path. Pure functions — no serialport, no wasm.

import {describe, it, expect} from 'vitest';
import {
  isEspJtagPort,
  preferCalloutPath,
  ESP_USB_JTAG_VID,
  ESP_USB_JTAG_PID,
} from '../uploader.mjs';

describe('device auto-detect', () => {
  it('exposes the ESP32 USB-Serial-JTAG id constants', () => {
    expect([ESP_USB_JTAG_VID, ESP_USB_JTAG_PID]).toEqual(['303a', '1001']);
  });

  it('matches the ESP32 USB-Serial-JTAG VID:PID (case-insensitive)', () => {
    expect(isEspJtagPort({vendorId: '303a', productId: '1001'})).toBe(true);
    expect(isEspJtagPort({vendorId: '303A', productId: '1001'})).toBe(true);
  });

  it('rejects the CH34x flashing bridge and anything else', () => {
    expect(isEspJtagPort({vendorId: '1a86', productId: '55d3'})).toBe(false); // WCH CH343 (UART)
    expect(isEspJtagPort({vendorId: '303a'})).toBe(false); // JTAG VID but no/other PID
    expect(isEspJtagPort({})).toBe(false); // a plain /dev/tty.* with no USB ids
    expect(isEspJtagPort(null)).toBe(false);
  });

  it('maps tty.* → cu.* on macOS, and passes other paths through unchanged', () => {
    const tty = '/dev/tty.usbmodem1401';
    expect(preferCalloutPath(tty)).toBe(
      process.platform === 'darwin' ? '/dev/cu.usbmodem1401' : tty,
    );
    expect(preferCalloutPath('/dev/cu.usbmodem1401')).toBe(
      '/dev/cu.usbmodem1401',
    );
    expect(preferCalloutPath('COM5')).toBe('COM5');
  });
});
