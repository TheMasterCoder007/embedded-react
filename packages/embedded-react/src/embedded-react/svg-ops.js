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

// Pure SVG -> engine op-tape compiler. No engine/React imports, so it is unit-testable in isolation.
//
// The engine's ER_NODE_VECTOR node takes a flat float "op-tape" plus a flat paint table (see the
// ER_VECTOR_OP_* contract in er_scene.h). This module compiles a declarative <Svg> subtree (Path/Circle/
// Rect/Line/Ellipse/G elements) into that form: it parses SVG `d` path data, converts primitive
// shapes to path ops, bakes viewBox + <G> translate/scale into coordinates, and resolves inherited
// paint. The opcodes below MUST stay in sync with er_scene.h.

import {NAMED_COLORS} from './named-colors.js';

// Op-tape vocabulary — exported so the build-time SVG baker (assets/bake-svg.mjs) can reuse it without
// duplicating the encoding (and without pulling its SVG-parser dependency into this runtime module).
export const VOP_SHAPE = 0;
export const VOP_MOVE = 1;
export const VOP_LINE = 2;
export const VOP_QUAD = 3;
export const VOP_CUBIC = 4;
export const VOP_ARC = 5;
export const VOP_CLOSE = 6;

// Control-point ratio for approximating a quarter (circular or elliptical) arc with one cubic:
// 4/3 * (sqrt(2) - 1). Used by rounded-rect corners.
export const KAPPA = 0.5522847498307936;

export const CAP = {butt: 0, round: 1, square: 2};
export const JOIN = {miter: 0, round: 1, bevel: 2};

// Paint record width in the flat paint table: [fill, stroke, strokeWidth, miter, cap, join, fillRule,
// fillGradient, strokeGradient]. fillGradient/strokeGradient are 1-based indices into the gradient table (0 = solid). MUST
// match the bridge's VEC_PAINT_STRIDE (native_ui_bridge.c) and the AOT's paint emission.
export const PAINT_STRIDE = 9;

// Gradient table encoding — a gradient descriptor is { type, stops: [{color, offset}], ax, ay, bx, by, r }.
// type 1 = linear (axis (ax,ay)->(bx,by)), 2 = radial (centre (ax,ay), radius r). The flat float record is
// [type, stopCount, (color, offset) × GRAD_MAX_STOPS, ax, ay, bx, by, r]; MUST match the bridge's
// VEC_GRADIENT_STRIDE and the engine's ER_VECTOR_GRADIENT_MAX_STOPS.
export const GRAD_MAX_STOPS = 8; // MUST match the engine's ER_VECTOR_GRADIENT_MAX_STOPS (er_scene.h)
export const GRAD_STRIDE = 2 + GRAD_MAX_STOPS * 2 + 5; // 23
export const GRAD_LINEAR = 1;
export const GRAD_RADIAL = 2;
export const GRAD_CONIC = 3;

const ALPHA_OPAQUE = 255;
const CHANNEL_MAX = 255;
const OPAQUE_BLACK = 0xff000000;

const HALF_TURN_DEGREES = 180;
const QUARTER_TURN_RADIANS = Math.PI / 2;
const FULL_TURN_RADIANS = 2 * Math.PI;
// Arc convenience angles start at 12 o'clock; the engine's start at +X, a quarter turn earlier.
const TWELVE_OCLOCK_DEGREES = 90;

const DEFAULT_STROKE_WIDTH = 1;
const DEFAULT_MITER_LIMIT = 4;
const FILL_RULE_NONZERO = 0;
const FILL_RULE_EVEN_ODD = 1;

// A shape's tape starts with VOP_SHAPE and its paint index.
const SHAPE_HEADER_LENGTH = 2;
// Where strokeWidth sits in a paint record (see PAINT_STRIDE).
const PAINT_STROKE_WIDTH_SLOT = 2;

/** Linearly interpolates two straight-alpha ARGB8888 colors, per channel. */
function lerpArgb(fromColor, toColor, fraction) {
  const channel = shift => {
    const fromChannel = (fromColor >>> shift) & 0xff;
    const toChannel = (toColor >>> shift) & 0xff;
    return (
      Math.round(fromChannel + (toChannel - fromChannel) * fraction) & 0xff
    );
  };
  return (
    ((channel(24) << 24) |
      (channel(16) << 16) |
      (channel(8) << 8) |
      channel(0)) >>>
    0
  );
}

/** Gradient color at `position` (stops ascending by offset; clamps to the endpoint colors). */
function evalStopsAt(stops, position) {
  if (!stops.length) return 0;
  if (position <= stops[0].offset) return stops[0].color >>> 0;
  const last = stops[stops.length - 1];
  if (position >= last.offset) return last.color >>> 0;
  for (let index = 0; index < stops.length - 1; index++) {
    const before = stops[index];
    const after = stops[index + 1];
    if (position <= after.offset) {
      const span = after.offset - before.offset;
      return lerpArgb(
        before.color >>> 0,
        after.color >>> 0,
        span > 0 ? (position - before.offset) / span : 0,
      );
    }
  }
  return last.color >>> 0;
}

/** Reduces a stop list to at most `maxStops` entries, resampling evenly across the stop range when it's over. */
function capStops(stops, maxStops) {
  if (stops.length <= maxStops) return stops;
  const firstOffset = stops[0].offset;
  const lastOffset = stops[stops.length - 1].offset;
  const resampled = [];
  for (let index = 0; index < maxStops; index++) {
    const offset =
      maxStops === 1
        ? firstOffset
        : firstOffset + (lastOffset - firstOffset) * (index / (maxStops - 1));
    resampled.push({color: evalStopsAt(stops, offset), offset: offset});
  }
  return resampled;
}

/** Encodes gradient descriptors into the flat float array NativeUI.setVectorOps takes (GRAD_STRIDE/gradient).
 *  A gradient with more than GRAD_MAX_STOPS stops is resampled down (not truncated) so it still ramps right. */
export function encodeVectorGradients(gradients) {
  const encoded = [];
  if (!gradients) return encoded;
  for (const gradient of gradients) {
    const stops = capStops(gradient.stops || [], GRAD_MAX_STOPS);
    encoded.push(gradient.type, stops.length);
    for (let stopIndex = 0; stopIndex < GRAD_MAX_STOPS; stopIndex++) {
      const stop = stops[stopIndex];
      encoded.push(stop ? stop.color >>> 0 : 0, stop ? stop.offset : 0);
    }
    encoded.push(
      gradient.ax || 0,
      gradient.ay || 0,
      gradient.bx || 0,
      gradient.by || 0,
      gradient.r || 0,
    );
  }
  return encoded;
}

// The shared named colors plus SVG's `none`; everything else goes through hex/rgb parsing.
const SVG_NAMED_COLORS = Object.freeze(
  Object.assign(Object.create(null), NAMED_COLORS, {none: 0}),
);

// Cache string->uint32 results: a theme reuses a handful of color strings, so on a continuous drag
// (which recompiles the dial every move) this turns repeated hex parsing into a Map lookup.
const _colorCache = new Map();

/**
 * Parses a CSS color string to a uint32 ARGB8888. Returns 0 (fully transparent) for none/transparent/
 * null, so the engine treats it as "no fill"/"no stroke". Accepts #rgb, #rgba, #rrggbb, #rrggbbaa,
 * rgb()/rgba(), and the named colors above. String results are memoised (see _colorCache).
 */
export function parseColor(color) {
  if (color == null || color === false) return 0;
  if (typeof color === 'number') return color >>> 0;
  const cached = _colorCache.get(color);
  if (cached !== undefined) return cached;
  const parsed = parseColorString(color);
  _colorCache.set(color, parsed);
  return parsed;
}

function parseColorString(color) {
  let text = String(color).trim().toLowerCase();
  if (text in SVG_NAMED_COLORS) return SVG_NAMED_COLORS[text] >>> 0;
  if (text[0] === '#') {
    text = text.slice(1);
    let red,
      green,
      blue,
      alpha = ALPHA_OPAQUE;
    if (text.length === 3 || text.length === 4) {
      red = parseInt(text[0] + text[0], 16);
      green = parseInt(text[1] + text[1], 16);
      blue = parseInt(text[2] + text[2], 16);
      if (text.length === 4) alpha = parseInt(text[3] + text[3], 16);
    } else if (text.length === 6 || text.length === 8) {
      red = parseInt(text.slice(0, 2), 16);
      green = parseInt(text.slice(2, 4), 16);
      blue = parseInt(text.slice(4, 6), 16);
      if (text.length === 8) alpha = parseInt(text.slice(6, 8), 16);
    } else {
      return OPAQUE_BLACK;
    }
    return ((alpha << 24) | (red << 16) | (green << 8) | blue) >>> 0;
  }
  if (text.startsWith('rgb')) {
    const match = text.match(/rgba?\(([^)]+)\)/);
    if (match) {
      const parts = match[1].split(',').map(part => part.trim());
      const red = parseInt(parts[0], 10) || 0;
      const green = parseInt(parts[1], 10) || 0;
      const blue = parseInt(parts[2], 10) || 0;
      const alpha =
        parts[3] != null
          ? Math.round(parseFloat(parts[3]) * CHANNEL_MAX)
          : ALPHA_OPAQUE;
      return ((alpha << 24) | (red << 16) | (green << 8) | blue) >>> 0;
    }
  }
  return OPAQUE_BLACK; // unknown -> opaque black (SVG-ish default)
}

// --- SVG `d` path-data parser -------------------------------------------------------------------

// Hoisted so the pattern is compiled once at module load, not on every parsePath call. The /g lastIndex is reset below.
const PATH_RE = /([MmLlHhVvCcSsQqTtAaZz])|(-?\d*\.?\d+(?:[eE][-+]?\d+)?)/g;

/** Tokenizes a `d` string into {command, values[]} groups (one per command letter). */
function tokenizePath(d) {
  const commands = [];
  const pattern = PATH_RE;
  pattern.lastIndex = 0;
  let match;
  let current = null;
  while ((match = pattern.exec(d)) !== null) {
    if (match[1]) {
      current = {command: match[1], values: []};
      commands.push(current);
    } else if (current) {
      current.values.push(parseFloat(match[2]));
    }
  }
  return commands;
}

/** Converts an SVG elliptical arc (endpoint form) to a list of cubic bezier segments. */
function arcToCubics(x0, y0, rx, ry, rotationDegrees, largeArc, sweep, x, y) {
  const cubics = [];
  if (rx === 0 || ry === 0) {
    cubics.push([x0, y0, x, y, x, y]); // degenerate -> straight line as a cubic
    return cubics;
  }
  rx = Math.abs(rx);
  ry = Math.abs(ry);
  const rotation = (rotationDegrees * Math.PI) / HALF_TURN_DEGREES;
  const cosRotation = Math.cos(rotation);
  const sinRotation = Math.sin(rotation);
  const halfDeltaX = (x0 - x) / 2;
  const halfDeltaY = (y0 - y) / 2;
  const primeX = cosRotation * halfDeltaX + sinRotation * halfDeltaY;
  const primeY = -sinRotation * halfDeltaX + cosRotation * halfDeltaY;
  let rxSquared = rx * rx;
  let rySquared = ry * ry;
  const primeXSquared = primeX * primeX;
  const primeYSquared = primeY * primeY;
  // Correct out-of-range radii.
  const radiiScale = primeXSquared / rxSquared + primeYSquared / rySquared;
  if (radiiScale > 1) {
    const scale = Math.sqrt(radiiScale);
    rx *= scale;
    ry *= scale;
    rxSquared = rx * rx;
    rySquared = ry * ry;
  }
  let sign = largeArc !== sweep ? 1 : -1;
  let numerator =
    rxSquared * rySquared -
    rxSquared * primeYSquared -
    rySquared * primeXSquared;
  if (numerator < 0) numerator = 0;
  const coefficient =
    sign *
    Math.sqrt(
      numerator / (rxSquared * primeYSquared + rySquared * primeXSquared || 1),
    );
  const primeCenterX = (coefficient * rx * primeY) / ry;
  const primeCenterY = (-coefficient * ry * primeX) / rx;
  const cx =
    cosRotation * primeCenterX - sinRotation * primeCenterY + (x0 + x) / 2;
  const cy =
    sinRotation * primeCenterX + cosRotation * primeCenterY + (y0 + y) / 2;
  const angleBetween = (fromX, fromY, toX, toY) => {
    const dot = fromX * toX + fromY * toY;
    const lengthProduct =
      Math.sqrt((fromX * fromX + fromY * fromY) * (toX * toX + toY * toY)) || 1;
    let angle = Math.acos(Math.max(-1, Math.min(1, dot / lengthProduct)));
    if (fromX * toY - fromY * toX < 0) angle = -angle;
    return angle;
  };
  const startAngle = angleBetween(
    1,
    0,
    (primeX - primeCenterX) / rx,
    (primeY - primeCenterY) / ry,
  );
  let sweepAngle = angleBetween(
    (primeX - primeCenterX) / rx,
    (primeY - primeCenterY) / ry,
    (-primeX - primeCenterX) / rx,
    (-primeY - primeCenterY) / ry,
  );
  if (!sweep && sweepAngle > 0) sweepAngle -= FULL_TURN_RADIANS;
  if (sweep && sweepAngle < 0) sweepAngle += FULL_TURN_RADIANS;
  const segmentCount = Math.ceil(Math.abs(sweepAngle) / QUARTER_TURN_RADIANS);
  const segmentAngle = sweepAngle / segmentCount;
  const handleScale = (4 / 3) * Math.tan(segmentAngle / 4);
  let segmentStart = startAngle;
  let previousX = x0;
  let previousY = y0;
  for (let segment = 0; segment < segmentCount; segment++) {
    const segmentEnd = segmentStart + segmentAngle;
    const cosStart = Math.cos(segmentStart);
    const sinStart = Math.sin(segmentStart);
    const cosEnd = Math.cos(segmentEnd);
    const sinEnd = Math.sin(segmentEnd);
    const startPointX =
      cx + rx * cosRotation * cosStart - ry * sinRotation * sinStart;
    const startPointY =
      cy + rx * sinRotation * cosStart + ry * cosRotation * sinStart;
    const endPointX =
      cx + rx * cosRotation * cosEnd - ry * sinRotation * sinEnd;
    const endPointY =
      cy + rx * sinRotation * cosEnd + ry * cosRotation * sinEnd;
    const startTangentX =
      -rx * cosRotation * sinStart - ry * sinRotation * cosStart;
    const startTangentY =
      -rx * sinRotation * sinStart + ry * cosRotation * cosStart;
    const endTangentX = -rx * cosRotation * sinEnd - ry * sinRotation * cosEnd;
    const endTangentY = -rx * sinRotation * sinEnd + ry * cosRotation * cosEnd;
    cubics.push([
      previousX + handleScale * startTangentX,
      previousY + handleScale * startTangentY,
      endPointX - handleScale * endTangentX,
      endPointY - handleScale * endTangentY,
      endPointX,
      endPointY,
    ]);
    previousX = endPointX;
    previousY = endPointY;
    segmentStart = segmentEnd;
  }
  return cubics;
}

/**
 * Parses an SVG `d` string into engine path ops (a flat number array, NO leading SHAPE op). Supports
 * M/L/H/V/C/S/Q/T/A/Z and their relative (lowercase) forms; arcs are converted to cubics.
 *
 * Results are cached by `d` string, so the returned array is SHARED — read it, never mutate it.
 */
export function parsePath(d) {
  return pathEntry(d).ops;
}

// Parsed `d` strings, keyed on the string itself. An <Svg> recompiles its whole tape whenever any
// part of it changes, so without this a moving needle re-runs the tokenizer and the arc->cubic
// conversion over every static path beside it, only for the engine to memcmp the result and throw
// the identical bytes away. Held to both an op budget and an entry count, and a path too big to fit
// the budget on its own is never stored, so nothing can push it over; an entry also holds one
// transformed copy (see lastTransform), so the real footprint is ~2x the op budget. The bridge caps a single
// <Svg> at 1024 ops and 16 shapes, so this holds several screens' worth.
const PATH_CACHE = new Map();
const PATH_CACHE_MAX_OPS = 4096;
const PATH_CACHE_MAX_ENTRIES = 128;
let _pathCacheOps = 0;

/** Cache record for one `d` string: `ops` are the parsed path ops, `lastTransform` the last transform applied
 *  to them (see transformCached). Coerces its key, so a non-string `d` still hits the cache instead
 *  of keying a fresh entry off object identity every call. */
function pathEntry(d) {
  const key = typeof d === 'string' ? d : String(d);
  const hit = PATH_CACHE.get(key);
  if (hit !== undefined) {
    // A Map iterates oldest-first, so re-inserting on a hit makes eviction LRU — which is what keeps
    // a static face cached next to a needle whose `d` is rebuilt every frame.
    PATH_CACHE.delete(key);
    PATH_CACHE.set(key, hit);
    return hit;
  }
  const entry = {ops: parsePathUncached(key), lastTransform: null};
  // A path that alone exceeds the whole budget is parsed but not stored: caching it would blow the
  // bound it is measured against, and at that size it is already past the bridge's per-<Svg> op cap.
  if (entry.ops.length > PATH_CACHE_MAX_OPS) return entry;
  PATH_CACHE.set(key, entry);
  _pathCacheOps += entry.ops.length;
  while (
    _pathCacheOps > PATH_CACHE_MAX_OPS ||
    PATH_CACHE.size > PATH_CACHE_MAX_ENTRIES
  ) {
    const oldest = PATH_CACHE.keys().next().value;
    _pathCacheOps -= PATH_CACHE.get(oldest).ops.length;
    PATH_CACHE.delete(oldest);
  }
  return entry;
}

/** Drops every cached path. Only for tests that measure parse work. */
export function clearPathCache() {
  PATH_CACHE.clear();
  _pathCacheOps = 0;
}

function parsePathUncached(d) {
  const ops = [];
  const tokens = tokenizePath(d);
  let currentX = 0;
  let currentY = 0;
  let subpathStartX = 0;
  let subpathStartY = 0; // subpath start
  let previousControlX = 0;
  let previousControlY = 0;
  let previousCommand = '';
  for (const token of tokens) {
    const command = token.command;
    const relative = command >= 'a' && command <= 'z';
    const values = token.values;
    const absoluteCommand = command.toUpperCase();
    let index = 0;
    const resolveX = value => (relative ? currentX + value : value);
    const resolveY = value => (relative ? currentY + value : value);
    if (absoluteCommand === 'M') {
      // First pair is moveto; later pairs are implicit linetos.
      currentX = resolveX(values[index++]);
      currentY = resolveY(values[index++]);
      subpathStartX = currentX;
      subpathStartY = currentY;
      ops.push(VOP_MOVE, currentX, currentY);
      while (index + 1 < values.length) {
        currentX = relative ? currentX + values[index++] : values[index++];
        currentY = relative ? currentY + values[index++] : values[index++];
        ops.push(VOP_LINE, currentX, currentY);
      }
    } else if (absoluteCommand === 'L') {
      while (index + 1 < values.length) {
        currentX = relative ? currentX + values[index++] : values[index++];
        currentY = relative ? currentY + values[index++] : values[index++];
        ops.push(VOP_LINE, currentX, currentY);
      }
    } else if (absoluteCommand === 'H') {
      while (index < values.length) {
        currentX = relative ? currentX + values[index++] : values[index++];
        ops.push(VOP_LINE, currentX, currentY);
      }
    } else if (absoluteCommand === 'V') {
      while (index < values.length) {
        currentY = relative ? currentY + values[index++] : values[index++];
        ops.push(VOP_LINE, currentX, currentY);
      }
    } else if (absoluteCommand === 'C') {
      while (index + 5 < values.length) {
        const control1X = resolveX(values[index++]);
        const control1Y = resolveY(values[index++]);
        const control2X = resolveX(values[index++]);
        const control2Y = resolveY(values[index++]);
        const endX = resolveX(values[index++]);
        const endY = resolveY(values[index++]);
        ops.push(
          VOP_CUBIC,
          control1X,
          control1Y,
          control2X,
          control2Y,
          endX,
          endY,
        );
        previousControlX = control2X;
        previousControlY = control2Y;
        currentX = endX;
        currentY = endY;
      }
    } else if (absoluteCommand === 'S') {
      while (index + 3 < values.length) {
        const reflected = previousCommand === 'C' || previousCommand === 'S';
        const control1X = reflected
          ? 2 * currentX - previousControlX
          : currentX;
        const control1Y = reflected
          ? 2 * currentY - previousControlY
          : currentY;
        const control2X = resolveX(values[index++]);
        const control2Y = resolveY(values[index++]);
        const endX = resolveX(values[index++]);
        const endY = resolveY(values[index++]);
        ops.push(
          VOP_CUBIC,
          control1X,
          control1Y,
          control2X,
          control2Y,
          endX,
          endY,
        );
        previousControlX = control2X;
        previousControlY = control2Y;
        currentX = endX;
        currentY = endY;
      }
    } else if (absoluteCommand === 'Q') {
      while (index + 3 < values.length) {
        const controlX = resolveX(values[index++]);
        const controlY = resolveY(values[index++]);
        const endX = resolveX(values[index++]);
        const endY = resolveY(values[index++]);
        ops.push(VOP_QUAD, controlX, controlY, endX, endY);
        previousControlX = controlX;
        previousControlY = controlY;
        currentX = endX;
        currentY = endY;
      }
    } else if (absoluteCommand === 'T') {
      while (index + 1 < values.length) {
        const reflected = previousCommand === 'Q' || previousCommand === 'T';
        const controlX = reflected ? 2 * currentX - previousControlX : currentX;
        const controlY = reflected ? 2 * currentY - previousControlY : currentY;
        const endX = resolveX(values[index++]);
        const endY = resolveY(values[index++]);
        ops.push(VOP_QUAD, controlX, controlY, endX, endY);
        previousControlX = controlX;
        previousControlY = controlY;
        currentX = endX;
        currentY = endY;
      }
    } else if (absoluteCommand === 'A') {
      while (index + 6 < values.length) {
        const radiusX = values[index++];
        const radiusY = values[index++];
        const rotation = values[index++];
        const largeArcFlag = values[index++];
        const sweepFlag = values[index++];
        const endX = resolveX(values[index++]);
        const endY = resolveY(values[index++]);
        const cubics = arcToCubics(
          currentX,
          currentY,
          radiusX,
          radiusY,
          rotation,
          largeArcFlag,
          sweepFlag,
          endX,
          endY,
        );
        for (const cubic of cubics)
          ops.push(
            VOP_CUBIC,
            cubic[0],
            cubic[1],
            cubic[2],
            cubic[3],
            cubic[4],
            cubic[5],
          );
        currentX = endX;
        currentY = endY;
      }
    } else if (absoluteCommand === 'Z') {
      ops.push(VOP_CLOSE);
      currentX = subpathStartX;
      currentY = subpathStartY;
    }
    previousCommand = absoluteCommand;
  }
  return ops;
}

// --- Primitive shapes -> path ops ----------------------------------------------------------------

function num(value, fallback) {
  const parsed = typeof value === 'number' ? value : parseFloat(value);
  return isNaN(parsed) ? fallback : parsed;
}

function circleOps(props) {
  const cx = num(props.cx, 0);
  const cy = num(props.cy, 0);
  const r = num(props.r, 0);
  // Start at angle 0, full circular arc.
  return [
    VOP_MOVE,
    cx + r,
    cy,
    VOP_ARC,
    cx,
    cy,
    r,
    0,
    FULL_TURN_RADIANS,
    0,
    VOP_CLOSE,
  ];
}

/** An ellipse as a `d` string: an arc-free path of four cubic Béziers (kappa) once parsePath lowers
 *  the two `A` commands. Returned as a string so it goes through the same parse cache as <Path d>. */
function ellipsePathD(props) {
  const cx = num(props.cx, 0);
  const cy = num(props.cy, 0);
  const rx = num(props.rx, 0);
  const ry = num(props.ry, 0);
  return `M ${cx - rx} ${cy} A ${rx} ${ry} 0 1 0 ${cx + rx} ${cy} A ${rx} ${ry} 0 1 0 ${cx - rx} ${cy} Z`;
}

/**
 * SVG <rect> corner-radius resolution. A missing radius is `auto` and falls back to the other one; a
 * NEGATIVE radius is invalid, which also means `auto`, so it falls back too — <rect rx="-3" ry="8">
 * rounds at 8, matching what browsers render. An explicit ZERO is a valid radius and squares the
 * corners, and each radius is clamped to half its own side. Returns [rx, ry] — [0, 0] for a
 * square-cornered rect. Exported so the AOT and the SVG baker apply the same rules.
 */
export function rectRadii(rxAttr, ryAttr, width, height) {
  let rx = num(rxAttr, -1);
  let ry = num(ryAttr, -1);
  if (rx < 0) rx = ry; // rx omitted -> ry
  if (ry < 0) ry = rx; // ry omitted -> rx
  rx = Math.min(rx, width / 2);
  ry = Math.min(ry, height / 2);
  return rx > 0 && ry > 0 ? [rx, ry] : [0, 0];
}

/**
 * Rect path ops, clockwise from the top-left corner. rx/ry must already be resolved by rectRadii();
 * 0 gives the plain four-line rectangle. Corners are cubics rather than VOP_ARC so each one STARTS at
 * exactly the preceding line's endpoint — an arc recomputes that point from cos/sin and can miss the
 * engine's exact-equality join dedupe by a ULP, which spikes the miter join on a stroked rect.
 */
function rectOpsXY(x, y, width, height, rx, ry) {
  if (rx <= 0 || ry <= 0)
    return [
      VOP_MOVE,
      x,
      y,
      VOP_LINE,
      x + width,
      y,
      VOP_LINE,
      x + width,
      y + height,
      VOP_LINE,
      x,
      y + height,
      VOP_CLOSE,
    ];
  const controlOffsetX = rx * KAPPA;
  const controlOffsetY = ry * KAPPA;
  const innerLeft = x + rx; // where the top/bottom edges start, past the left corners
  const innerRight = x + width - rx; // ... and end, before the right corners
  const innerTop = y + ry;
  const innerBottom = y + height - ry;
  const right = x + width;
  const bottom = y + height;
  return [
    VOP_MOVE,
    innerLeft,
    y,
    VOP_LINE,
    innerRight,
    y,
    VOP_CUBIC,
    innerRight + controlOffsetX,
    y,
    right,
    innerTop - controlOffsetY,
    right,
    innerTop,
    VOP_LINE,
    right,
    innerBottom,
    VOP_CUBIC,
    right,
    innerBottom + controlOffsetY,
    innerRight + controlOffsetX,
    bottom,
    innerRight,
    bottom,
    VOP_LINE,
    innerLeft,
    bottom,
    VOP_CUBIC,
    innerLeft - controlOffsetX,
    bottom,
    x,
    innerBottom + controlOffsetY,
    x,
    innerBottom,
    VOP_LINE,
    x,
    innerTop,
    VOP_CUBIC,
    x,
    innerTop - controlOffsetY,
    innerLeft - controlOffsetX,
    y,
    innerLeft,
    y,
    VOP_CLOSE,
  ];
}

function rectOps(props) {
  const x = num(props.x, 0);
  const y = num(props.y, 0);
  const width = num(props.width, 0);
  const height = num(props.height, 0);
  const [rx, ry] = rectRadii(props.rx, props.ry, width, height);
  return rectOpsXY(x, y, width, height, rx, ry);
}

function lineOps(props) {
  return [
    VOP_MOVE,
    num(props.x1, 0),
    num(props.y1, 0),
    VOP_LINE,
    num(props.x2, 0),
    num(props.y2, 0),
  ];
}

// Arc convenience: angles in DEGREES, clockwise from 12 o'clock (the gauge/dial convention). Emits a
// native VOP_ARC (no d-string regex / bezier conversion), so it is cheap enough to rebuild every drag
// frame — the key to fast imperative updates.
function arcOpsCW(cx, cy, r, startDegrees, endDegrees) {
  // The engine's arc angle runs from +X (cos/sin); top-clockwise => subtract 90°.
  const startRadians =
    ((startDegrees - TWELVE_OCLOCK_DEGREES) * Math.PI) / HALF_TURN_DEGREES;
  const endRadians =
    ((endDegrees - TWELVE_OCLOCK_DEGREES) * Math.PI) / HALF_TURN_DEGREES;
  return [VOP_ARC, cx, cy, r, startRadians, endRadians, 0];
}

// --- Imperative shapes -> op-tape (the fast path; no JSX, no React) -------------------------------

/**
 * Compiles a flat array of primitive shape descriptors into {ops, paints} for NativeUI.setVectorOps.
 * Each descriptor names one primitive plus paint fields, e.g.:
 *   { arc: [cx, cy, r, startDeg, endDeg], stroke: '#f4a261', strokeWidth: 14, cap: 'round' }
 *   { circle: [cx, cy, r], fill: '#16202f', stroke: '#f4a261', strokeWidth: 3 }
 *   { line: [x1, y1, x2, y2], stroke, strokeWidth }
 *   { rect: [x, y, width, height], fill }     // optional [x, y, width, height, rx, ry] for rounded corners
 *   { path: 'M..A..', stroke }   // d-string supported but slower (regex + bezier)
 * Arc angles are degrees, clockwise from 12 o'clock. This avoids the d-string parser entirely, so it's
 * cheap enough to call on every pointer move (the imperative drag path).
 */
// Reused output buffers — shapesToVector is on the drag hot path and its result is consumed
// synchronously by NativeUI.setVectorOps, so growing fresh arrays (plus per-shape arrays + spreads)
// every move is pure GC. Push directly into these instead.
const _vectorOps = [];
const _vectorPaints = [];
const _vectorGradients = [];

export function shapesToVector(shapes) {
  const ops = _vectorOps;
  const paints = _vectorPaints;
  const gradients = _vectorGradients;
  ops.length = 0;
  paints.length = 0;
  gradients.length = 0;
  for (let shapeIndex = 0; shapeIndex < shapes.length; shapeIndex++) {
    const shape = shapes[shapeIndex];
    const opStart = ops.length;
    const paintIndex = paints.length / PAINT_STRIDE;
    ops.push(VOP_SHAPE, paintIndex);
    if (shape.arc) {
      const cx = shape.arc[0];
      const cy = shape.arc[1];
      const r = shape.arc[2];
      const startRadians =
        ((shape.arc[3] - TWELVE_OCLOCK_DEGREES) * Math.PI) / HALF_TURN_DEGREES;
      const endRadians =
        ((shape.arc[4] - TWELVE_OCLOCK_DEGREES) * Math.PI) / HALF_TURN_DEGREES;
      ops.push(VOP_ARC, cx, cy, r, startRadians, endRadians, 0);
    } else if (shape.circle) {
      const cx = shape.circle[0];
      const cy = shape.circle[1];
      const r = shape.circle[2];
      ops.push(
        VOP_MOVE,
        cx + r,
        cy,
        VOP_ARC,
        cx,
        cy,
        r,
        0,
        FULL_TURN_RADIANS,
        0,
        VOP_CLOSE,
      );
    } else if (shape.rect) {
      const x = shape.rect[0];
      const y = shape.rect[1];
      const width = shape.rect[2];
      const height = shape.rect[3];
      if (shape.rect.length > 4) {
        // Rounded — the optional trailing [rx, ry]. Off the allocation-free path below, but a rounded
        // rect is a chrome/backing shape, not something a drag rebuilds every move.
        const [rx, ry] = rectRadii(shape.rect[4], shape.rect[5], width, height);
        const roundedOps = rectOpsXY(x, y, width, height, rx, ry);
        for (let index = 0; index < roundedOps.length; index++)
          ops.push(roundedOps[index]);
      } else {
        ops.push(
          VOP_MOVE,
          x,
          y,
          VOP_LINE,
          x + width,
          y,
          VOP_LINE,
          x + width,
          y + height,
          VOP_LINE,
          x,
          y + height,
          VOP_CLOSE,
        );
      }
    } else if (shape.line) {
      ops.push(
        VOP_MOVE,
        shape.line[0],
        shape.line[1],
        VOP_LINE,
        shape.line[2],
        shape.line[3],
      );
    } else if (shape.path) {
      const pathOps = parsePath(shape.path);
      for (let index = 0; index < pathOps.length; index++)
        ops.push(pathOps[index]);
    }
    if (ops.length <= opStart + SHAPE_HEADER_LENGTH) {
      ops.length = opStart; // nothing emitted — roll back the SHAPE header
      continue;
    }
    // Optional gradients. `paint.fill_gradient` / `stroke_gradient` are 1-BASED indices into the gradient table
    // (0 = solid), which is what lets one shape carry a smooth ramp instead of being chopped into many
    // solid pieces. Stop colors may be strings here; the engine wants packed ARGB.
    paints.push(
      parseColor(shape.fill),
      parseColor(shape.stroke),
      num(shape.strokeWidth, DEFAULT_STROKE_WIDTH),
      num(shape.miter, DEFAULT_MITER_LIMIT),
      CAP[shape.cap] ?? CAP.butt,
      JOIN[shape.join] ?? JOIN.miter,
      shape.fillRule === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
      shape.fillGradient ? pushGrad(gradients, shape.fillGradient) : 0,
      shape.strokeGradient ? pushGrad(gradients, shape.strokeGradient) : 0,
    );
  }
  return {ops, paints, gradients};
}

/** Appends a gradient descriptor (normalizing string stop colours) and returns its 1-based index. */
function pushGrad(gradients, gradient) {
  gradients.push({
    type: gradient.type,
    ax: gradient.ax,
    ay: gradient.ay,
    bx: gradient.bx,
    by: gradient.by,
    r: gradient.r,
    stops: (gradient.stops || []).map(stop => ({
      color:
        typeof stop.color === 'string' ? parseColor(stop.color) : stop.color,
      offset: stop.offset,
    })),
  });
  return gradients.length;
}

// --- Flatten an <Svg> subtree --------------------------------------------------------------------

const PAINT_DEFAULT = {
  fill: 'black',
  stroke: 'none',
  strokeWidth: DEFAULT_STROKE_WIDTH,
  strokeLinecap: 'butt',
  strokeLinejoin: 'miter',
  strokeMiterlimit: DEFAULT_MITER_LIMIT,
  fillRule: 'nonzero',
  fillGradient: null,
  strokeGradient: null,
};

/** The props that make up a shape's paint; a <G> or the <Svg> itself passes them down. */
export const PAINT_PROPS = Object.keys(PAINT_DEFAULT);

function mergePaint(base, props) {
  const merged = {...base};
  for (const key of PAINT_PROPS)
    if (props[key] != null) merged[key] = props[key];
  return merged;
}

/** Maps the resolved paint to the PAINT_STRIDE-number record [fill, stroke, strokeWidth, miter, cap, join, rule,
 *  fillGradient, strokeGradient]. Any gradient descriptor is appended to `gradients` and referenced by its
 *  1-BASED index (0 = solid), the same encoding shapesToVector uses. */
function paintRecord(paint, scale, gradients) {
  return [
    parseColor(paint.fill),
    parseColor(paint.stroke),
    num(paint.strokeWidth, DEFAULT_STROKE_WIDTH) * scale,
    num(paint.strokeMiterlimit, DEFAULT_MITER_LIMIT),
    CAP[paint.strokeLinecap] ?? CAP.butt,
    JOIN[paint.strokeLinejoin] ?? JOIN.miter,
    paint.fillRule === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
    paint.fillGradient && gradients
      ? pushGrad(gradients, paint.fillGradient)
      : 0,
    paint.strokeGradient && gradients
      ? pushGrad(gradients, paint.strokeGradient)
      : 0,
  ];
}

/** Applies a {scaleX, scaleY, translateX, translateY} transform to a path-op array (no leading SHAPE op). */
function transformOps(ops, transform) {
  const transformed = [];
  let index = 0;
  const mapX = coordinate =>
    coordinate * transform.scaleX + transform.translateX;
  const mapY = coordinate =>
    coordinate * transform.scaleY + transform.translateY;
  while (index < ops.length) {
    const opcode = ops[index++];
    transformed.push(opcode);
    if (opcode === VOP_MOVE || opcode === VOP_LINE) {
      transformed.push(mapX(ops[index++]), mapY(ops[index++]));
    } else if (opcode === VOP_QUAD) {
      transformed.push(
        mapX(ops[index++]),
        mapY(ops[index++]),
        mapX(ops[index++]),
        mapY(ops[index++]),
      );
    } else if (opcode === VOP_CUBIC) {
      transformed.push(
        mapX(ops[index++]),
        mapY(ops[index++]),
        mapX(ops[index++]),
        mapY(ops[index++]),
        mapX(ops[index++]),
        mapY(ops[index++]),
      );
    } else if (opcode === VOP_ARC) {
      // center + radius scale (uniform scale assumed for arcs)
      transformed.push(
        mapX(ops[index++]),
        mapY(ops[index++]),
        ops[index++] * transform.scaleX,
        ops[index++],
        ops[index++],
        ops[index++],
      );
    }
    // VOP_CLOSE has no args
  }
  return transformed;
}

/**
 * transformOps against a cached path, reusing the last result when the transform is unchanged. One
 * slot per path: a shape's transform is the root viewBox scale composed with its <G>s, which almost
 * never moves between commits, so an untouched shape costs a compare instead of a rebuild. The same
 * `d` drawn under several different transforms just misses and pays the full transform.
 */
function transformCached(entry, transform) {
  const lastTransform = entry.lastTransform;
  if (
    lastTransform &&
    lastTransform.scaleX === transform.scaleX &&
    lastTransform.scaleY === transform.scaleY &&
    lastTransform.translateX === transform.translateX &&
    lastTransform.translateY === transform.translateY
  )
    return lastTransform.ops;
  const transformedOps = transformOps(entry.ops, transform);
  entry.lastTransform = {
    scaleX: transform.scaleX,
    scaleY: transform.scaleY,
    translateX: transform.translateX,
    translateY: transform.translateY,
    ops: transformedOps,
  };
  return transformedOps;
}

function isElement(child) {
  return (
    child &&
    typeof child === 'object' &&
    child.type != null &&
    child.props != null
  );
}

// React's fragment tag. Read from the global symbol registry rather than importing React, which this
// module deliberately does not depend on; `Symbol.for` returns the very symbol React itself uses.
const REACT_FRAGMENT = Symbol.for('react.fragment');

/** Children JSX produces for "draw nothing": a bare `{cond && <Path/>}` that failed, a hole in a
 *  list, and the whitespace JSX leaves between elements. Skipped without a word. */
function isEmptyChild(child) {
  return (
    child == null ||
    typeof child === 'boolean' ||
    (typeof child === 'string' && child.trim() === '')
  );
}

// Shape/grouping tags flattenSvg knows. A tag outside this set is a mistake worth reporting; a known
// tag that produces no geometry (a <Path> with no `d`, a zero-radius <Circle>) is not.
const SVG_TAGS = ['Path', 'Circle', 'Ellipse', 'Rect', 'Line', 'Arc', 'G'];

let _warnedSvgChild = false;

/**
 * Warns ONCE about a child <Svg> cannot draw. The two common cases are a component (`<Needle/>` —
 * <Svg> owns its subtree, so React never renders it) and a non-shape host tag; both used to vanish
 * with no output at all. Once per app, like the other commit-path warnings: an <Svg> recompiles its
 * tape on every update, and one bad child would otherwise print on every frame.
 *
 * The way out of the component case is to CALL it — `{needle()}` puts whatever it returns (an element
 * or an array of them) straight into children, where the walk sees it. Having it return an array does
 * nothing for `<Needle/>`: that form is never invoked at all.
 */
function warnSvgChild(child) {
  if (_warnedSvgChild) return;
  _warnedSvgChild = true;
  const description = !isElement(child)
    ? typeof child === 'string'
      ? `the text ${JSON.stringify(child)}`
      : `a ${typeof child} child`
    : typeof child.type === 'function'
      ? `<${child.type.displayName || child.type.name || 'Component'}>, a component`
      : `<${String(child.type)}>`;
  console.warn(
    `embedded-react: <Svg> can't draw ${description}, and skipped it. Children must be ${SVG_TAGS.join('/')} ` +
      `elements (arrays and fragments around them are unwrapped). <Svg> flattens its subtree instead of ` +
      `mounting it, so a component is never rendered — inline its shapes, or call it as a plain ` +
      `function: {needle()}.`,
  );
}

/**
 * Flattens an <Svg> element's props into {ops, paints, gradients} flat arrays for NativeUI.setVectorOps.
 * Handles Path/Circle/Ellipse/Rect/Line shapes, <G> grouping (inherited paint + translate/scale), and
 * a root viewBox -> width/height scale. Arrays and fragments are unwrapped at any depth, so a mapped
 * list of shapes draws like an inline one. A shape's fillGradient/strokeGradient descriptor lands in
 * `gradients` and is referenced 1-based from its paint record. Returns empty arrays when there is
 * nothing to draw.
 */
export function flattenSvg(props) {
  const ops = [];
  const paints = [];
  const gradients = [];

  // Root transform from viewBox vs width/height.
  let root = {scaleX: 1, scaleY: 1, translateX: 0, translateY: 0};
  if (props.viewBox && props.width && props.height) {
    const viewBox = String(props.viewBox)
      .trim()
      .split(/[\s,]+/)
      .map(parseFloat);
    if (viewBox.length === 4 && viewBox[2] > 0 && viewBox[3] > 0) {
      const scaleX = num(props.width, viewBox[2]) / viewBox[2];
      const scaleY = num(props.height, viewBox[3]) / viewBox[3];
      root = {
        scaleX: scaleX,
        scaleY: scaleY,
        translateX: -viewBox[0] * scaleX,
        translateY: -viewBox[1] * scaleY,
      };
    }
  }

  // One child at a time, recursing through the wrappers, JSX inserts around shapes: an array (every
  // `{shapes.map(...)}`, and arrays of arrays when those nest) and a fragment, either of which can
  // sit anywhere a shape can. Both are transparent — they carry no paint and no transform.
  const walk = (child, paint, transform) => {
    if (isEmptyChild(child)) return;
    if (Array.isArray(child)) {
      for (const item of child) walk(item, paint, transform);
      return;
    }
    if (!isElement(child)) return warnSvgChild(child);
    const childProps = child.props || {};
    if (child.type === REACT_FRAGMENT)
      return walk(childProps.children, paint, transform);
    const merged = mergePaint(paint, childProps);
    if (child.type === 'G') {
      // Compose a translate/scale for the group, then recurse.
      const groupScale = num(childProps.scale, 1);
      const groupX = num(childProps.x ?? childProps.translateX, 0);
      const groupY = num(childProps.y ?? childProps.translateY, 0);
      const childTransform = {
        scaleX: transform.scaleX * groupScale,
        scaleY: transform.scaleY * groupScale,
        translateX: groupX * transform.scaleX + transform.translateX,
        translateY: groupY * transform.scaleY + transform.translateY,
      };
      return walk(childProps.children, merged, childTransform);
    }
    if (!SVG_TAGS.includes(child.type)) return warnSvgChild(child);

    // Path/Ellipse geometry comes from a `d` string and is cached (parse + transform) by that string;
    // the primitives build a fixed handful of numbers, which is cheaper than any cache lookup.
    let entry = null;
    let shapeOps = null;
    if (child.type === 'Path' && childProps.d)
      entry = pathEntry(childProps.d); // pathEntry coerces the key
    else if (child.type === 'Ellipse')
      entry = pathEntry(ellipsePathD(childProps));
    else if (child.type === 'Circle') shapeOps = circleOps(childProps);
    else if (child.type === 'Rect') shapeOps = rectOps(childProps);
    else if (child.type === 'Line') shapeOps = lineOps(childProps);
    else if (child.type === 'Arc')
      shapeOps = arcOpsCW(
        num(childProps.cx, 0),
        num(childProps.cy, 0),
        num(childProps.r, 0),
        num(childProps.startAngle, 0),
        num(childProps.endAngle, 0),
      );
    const baseOps = entry ? entry.ops : shapeOps;
    if (!baseOps || baseOps.length === 0) return;

    const shapeTape = entry
      ? transformCached(entry, transform)
      : transformOps(baseOps, transform);
    const paintIndex = paints.length / PAINT_STRIDE;
    const scale = (transform.scaleX + transform.scaleY) / 2; // stroke-width scale (uniform assumed)
    paints.push(...paintRecord(merged, scale, gradients));
    // Appended one at a time: `shapeTape` can be a long cached tape, and spreading it into push() both
    // copies it into an argument list and risks the call-argument limit.
    ops.push(VOP_SHAPE, paintIndex);
    for (let index = 0; index < shapeTape.length; index++)
      ops.push(shapeTape[index]);
  };

  // The <Svg>'s own paint cascades to every shape, like a <G> wrapping them all.
  walk(props.children, mergePaint(PAINT_DEFAULT, props), root);
  return {ops, paints, gradients};
}

// --- Imported SVG artifacts (<Svg source>) -------------------------------------------------------

/** Scales a full op-tape (with SHAPE headers) by (scaleX, scaleY): coordinates only, paint indices untouched. */
function scaleTape(ops, scaleX, scaleY) {
  const scaled = [];
  let index = 0;
  while (index < ops.length) {
    const opcode = ops[index++];
    scaled.push(opcode);
    if (opcode === VOP_SHAPE)
      scaled.push(ops[index++]); // paint index — copy, do not scale
    else if (opcode === VOP_MOVE || opcode === VOP_LINE)
      scaled.push(ops[index++] * scaleX, ops[index++] * scaleY);
    else if (opcode === VOP_QUAD)
      scaled.push(
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++] * scaleY,
      );
    else if (opcode === VOP_CUBIC)
      scaled.push(
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++] * scaleY,
      );
    else if (opcode === VOP_ARC)
      scaled.push(
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++],
        ops[index++],
        ops[index++],
      );
    // VOP_CLOSE has no args.
  }
  return scaled;
}

/**
 * Scales an imported vector artifact ({ops, paints, width, height} from the .svg baker) from its intrinsic
 * size to a target box, also scaling stroke widths. Returns the original arrays untouched when no scaling
 * is needed (the common <Svg source> case where the box equals the intrinsic size).
 */
export function scaleVectorArtifact(artifact, targetWidth, targetHeight) {
  const scaleX = artifact.width ? targetWidth / artifact.width : 1;
  const scaleY = artifact.height ? targetHeight / artifact.height : 1;
  const sourceGradients = artifact.gradients || [];
  if (scaleX === 1 && scaleY === 1)
    return {
      ops: artifact.ops,
      paints: artifact.paints,
      gradients: sourceGradients,
    };
  const strokeScale = Math.sqrt(Math.abs(scaleX * scaleY)) || 1; // uniform stroke-width / radius scale
  const paints = artifact.paints.slice();
  for (
    let index = PAINT_STROKE_WIDTH_SLOT;
    index < paints.length;
    index += PAINT_STRIDE
  )
    paints[index] *= strokeScale;
  // Gradient geometry lives in the same coordinate space as the path, so it scales with it (radius uniformly).
  const gradients = sourceGradients.map(gradient => ({
    ...gradient,
    ax: (gradient.ax || 0) * scaleX,
    ay: (gradient.ay || 0) * scaleY,
    bx: (gradient.bx || 0) * scaleX,
    by: (gradient.by || 0) * scaleY,
    // Radial r is a length (scales); conic r is a start angle (invariant under uniform scale) — don't scale it.
    r:
      gradient.type === GRAD_CONIC
        ? gradient.r || 0
        : (gradient.r || 0) * strokeScale,
  }));
  return {ops: scaleTape(artifact.ops, scaleX, scaleY), paints, gradients};
}

// --- Bridge-cap diagnostics ----------------------------------------------------------------------

let _warnedVectorOps = false;
let _warnedVectorPaints = false;
let _warnedVectorGradients = false;

/**
 * Warns (once per kind) when an <Svg>'s compiled geometry exceeds the native bridge's caps and will be
 * SILENTLY TRUNCATED. The bridge (native_ui_bridge.c) copies the op-tape into a fixed buffer and caps the
 * paint table; past the cap, geometry/shapes are dropped. The engine has its own pool diagnostics (on
 * stderr, debug builds) for what happens further in; this surfaces the JS->engine boundary truncation in
 * the developer's console. Pass the live caps from NativeUI.maxVectorOps / maxVectorPaints / maxVectorGrads
 * (undefined on an older bridge => no-op).
 *
 * @param {number} opsLength         Op-tape length (flat float count).
 * @param {number} paintsLength      Paint-table length (PAINT_STRIDE numbers per shape).
 * @param {number} maxOps            Bridge op-tape cap (NativeUI.maxVectorOps).
 * @param {number} maxPaints         Bridge paint cap, in SHAPES (NativeUI.maxVectorPaints).
 * @param {number} [gradientsLength] Gradient count (number of gradients referenced by the shapes).
 * @param {number} [maxGradients]    Bridge gradient cap (NativeUI.maxVectorGrads).
 */
export function warnVectorCaps(
  opsLength,
  paintsLength,
  maxOps,
  maxPaints,
  gradientsLength,
  maxGradients,
) {
  if (!_warnedVectorOps && maxOps > 0 && opsLength > maxOps) {
    _warnedVectorOps = true;
    console.warn(
      `embedded-react: an <Svg> op-tape is too long (${opsLength} > ${maxOps} floats) and will be truncated — ` +
        `the shape gets cut off. Simplify the path (fewer/coarser curves) or split it across <Svg> nodes; ` +
        `raising the limit needs ERUI_VECTOR_TAPE_MAX.`,
    );
  }
  const shapes = (paintsLength / PAINT_STRIDE) | 0;
  if (!_warnedVectorPaints && maxPaints > 0 && shapes > maxPaints) {
    _warnedVectorPaints = true;
    console.warn(
      `embedded-react: an <Svg> has ${shapes} shapes (> ${maxPaints}) — the extra shapes won't render. ` +
        `Split them across multiple <Svg> nodes; raising the limit needs ERUI_VECTOR_PAINTS_MAX.`,
    );
  }
  if (
    !_warnedVectorGradients &&
    maxGradients > 0 &&
    gradientsLength > maxGradients
  ) {
    _warnedVectorGradients = true;
    console.warn(
      `embedded-react: an <Svg> references ${gradientsLength} gradients (> ${maxGradients}) — the extra gradients are ` +
        `dropped and shapes that use them fall back to solid fills/strokes. Reuse fewer distinct gradients across ` +
        `shapes; raising the limit needs ERUI_VECTOR_GRADIENTS_MAX.`,
    );
  }
}
