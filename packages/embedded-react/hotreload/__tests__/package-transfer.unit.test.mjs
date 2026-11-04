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

import {describe, expect, it} from 'vitest';
import {
  PackageTransfer,
  beginFrame,
  chunkFrame,
  parseReply,
} from '../package-transfer.mjs';
import {
  FrameDecoder,
  MSG_PACKAGE_BEGIN,
  MSG_PACKAGE_CHUNK,
} from '../protocol.mjs';

const CHUNK_BYTES = 256;
const PACKAGE_BYTES = CHUNK_BYTES * 3 + 10;

const samplePackage = Buffer.from(
  Array.from({length: PACKAGE_BYTES}, (_, index) => index & 0xff),
);

/** What runtime/erb_package_reload.c does with frames, with hooks to get in its way. */
function fakeDevice({
  chunkBytes = CHUNK_BYTES,
  outcome = 'hot reload: ok (slot b, 778 bytes)',
  mangle = () => false,
} = {}) {
  const device = {received: null, offset: 0, frames: 0, begins: 0};
  const decoder = new FrameDecoder({
    onFrame: (payload, {type}) => {
      if (type === MSG_PACKAGE_BEGIN) {
        device.begins++;
        device.received = Buffer.alloc(payload.readUInt32LE(0));
        device.offset = 0;
        device.say(`I (1) embedded-react: erb reload: ready ${chunkBytes}`);
        return;
      }
      expect(type).toBe(MSG_PACKAGE_CHUNK);
      if (!device.received) return;
      const offset = payload.readUInt32LE(0);
      if (offset === device.offset) {
        payload.copy(device.received, offset, 4);
        device.offset += payload.length - 4;
      }
      device.say(`erb reload: got ${device.offset}`);
      if (device.offset === device.received.length) {
        const {received} = device;
        device.received = null;
        device.final = received;
        device.say(outcome);
      }
    },
    onError: () => device.say('dropped frame: transport CRC mismatch'),
  });
  device.write = async frame => {
    device.frames++;
    const action = mangle(device.frames);
    if (action === 'lose') return;
    if (action === 'corrupt') {
      const bad = Buffer.from(frame);
      bad[bad.length - 1] ^= 0xff;
      decoder.push(bad);
      return;
    }
    decoder.push(frame);
  };
  return device;
}

function connect(device, options = {}) {
  const transfer = new PackageTransfer({
    write: frame => device.write(frame),
    beginResendMs: 5,
    chunkTimeoutMs: 5,
    loadTimeoutMs: 20,
    ...options,
  });
  device.say = line => setImmediate(() => transfer.onLine(line));
  return transfer;
}

describe('frames', () => {
  it('begins with the package length', () => {
    const decoded = [];
    new FrameDecoder({onFrame: (payload, meta) => decoded.push([payload, meta])}).push(
      beginFrame(0x1234),
    );
    expect(decoded[0][1].type).toBe(MSG_PACKAGE_BEGIN);
    expect(decoded[0][0]).toEqual(Buffer.from([0x34, 0x12, 0, 0]));
  });

  it('puts the offset in front of each chunk, and cuts the last short', () => {
    const decoded = [];
    new FrameDecoder({onFrame: (payload, meta) => decoded.push([payload, meta])}).push(
      chunkFrame(samplePackage, CHUNK_BYTES * 3, CHUNK_BYTES),
    );
    expect(decoded[0][1].type).toBe(MSG_PACKAGE_CHUNK);
    expect(decoded[0][0].readUInt32LE(0)).toBe(CHUNK_BYTES * 3);
    expect(decoded[0][0].subarray(4)).toEqual(
      samplePackage.subarray(CHUNK_BYTES * 3),
    );
  });

  // The same bytes as frame() in erb_package_reload_test.c.
  it('matches the header the C parser reads', () => {
    const frame = beginFrame(0x01020304);
    expect(frame.subarray(0, 12)).toEqual(
      Buffer.from([
        0x45, 0x52, 0x48, 0x52, 1, MSG_PACKAGE_BEGIN, 0, 0, 4, 0, 0, 0,
      ]),
    );
  });
});

describe('parseReply', () => {
  it('reads the protocol lines, wherever a log prefix leaves them', () => {
    expect(
      parseReply('\u001b[0;32mI (812) embedded-react: erb reload: got 4096'),
    ).toEqual({kind: 'got', value: 4096});
    expect(parseReply('erb reload: ready 1024')).toEqual({
      kind: 'ready',
      value: 1024,
    });
    expect(parseReply('hot reload: ok (slot a, 10 bytes)').kind).toBe('ok');
    expect(parseReply('hot reload: no layout for 240 x 320').kind).toBe(
      'rejected',
    );
    expect(parseReply('dropped frame: payload exceeds buffer').kind).toBe(
      'dropped',
    );
    expect(parseReply('I (5) wifi: scan done')).toBeNull();
  });
});

describe('PackageTransfer', () => {
  it('sends a whole package', async () => {
    const device = fakeDevice();
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.final).toEqual(samplePackage);
    expect(device.frames).toBe(1 + 4); // begin + 3 whole chunks + the short last
  });

  it('uses the chunk size the device asks for', async () => {
    const device = fakeDevice({chunkBytes: 512});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.final).toEqual(samplePackage);
    expect(device.frames).toBe(1 + 2);
  });

  it('hides the protocol lines and shows the rest', () => {
    const transfer = connect(fakeDevice());
    expect(transfer.onLine('erb reload: got 256')).toBe(true);
    expect(transfer.onLine('erb reload: ready 256')).toBe(true);
    expect(transfer.onLine('hot reload: bad bytecode')).toBe(false);
    expect(transfer.onLine('I (1) app: hello')).toBe(false);
  });

  it('reports a package the device rejects', async () => {
    const device = fakeDevice({outcome: 'hot reload: bad template'});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('rejected');
  });

  it('reports a package the device refuses at the start', async () => {
    const device = fakeDevice();
    const transfer = connect(device);
    device.write = async () => device.say('hot reload: package slot: too big');
    expect(await transfer.send(samplePackage)).toBe('rejected');
    expect(device.received).toBeNull();
  });

  it('resends a begin until the device answers', async () => {
    const device = fakeDevice({mangle: frame => (frame < 4 ? 'lose' : false)});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.final).toEqual(samplePackage);
  });

  it('gives up when the device never answers', async () => {
    const device = fakeDevice({mangle: () => 'lose'});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage, {beginTimeoutMs: 20})).toBe(
      'timeout',
    );
  });

  it('resends a lost chunk', async () => {
    const device = fakeDevice({mangle: frame => frame === 3 && 'lose'});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.final).toEqual(samplePackage);
  });

  it('resends a corrupt chunk', async () => {
    const device = fakeDevice({mangle: frame => frame === 3 && 'corrupt'});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.final).toEqual(samplePackage);
  });

  it('gives up on a link that keeps losing chunks', async () => {
    const device = fakeDevice({mangle: frame => frame > 1 && 'lose'});
    const transfer = connect(device, {chunkResends: 2});
    expect(await transfer.send(samplePackage)).toBe('timeout');
    expect(device.frames).toBe(1 + 3);
  });

  it('goes back to where the device says it is', async () => {
    // A stray repeated begin restarts the device part way through.
    const device = fakeDevice();
    const transfer = connect(device);
    const write = device.write;
    let restarted = false;
    device.write = async frame => {
      if (device.offset === CHUNK_BYTES * 2 && !restarted) {
        restarted = true;
        await write(beginFrame(samplePackage.length));
      }
      await write(frame);
    };
    expect(await transfer.send(samplePackage)).toBe('ok');
    expect(device.begins).toBe(2);
    expect(device.final).toEqual(samplePackage);
  });

  it('takes an answer that overtakes the last chunk being resent', async () => {
    const device = fakeDevice();
    const transfer = connect(device);
    device.say = line => {
      // The reply to the last chunk goes missing, and only its load's answer arrives.
      if (line === `erb reload: got ${PACKAGE_BYTES}`) return;
      setImmediate(() => transfer.onLine(line));
    };
    expect(await transfer.send(samplePackage)).toBe('ok');
  });

  it('times out when the load never answers', async () => {
    const device = fakeDevice({outcome: 'I (9) app: log, not an answer'});
    const transfer = connect(device);
    expect(await transfer.send(samplePackage)).toBe('timeout');
  });

  it('ignores replies left over from the last send', async () => {
    const device = fakeDevice();
    const transfer = connect(device);
    transfer.onLine('hot reload: stale');
    expect(await transfer.send(samplePackage)).toBe('ok');
  });
});
