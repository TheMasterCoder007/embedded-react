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

// The images an .svg embeds as data: URLs, decoded to PNGs the image baker registers. Each is named
// after a hash of its pixels, so a tape's image op can name it by number (IMAGE_PREFIX and six hex digits).
import {createHash} from 'node:crypto';
import {PNG} from 'pngjs';
import {IMAGE_ID_MAX, IMAGE_PREFIX} from '../src/embedded-react/svg-ops.mts';

/** An embedded image, ready to register. */
export interface EmbeddedImage {
  /** The number the tape names it by. */
  id: number;
  /** Its asset name: IMAGE_PREFIX and the id in six hex digits. */
  name: string;
  png: Buffer;
  width: number;
  height: number;
}

const ID_DIGITS = 6;
const HEX = 16;
const GIF_WIDTH_AT = 6;
const GIF_HEIGHT_AT = 8;
const JPEG_SIZE_MARKERS = /^(c[0-35-79a-bd-f])$/; // SOF0-SOF15, not DHT (c4), JPG (c8) or DAC (cc)

// Each id's full hash, so two different images that share an id fail the build instead of swapping.
const hashesById = new Map<number, string>();

/**
 * Decodes each data: URL an <image> gives.
 *
 * @returns By URL: the image, or why it cannot be drawn as one.
 */
export async function decodeEmbeddedImages(
  urls: Iterable<string>,
): Promise<Map<string, EmbeddedImage | string>> {
  const decoded = new Map<string, EmbeddedImage | string>();
  for (const url of urls) {
    if (!decoded.has(url)) decoded.set(url, await decodeImage(url));
  }
  return decoded;
}

/** One data: URL as a PNG, or why it is not one. */
async function decodeImage(url: string): Promise<EmbeddedImage | string> {
  const match = url.match(/^data:([^;,]*)((?:;[^;,]*)*),([\s\S]*)$/i);
  if (!match) return 'image with a malformed data: URL';
  const type = match[1].trim().toLowerCase() || 'text/plain';
  const bytes = /;base64/i.test(match[2])
    ? Buffer.from(match[3].replace(/\s+/g, ''), 'base64')
    : Buffer.from(decodeURIComponent(match[3]), 'latin1');

  let png: Buffer;
  let width: number;
  let height: number;
  if (type === 'image/png') {
    try {
      ({width, height} = PNG.sync.read(bytes));
    } catch {
      return 'image that is not a readable PNG';
    }
    png = bytes;
  } else if (
    type === 'image/jpeg' ||
    type === 'image/jpg' ||
    type === 'image/gif'
  ) {
    const size = type === 'image/gif' ? gifSize(bytes) : jpegSize(bytes);
    if (!size)
      return `image that is not a readable ${type.slice(6).toUpperCase()}`;
    ({width, height} = size);
    png = await toPng(url, width, height);
  } else {
    return `image of type ${type}`;
  }
  if (!(width > 0 && height > 0)) return 'image of no size';
  return namedImage(png, width, height);
}

/** Names a PNG the tape draws after a hash of its bytes. */
export function namedImage(
  png: Buffer,
  width: number,
  height: number,
): EmbeddedImage {
  const hash = createHash('sha1').update(png).digest('hex');
  const id = parseInt(hash.slice(0, ID_DIGITS), HEX) & IMAGE_ID_MAX;
  const known = hashesById.get(id);
  if (known && known !== hash) {
    throw new Error(
      `two images in .svg files share the id ${id.toString(HEX)}; change one of them by a pixel`,
    );
  }
  hashesById.set(id, hash);
  const name = IMAGE_PREFIX + id.toString(HEX).padStart(ID_DIGITS, '0');
  return {id, name, png, width, height};
}

/** A JPEG's size, from its first frame header. */
function jpegSize(bytes: Buffer): {width: number; height: number} | null {
  if (bytes[0] !== 0xff || bytes[1] !== 0xd8) return null;
  let offset = 2;
  while (offset + 9 <= bytes.length) {
    if (bytes[offset] !== 0xff) return null;
    const marker = bytes[offset + 1];
    if (marker === 0xff) {
      offset++;
      continue;
    }
    if (JPEG_SIZE_MARKERS.test(marker.toString(HEX)))
      return {
        height: bytes.readUInt16BE(offset + 5),
        width: bytes.readUInt16BE(offset + 7),
      };
    offset += 2 + bytes.readUInt16BE(offset + 2);
  }
  return null;
}

/** A GIF's size, from its header. */
function gifSize(bytes: Buffer): {width: number; height: number} | null {
  if (
    bytes.length < GIF_HEIGHT_AT + 2 ||
    bytes.toString('latin1', 0, 4) !== 'GIF8'
  )
    return null;
  return {
    width: bytes.readUInt16LE(GIF_WIDTH_AT),
    height: bytes.readUInt16LE(GIF_HEIGHT_AT),
  };
}

/** Draws an image resvg can read into a PNG of its own size. */
async function toPng(
  url: string,
  width: number,
  height: number,
): Promise<Buffer> {
  const {Resvg} = await import('@resvg/resvg-js');
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}">` +
    `<image href="${url}" width="${width}" height="${height}" preserveAspectRatio="none"/></svg>`;
  const rendered = new Resvg(svg, {background: 'rgba(0,0,0,0)'}).render();
  const png = rendered.asPng();
  // Only the wasm build has free().
  (rendered as {free?: () => void}).free?.();
  return png;
}
