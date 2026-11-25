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

// The server side of a WebSocket, enough for the Flow B simulator's page and its debugger: whole text and binary
// messages each way, pings answered, no extensions.

import {createHash} from 'node:crypto';
import type {IncomingMessage} from 'node:http';
import type {Duplex} from 'node:stream';

const ACCEPT_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OPCODE_CONTINUATION = 0x0;
const OPCODE_TEXT = 0x1;
const OPCODE_BINARY = 0x2;
const OPCODE_CLOSE = 0x8;
const OPCODE_PING = 0x9;
const OPCODE_PONG = 0xa;
const FINAL = 0x80;
const MASKED = 0x80;
const LENGTH_16 = 126;
const LENGTH_64 = 127;
const MOST_16 = 0xffff;
/** The longest message taken from a client. Its messages are small JSON. */
const LONGEST_MESSAGE = 1 << 20;
/** The most a client may leave unread. One that falls further behind is dropped, and connects again. */
const MOST_UNREAD = 64 << 20;

/** One accepted connection. */
export interface WebSocketConnection {
  send(data: string | Uint8Array): void;
  close(): void;
  /** Called with each text message. */
  onText: (text: string) => void;
  /** Called once when the connection ends. */
  onClose: () => void;
}

function frame(opcode: number, payload: Uint8Array): Buffer {
  const length = payload.length;
  let header: Buffer;
  if (length < LENGTH_16) {
    header = Buffer.from([FINAL | opcode, length]);
  } else if (length <= MOST_16) {
    header = Buffer.alloc(4);
    header[0] = FINAL | opcode;
    header[1] = LENGTH_16;
    header.writeUInt16BE(length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = FINAL | opcode;
    header[1] = LENGTH_64;
    header.writeBigUInt64BE(BigInt(length), 2);
  }
  return Buffer.concat([header, payload]);
}

/**
 * Accepts a WebSocket upgrade.
 *
 * @param request  The upgrade request.
 * @param socket  Its socket.
 * @param head  What arrived after the request's headers.
 *
 * @returns The connection, or null when the request isn't a WebSocket's, with the socket closed.
 */
export function acceptWebSocket(
  request: IncomingMessage,
  socket: Duplex,
  head: Buffer,
): WebSocketConnection | null {
  const key = request.headers['sec-websocket-key'];
  if (
    typeof key !== 'string' ||
    String(request.headers.upgrade).toLowerCase() !== 'websocket'
  ) {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return null;
  }
  const accept = createHash('sha1')
    .update(key + ACCEPT_GUID)
    .digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
  );

  let open = true;
  let buffered: Buffer = head;
  let fragments: Buffer[] = [];
  const connection: WebSocketConnection = {
    send(data) {
      if (!open) return;
      if (socket.writableLength > MOST_UNREAD) {
        socket.destroy();
        ended();
        return;
      }
      socket.write(
        typeof data === 'string'
          ? frame(OPCODE_TEXT, Buffer.from(data, 'utf8'))
          : frame(OPCODE_BINARY, data),
      );
    },
    close() {
      if (!open) return;
      socket.write(frame(OPCODE_CLOSE, new Uint8Array(0)));
      socket.end();
      ended();
    },
    onText: () => {},
    onClose: () => {},
  };
  const ended = () => {
    if (!open) return;
    open = false;
    connection.onClose();
  };

  const read = () => {
    for (;;) {
      if (buffered.length < 2) return;
      const opcode = buffered[0] & 0x0f;
      const final = (buffered[0] & FINAL) !== 0;
      const masked = (buffered[1] & MASKED) !== 0;
      let length = buffered[1] & 0x7f;
      let at = 2;
      if (length === LENGTH_16) {
        if (buffered.length < at + 2) return;
        length = buffered.readUInt16BE(at);
        at += 2;
      } else if (length === LENGTH_64) {
        if (buffered.length < at + 8) return;
        length = Number(buffered.readBigUInt64BE(at));
        at += 8;
      }
      // A client's frames are masked, and none of this server's clients sends a long one.
      if (!masked || length > LONGEST_MESSAGE) {
        socket.destroy();
        ended();
        return;
      }
      if (buffered.length < at + 4 + length) return;
      const mask = buffered.subarray(at, at + 4);
      const payload = Buffer.from(buffered.subarray(at + 4, at + 4 + length));
      for (let index = 0; index < payload.length; index++) {
        payload[index] ^= mask[index % 4];
      }
      buffered = buffered.subarray(at + 4 + length);

      if (opcode === OPCODE_CLOSE) {
        connection.close();
        return;
      }
      if (opcode === OPCODE_PING) {
        socket.write(frame(OPCODE_PONG, payload));
        continue;
      }
      if (opcode === OPCODE_PONG) {
        continue;
      }
      if (opcode !== OPCODE_CONTINUATION) {
        fragments = [];
      }
      fragments.push(payload);
      const received = fragments.reduce(
        (total, fragment) => total + fragment.length,
        0,
      );
      if (received > LONGEST_MESSAGE) {
        socket.destroy();
        ended();
        return;
      }
      if (final) {
        const whole = Buffer.concat(fragments);
        fragments = [];
        connection.onText(whole.toString('utf8'));
      }
    }
  };

  socket.on('data', (chunk: Buffer) => {
    buffered = buffered.length ? Buffer.concat([buffered, chunk]) : chunk;
    read();
  });
  socket.on('close', ended);
  socket.on('error', ended);
  // The first message's listener is set after this returns.
  queueMicrotask(read);
  return connection;
}
