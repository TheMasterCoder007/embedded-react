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
  ERHR_HEADER_SIZE,
  ERHR_MAGIC,
  ERHR_PROTO_VERSION,
  FrameDecoder,
  MSG_LOAD_CONTAINER,
  encodeFrame,
} from '../protocol.mjs';

/** Collect frames + errors a decoder emits while fed the given chunks. */
function drive(chunks) {
  const frames = [];
  const errors = [];
  const dec = new FrameDecoder({
    onFrame: (payload, meta) => frames.push({payload, meta}),
    onError: reason => errors.push(reason),
  });
  for (const c of chunks) dec.push(c);
  return {frames, errors};
}

const sample = Buffer.from(
  'ERCF-pretend-this-is-a-real-container-blob-0123456789',
);

describe('encodeFrame', () => {
  it('writes a 16-byte ERHR header followed by the payload', () => {
    const frame = encodeFrame(sample);
    expect(frame.length).toBe(ERHR_HEADER_SIZE + sample.length);
    expect(frame.toString('ascii', 0, 4)).toBe(ERHR_MAGIC);
    expect(frame.readUInt8(4)).toBe(ERHR_PROTO_VERSION);
    expect(frame.readUInt8(5)).toBe(MSG_LOAD_CONTAINER);
    expect(frame.readUInt32LE(8)).toBe(sample.length);
    expect(frame.subarray(ERHR_HEADER_SIZE)).toEqual(sample);
  });

  it('throws on an empty payload', () => {
    expect(() => encodeFrame(Buffer.alloc(0))).toThrow(/payload/);
  });
});

describe('FrameDecoder roundtrip', () => {
  it('decodes a frame encoded by encodeFrame', () => {
    const {frames, errors} = drive([encodeFrame(sample)]);
    expect(errors).toEqual([]);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload).toEqual(sample);
    expect(frames[0].meta.type).toBe(MSG_LOAD_CONTAINER);
  });

  it('reassembles a frame split across arbitrary chunk boundaries', () => {
    const frame = encodeFrame(sample);
    const chunks = [];
    for (let i = 0; i < frame.length; i += 7)
      chunks.push(frame.subarray(i, i + 7));
    const {frames, errors} = drive(chunks);
    expect(errors).toEqual([]);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload).toEqual(sample);
  });

  it('feeding one byte at a time still completes the frame', () => {
    const frame = encodeFrame(sample);
    const {frames, errors} = drive([...frame].map(b => Buffer.from([b])));
    expect(errors).toEqual([]);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload).toEqual(sample);
  });

  it('decodes two back-to-back frames in one stream', () => {
    const a = encodeFrame(Buffer.from('first-payload'));
    const b = encodeFrame(Buffer.from('second-much-longer-payload-xyz'));
    const {frames, errors} = drive([Buffer.concat([a, b])]);
    expect(errors).toEqual([]);
    expect(frames.map(f => f.payload.toString())).toEqual([
      'first-payload',
      'second-much-longer-payload-xyz',
    ]);
  });
});

describe('FrameDecoder resync (in-band channel shared with device logs)', () => {
  it('skips leading log noise before the magic', () => {
    const noise = Buffer.from('I (1234) js: hello from device\nW (1250) wat\n');
    const {frames, errors} = drive([
      Buffer.concat([noise, encodeFrame(sample)]),
    ]);
    expect(errors).toEqual([]);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload).toEqual(sample);
  });

  it('skips noise BETWEEN two frames', () => {
    const a = encodeFrame(Buffer.from('alpha'));
    const log = Buffer.from('\nI (9999) js: reloaded ok\n');
    const b = encodeFrame(Buffer.from('beta'));
    const {frames, errors} = drive([Buffer.concat([a, log, b])]);
    expect(errors).toEqual([]);
    expect(frames.map(f => f.payload.toString())).toEqual(['alpha', 'beta']);
  });

  it('handles a false "ERH" prefix that is not a real magic', () => {
    // "ERHX...ERHR<frame>" — the decoder must not lock onto the bogus prefix.
    const junk = Buffer.from('ERHXnope ERHello ');
    const {frames, errors} = drive([
      Buffer.concat([junk, encodeFrame(sample)]),
    ]);
    expect(errors).toEqual([]);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload).toEqual(sample);
  });
});

describe('FrameDecoder rejects corrupt frames', () => {
  it('reports a CRC mismatch and recovers on the next good frame', () => {
    const frame = encodeFrame(sample);
    frame[ERHR_HEADER_SIZE + 3] ^= 0xff; // flip a payload byte; header CRC no longer matches
    const good = encodeFrame(Buffer.from('recovered'));
    const {frames, errors} = drive([Buffer.concat([frame, good])]);
    expect(errors).toHaveLength(1);
    expect(errors[0]).toMatch(/CRC/);
    expect(frames).toHaveLength(1);
    expect(frames[0].payload.toString()).toBe('recovered');
  });

  it('reports an unsupported protocol version', () => {
    const frame = encodeFrame(sample);
    frame[4] = 99; // bogus proto_version
    const {frames, errors} = drive([frame]);
    expect(frames).toHaveLength(0);
    expect(errors[0]).toMatch(/version/);
  });
});
