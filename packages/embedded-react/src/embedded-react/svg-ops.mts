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

import {NAMED_COLORS} from './named-colors.mts';
import type {
  ArcProps,
  CircleProps,
  ClipPathProps,
  EllipseProps,
  GProps,
  LinearGradientProps,
  LineProps,
  MaskProps,
  PathProps,
  PolygonProps,
  RadialGradientProps,
  RectProps,
  StopProps,
  SvgPaintProps,
  SvgProps,
  VectorGradient,
  VectorShape,
} from './index.d.ts';

/** A gradient stop with its color packed to ARGB8888. */
export interface PackedStop {
  color: number;
  offset: number;
}

/** A gradient as the bridge takes it, its geometry in the op-tape's coordinates. */
export interface PackedGradient {
  type: number;
  stops: PackedStop[];
  ax?: number;
  ay?: number;
  bx?: number;
  by?: number;
  r?: number;
}

/** What NativeUI.setVectorOps takes: the op-tape, the paint table and the gradient table. */
export interface VectorTape {
  ops: number[];
  paints: number[];
  gradients: PackedGradient[];
}

/** A baked .svg: a tape at its intrinsic size. */
export interface VectorArtifact {
  ops: number[];
  paints: number[];
  gradients?: PackedGradient[];
  width: number;
  height: number;
}

/** A color as a prop gives it: a CSS string, a packed ARGB8888, or nothing. */
export type ColorInput = string | number | null | undefined | false;

/** Scale then translate, per axis. */
export interface Transform {
  scaleX: number;
  scaleY: number;
  translateX: number;
  translateY: number;
}

/** One command letter of a `d` string and the numbers after it. */
interface PathToken {
  command: string;
  values: number[];
}

/** A parsed `d` string, and its ops under the last transform applied to them. */
interface PathEntry {
  ops: number[];
  lastTransform: (Transform & {ops: number[]}) | null;
}

type PaintProp = keyof SvgPaintProps;

/** An animated value, as far as a tape reads one: the host config binds it to the node. */
interface AnimatedOffset {
  __animated: boolean;
}

/** A shape's paint with every prop resolved. */
type ResolvedPaint = Required<
  Omit<SvgPaintProps, 'fillGradient' | 'strokeGradient'>
> & {
  fillGradient: VectorGradient | null;
  strokeGradient: VectorGradient | null;
};

/** Every prop a shape or <G> inside an <Svg> can carry. */
type ShapeProps = GProps &
  PathProps &
  CircleProps &
  EllipseProps &
  RectProps &
  LineProps &
  PolygonProps &
  ArcProps &
  ClipPathProps &
  MaskProps &
  LinearGradientProps &
  RadialGradientProps &
  StopProps;

/** A fill or stroke as a paint record holds it: a color, and a gradient drawn instead of it, or null. */
export interface PaintSource {
  color: number;
  gradient: PackedGradient | null;
}

/** A clip or mask as tape values: its op, then the shapes it takes. */
export interface MaskGroup {
  ops: number[];
  shapes: number;
}

/** A box in the tape's coordinates. */
export interface TapeBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Where a walk of an <Svg> subtree writes, and what it writes its shapes as. */
interface FlattenTarget {
  ops: number[];
  paints: number[];
  gradients: PackedGradient[];
  /** `draw` paints shapes, `clip` and `mask` collect a definition's, `bounds` only measures. */
  mode: 'draw' | 'clip' | 'mask' | 'bounds';
  writer: MaskWriter | null;
}

/** A component function, as a React element's type. */
type ComponentType = ((...parameters: never[]) => unknown) & {
  displayName?: string;
};

/** A React element, as far as flattenSvg reads one. */
interface SvgElement {
  type: string | symbol | ComponentType;
  props: ShapeProps;
}

// Op-tape vocabulary — exported so the build-time SVG baker (assets/bake-svg.mts) can reuse it without
// duplicating the encoding (and without pulling its SVG-parser dependency into this runtime module).
export const VOP_SHAPE = 0;
export const VOP_MOVE = 1;
export const VOP_LINE = 2;
export const VOP_QUAD = 3;
export const VOP_CUBIC = 4;
export const VOP_ARC = 5;
export const VOP_CLOSE = 6;
export const VOP_MASK = 7;
export const VOP_DASH = 8;
export const VOP_DASH_OFFSET = 9;
export const VOP_IMAGE = 10;

// What an image in a tape is registered under, before its id in six hex digits — MUST match the engine's
// ER_VECTOR_IMAGE_PREFIX and ER_VECTOR_IMAGE_ID_MAX (er_scene.h).
export const IMAGE_PREFIX = 'svg-image-';
export const IMAGE_ID_MAX = 0xffffff;

// Mask kinds and limits — MUST match the engine's ER_VECTOR_MASK_* (er_scene.h).
export const MASK_NONE = 0;
export const MASK_CLIP = 1;
export const MASK_LUMINANCE = 2;
export const MASK_ALPHA = 3;
export const MASK_GROUPS_MAX = 4;
export const MASK_SHAPES_MAX = 16;

// Dash and gap pairs in one shape's pattern — MUST match the engine's ER_VECTOR_DASH_MAX (er_scene.h).
export const DASH_MAX = 8;

/** How many values follow each opcode. */
export const OP_OPERANDS = [1, 2, 2, 4, 6, 6, 0, 2, 2, 2, 8];

// Control-point ratio for approximating a quarter (circular or elliptical) arc with one cubic:
// 4/3 * (sqrt(2) - 1). Used by rounded-rect corners.
export const KAPPA = 0.5522847498307936;

export const CAP: Record<string, number> = {butt: 0, round: 1, square: 2};
export const JOIN: Record<string, number> = {miter: 0, round: 1, bevel: 2};

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

// Where the stroke's color, width and gradient sit in a paint record (see PAINT_STRIDE).
const PAINT_STROKE_SLOT = 1;
const PAINT_STROKE_WIDTH_SLOT = 2;
const PAINT_STROKE_GRADIENT_SLOT = 8;
const ALPHA_SHIFT = 24;

/** Linearly interpolates two straight-alpha ARGB8888 colors, per channel. */
function lerpArgb(
  fromColor: number,
  toColor: number,
  fraction: number,
): number {
  const channel = (shift: number) => {
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

/**
 * The solid color a gradient paint carries beside it: its middle color. The engine draws it only when it drops
 * the gradient, as a board built without radial gradients does with a radial one.
 */
export function gradientFallback(stops: readonly PackedStop[]): number {
  return evalStopsAt(stops as PackedStop[], 0.5);
}

/** Gradient color at `position` (stops ascending by offset; clamps to the endpoint colors). */
function evalStopsAt(stops: PackedStop[], position: number): number {
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
function capStops(stops: PackedStop[], maxStops: number): PackedStop[] {
  if (stops.length <= maxStops) return stops;
  const firstOffset = stops[0].offset;
  const lastOffset = stops[stops.length - 1].offset;
  const resampled: PackedStop[] = [];
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
export function encodeVectorGradients(
  gradients: readonly PackedGradient[] | null | undefined,
): number[] {
  const encoded: number[] = [];
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
const SVG_NAMED_COLORS: Readonly<Record<string, number>> = Object.freeze(
  Object.assign(Object.create(null), NAMED_COLORS, {none: 0}),
);

// Cache string->uint32 results: a theme reuses a handful of color strings, so on a continuous drag
// (which recompiles the dial every move) this turns repeated hex parsing into a Map lookup.
const _colorCache = new Map<string, number>();

/**
 * Parses a CSS color string to a uint32 ARGB8888. Returns 0 (fully transparent) for none/transparent/
 * null, so the engine treats it as "no fill"/"no stroke". Accepts #rgb, #rgba, #rrggbb, #rrggbbaa,
 * rgb()/rgba(), and the named colors above. String results are memoised (see _colorCache).
 */
export function parseColor(color: ColorInput): number {
  if (color == null || color === false) return 0;
  if (typeof color === 'number') return color >>> 0;
  const cached = _colorCache.get(color);
  if (cached !== undefined) return cached;
  const parsed = parseColorString(color);
  _colorCache.set(color, parsed);
  return parsed;
}

function parseColorString(color: string): number {
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
function tokenizePath(d: string): PathToken[] {
  const commands: PathToken[] = [];
  const pattern = PATH_RE;
  pattern.lastIndex = 0;
  let match;
  let current: PathToken | null = null;
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
function arcToCubics(
  x0: number,
  y0: number,
  rx: number,
  ry: number,
  rotationDegrees: number,
  largeArc: number,
  sweep: number,
  x: number,
  y: number,
): number[][] {
  const cubics: number[][] = [];
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
  const angleBetween = (
    fromX: number,
    fromY: number,
    toX: number,
    toY: number,
  ) => {
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
export function parsePath(d: string): number[] {
  return pathEntry(d).ops;
}

// Parsed `d` strings, keyed on the string itself. An <Svg> recompiles its whole tape whenever any
// part of it changes, so without this a moving needle re-runs the tokenizer and the arc->cubic
// conversion over every static path beside it, only for the engine to memcmp the result and throw
// the identical bytes away. Held to both an op budget and an entry count, and a path too big to fit
// the budget on its own is never stored, so nothing can push it over; an entry also holds one
// transformed copy (see lastTransform), so the real footprint is ~2x the op budget. The bridge caps a single
// <Svg> at 1024 ops and 16 shapes, so this holds several screens' worth.
const PATH_CACHE = new Map<string, PathEntry>();
const PATH_CACHE_MAX_OPS = 4096;
const PATH_CACHE_MAX_ENTRIES = 128;
let _pathCacheOps = 0;

/** Cache record for one `d` string: `ops` are the parsed path ops, `lastTransform` the last transform applied
 *  to them (see transformCached). Coerces its key, so a non-string `d` still hits the cache instead
 *  of keying a fresh entry off object identity every call. */
function pathEntry(d: unknown): PathEntry {
  const key = typeof d === 'string' ? d : String(d);
  const hit = PATH_CACHE.get(key);
  if (hit !== undefined) {
    // A Map iterates oldest-first, so re-inserting on a hit makes eviction LRU — which is what keeps
    // a static face cached next to a needle whose `d` is rebuilt every frame.
    PATH_CACHE.delete(key);
    PATH_CACHE.set(key, hit);
    return hit;
  }
  const entry: PathEntry = {ops: parsePathUncached(key), lastTransform: null};
  // A path that alone exceeds the whole budget is parsed but not stored: caching it would blow the
  // bound it is measured against, and at that size it is already past the bridge's per-<Svg> op cap.
  if (entry.ops.length > PATH_CACHE_MAX_OPS) return entry;
  PATH_CACHE.set(key, entry);
  _pathCacheOps += entry.ops.length;
  while (
    _pathCacheOps > PATH_CACHE_MAX_OPS ||
    PATH_CACHE.size > PATH_CACHE_MAX_ENTRIES
  ) {
    const oldest = PATH_CACHE.keys().next().value!;
    _pathCacheOps -= PATH_CACHE.get(oldest)!.ops.length;
    PATH_CACHE.delete(oldest);
  }
  return entry;
}

/** Drops every cached path. Only for tests that measure parse work. */
export function clearPathCache(): void {
  PATH_CACHE.clear();
  _pathCacheOps = 0;
}

function parsePathUncached(d: string): number[] {
  const ops: number[] = [];
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
    const resolveX = (value: number) => (relative ? currentX + value : value);
    const resolveY = (value: number) => (relative ? currentY + value : value);
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

function num(value: unknown, fallback: number): number {
  const parsed =
    typeof value === 'number' ? value : parseFloat(value as string);
  return isNaN(parsed) ? fallback : parsed;
}

function circleOps(props: CircleProps): number[] {
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
function ellipsePathD(props: EllipseProps): string {
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
export function rectRadii(
  rxAttr: unknown,
  ryAttr: unknown,
  width: number,
  height: number,
): [number, number] {
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
function rectOpsXY(
  x: number,
  y: number,
  width: number,
  height: number,
  rx: number,
  ry: number,
): number[] {
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

function rectOps(props: RectProps): number[] {
  const x = num(props.x, 0);
  const y = num(props.y, 0);
  const width = num(props.width, 0);
  const height = num(props.height, 0);
  const [rx, ry] = rectRadii(props.rx, props.ry, width, height);
  return rectOpsXY(x, y, width, height, rx, ry);
}

/** Points, as a string or a list of numbers, as a `d` string: open, or closed for a polygon. Fewer than two
 *  points draw nothing. Returned as a string so it goes through the same parse cache as <Path d>. */
export function polyPathD(points: unknown, close: boolean): string {
  const values = (
    Array.isArray(points)
      ? points
      : String(points ?? '')
          .trim()
          .split(/[\s,]+/)
  )
    .map(value => (typeof value === 'number' ? value : parseFloat(value)))
    .filter(value => Number.isFinite(value));
  if (values.length < 4) return '';
  let d = `M${values[0]} ${values[1]}`;
  for (let index = 2; index + 1 < values.length; index += 2)
    d += `L${values[index]} ${values[index + 1]}`;
  return close ? `${d}Z` : d;
}

function lineOps(props: LineProps): number[] {
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
function arcOpsCW(
  cx: number,
  cy: number,
  r: number,
  startDegrees: number,
  endDegrees: number,
): number[] {
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
const _vectorOps: number[] = [];
const _vectorPaints: number[] = [];
const _vectorGradients: PackedGradient[] = [];

export function shapesToVector(shapes: readonly VectorShape[]): VectorTape {
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
    if (shape.dash) pushDash(ops, shape.dash, shape.dashOffset, 1);
    const headerEnd = ops.length;
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
    if (ops.length <= headerEnd) {
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
      CAP[shape.cap!] ?? CAP.butt,
      JOIN[shape.join!] ?? JOIN.miter,
      shape.fillRule === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
      shape.fillGradient ? pushGrad(gradients, shape.fillGradient) : 0,
      shape.strokeGradient ? pushGrad(gradients, shape.strokeGradient) : 0,
    );
  }
  return {ops, paints, gradients};
}

/** Appends a gradient descriptor (normalizing string stop colours) and returns its 1-based index. */
function pushGrad(
  gradients: PackedGradient[],
  gradient: VectorGradient,
): number {
  gradients.push(packGradient(gradient));
  return gradients.length;
}

/** A gradient descriptor with its stop colors packed, and faded by `opacity`. */
function packGradient(gradient: VectorGradient, opacity = 1): PackedGradient {
  return {
    type: gradient.type,
    ax: gradient.ax,
    ay: gradient.ay,
    bx: gradient.bx,
    by: gradient.by,
    r: gradient.r,
    stops: (gradient.stops || []).map(stop => ({
      color: fadeColor(
        typeof stop.color === 'string' ? parseColor(stop.color) : stop.color,
        opacity,
      ),
      offset: stop.offset,
    })),
  };
}

// --- Opacity and gradients by reference ------------------------------------------------------------

const PERCENT = 100;

function clamp01(value: number): number {
  return value < 0 ? 0 : value > 1 ? 1 : value;
}

/** An opacity prop as 0 to 1; anything that is not a number is 1. */
export function opacityOf(value: unknown): number {
  return clamp01(num(value, 1));
}

/** An ARGB color with its alpha multiplied by `factor`. */
export function fadeColor(argb: number, factor: number): number {
  if (factor >= 1) return argb >>> 0;
  const alpha = Math.round(((argb >>> ALPHA_SHIFT) & 0xff) * clamp01(factor));
  return ((alpha << ALPHA_SHIFT) | (argb & 0xffffff)) >>> 0;
}

/** A gradient coordinate or stop offset: a number, or a percentage as a fraction (`'50%'` is 0.5). */
export function gradientNumber(value: unknown, fallback: number): number {
  const parsed =
    typeof value === 'string' && value.trim().endsWith('%')
      ? parseFloat(value) / PERCENT
      : num(value, fallback);
  return Number.isFinite(parsed) ? parsed : fallback;
}

/** A gradient's stops before packing: a color, an offset and an opacity, each as written. */
export interface StopInput {
  color: ColorInput;
  offset: unknown;
  opacity: unknown;
}

/**
 * A gradient's stops, packed: each offset clamped to 0..1 and never behind the stop before it, as in SVG, and
 * each color faded by its stop's opacity. A stop with no color is black.
 */
export function packStops(stops: readonly StopInput[]): PackedStop[] {
  const packed: PackedStop[] = [];
  let floor = 0;
  for (const stop of stops) {
    const offset = Math.max(floor, clamp01(gradientNumber(stop.offset, 0)));
    floor = offset;
    packed.push({
      color: fadeColor(
        parseColor(stop.color ?? 'black'),
        opacityOf(stop.opacity),
      ),
      offset,
    });
  }
  return packed;
}

/** What a gradient element is placed by: its tag and its props. */
export interface GradientElement {
  type: 'LinearGradient' | 'RadialGradient';
  props: LinearGradientProps & RadialGradientProps;
}

/**
 * A <LinearGradient> or <RadialGradient> placed on one shape, in the tape's coordinates. In `objectBoundingBox`
 * units, the default, it spans `box`, the shape's box before `transform`. A radial across a box that is not
 * square takes the circle of the same area, as a file's does.
 */
export function placeGradient(
  element: GradientElement,
  stops: PackedStop[],
  box: TapeBounds | null,
  transform: Transform,
): PackedGradient {
  const props = element.props;
  const area =
    props.gradientUnits === 'userSpaceOnUse'
      ? null
      : (box ?? {x: 0, y: 0, width: 0, height: 0});
  const place = (x: number, y: number): [number, number] => {
    const userX = area ? area.x + x * area.width : x;
    const userY = area ? area.y + y * area.height : y;
    return [
      userX * transform.scaleX + transform.translateX,
      userY * transform.scaleY + transform.translateY,
    ];
  };
  if (element.type === 'RadialGradient') {
    const [ax, ay] = place(
      gradientNumber(props.cx, 0.5),
      gradientNumber(props.cy, 0.5),
    );
    const radius =
      gradientNumber(props.r, 0.5) *
      (area ? Math.sqrt(area.width * area.height) : 1) *
      Math.sqrt(Math.abs(transform.scaleX * transform.scaleY));
    return {type: GRAD_RADIAL, stops, ax, ay, bx: 0, by: 0, r: radius};
  }
  const [ax, ay] = place(
    gradientNumber(props.x1, 0),
    gradientNumber(props.y1, 0),
  );
  const [bx, by] = place(
    gradientNumber(props.x2, 1),
    gradientNumber(props.y2, 0),
  );
  return {type: GRAD_LINEAR, stops, ax, ay, bx, by, r: 0};
}

/** A packed gradient with its stops faded by `opacity`. */
function fadeGradient(
  gradient: PackedGradient,
  opacity: number,
): PackedGradient {
  if (opacity >= 1) return gradient;
  return {
    ...gradient,
    stops: gradient.stops.map(stop => ({
      color: fadeColor(stop.color, opacity),
      offset: stop.offset,
    })),
  };
}

// --- Dashed strokes --------------------------------------------------------------------------------

/**
 * A `strokeDasharray` as dash and gap lengths, or null for a solid stroke. An odd count repeats to make
 * pairs, as in SVG. A negative length, a percentage or a pattern of no length is solid.
 */
export function dashPattern(value: unknown): number[] | null {
  if (value == null || value === 'none' || typeof value === 'boolean')
    return null;
  const entries: unknown[] = Array.isArray(value)
    ? value
    : typeof value === 'number'
      ? [value]
      : String(value)
          .trim()
          .split(/[\s,]+/)
          .filter(Boolean);
  const lengths: number[] = [];
  let total = 0;
  for (const entry of entries) {
    if (typeof entry === 'string' && entry.includes('%')) return null;
    const length =
      typeof entry === 'number' ? entry : parseFloat(String(entry));
    if (!Number.isFinite(length) || length < 0) return null;
    lengths.push(length);
    total += length;
  }
  if (!(total > 0)) return null;
  return lengths.length % 2 ? [...lengths, ...lengths] : lengths;
}

/** True for an animated value given as a `strokeDashoffset`. */
function isAnimatedOffset(value: unknown): value is AnimatedOffset {
  return (
    value != null &&
    typeof value === 'object' &&
    (value as AnimatedOffset).__animated === true
  );
}

let _warnedDashCount = false;

/**
 * Appends a shape's dash ops, lengths scaled by `scale`. An animated offset is left to the node's dash
 * offset, which the engine adds at `scale`. Returns false, having written nothing, for a solid stroke.
 */
function pushDash(
  ops: number[],
  dasharray: unknown,
  dashoffset: unknown,
  scale: number,
): boolean {
  const pattern = dashPattern(dasharray);
  if (!pattern) return false;
  if (pattern.length > 2 * DASH_MAX && !_warnedDashCount) {
    _warnedDashCount = true;
    console.warn(
      `embedded-react: a strokeDasharray has more than ${DASH_MAX} dashes; the rest are left off.`,
    );
  }
  const count = Math.min(pattern.length, 2 * DASH_MAX);
  for (let index = 0; index < count; index += 2)
    ops.push(VOP_DASH, pattern[index] * scale, pattern[index + 1] * scale);
  if (isAnimatedOffset(dashoffset)) ops.push(VOP_DASH_OFFSET, 0, scale);
  else {
    const offset = num(dashoffset, 0) * scale;
    if (offset !== 0) ops.push(VOP_DASH_OFFSET, offset, 0);
  }
  return true;
}

/**
 * The animated value an <Svg>'s dashed strokes follow, or null: the first animated `strokeDashoffset` on
 * the <Svg> or a shape under it. A node has one dash offset, so every animated one in an <Svg> moves
 * with it; a second, different value warns once.
 */
export function svgDashOffset<Value extends AnimatedOffset>(
  props: SvgProps,
): Value | null {
  let found: Value | null = null;
  const visit = (elementProps: {
    strokeDashoffset?: unknown;
    children?: unknown;
  }): void => {
    const offset = elementProps.strokeDashoffset;
    if (isAnimatedOffset(offset)) {
      if (found === null) found = offset as Value;
      else if (found !== offset && !sameAnimated(found, offset))
        warnDashOffsets();
    }
    walkChildren(elementProps.children);
  };
  const walkChildren = (child: unknown): void => {
    if (Array.isArray(child)) for (const item of child) walkChildren(item);
    else if (isElement(child)) visit(child.props);
  };
  visit(props);
  return found;
}

/** Whether two animated values are one binding: the same value, or interpolations that say they agree. */
function sameAnimated(first: AnimatedOffset, second: AnimatedOffset): boolean {
  const compare = (first as {__bindEq?: (other: unknown) => boolean}).__bindEq;
  return typeof compare === 'function' && compare.call(first, second);
}

let _warnedDashOffsets = false;

function warnDashOffsets(): void {
  if (_warnedDashOffsets) return;
  _warnedDashOffsets = true;
  console.warn(
    'embedded-react: an <Svg> has two different animated strokeDashoffset values. A node has one dash ' +
      'offset, so every animated one follows the first. Give each its own <Svg>.',
  );
}

// --- Clips and masks -------------------------------------------------------------------------------

/**
 * Writes the mask ops that bring the masks in force to what the next shape is drawn under. A mask only
 * adds to those before it, so going back to fewer lifts them all and starts again.
 */
export class MaskWriter {
  private inForce: readonly MaskGroup[] = [];
  private readonly ops: number[];

  constructor(ops: number[]) {
    this.ops = ops;
  }

  use(wanted: readonly MaskGroup[]): void {
    let shared = 0;
    while (
      shared < this.inForce.length &&
      shared < wanted.length &&
      this.inForce[shared] === wanted[shared]
    )
      shared++;
    if (shared < this.inForce.length) {
      this.ops.push(VOP_MASK, MASK_NONE, 0);
      shared = 0;
    }
    for (let index = shared; index < wanted.length; index++) {
      const groupOps = wanted[index].ops;
      for (let value = 0; value < groupOps.length; value++)
        this.ops.push(groupOps[value]);
    }
    this.inForce = wanted;
  }
}

/** A mask group from its kind and its shapes' tape (SHAPE headers included). */
export function maskGroup(
  kind: number,
  shapeOps: readonly number[],
  shapes: number,
): MaskGroup {
  return {ops: [VOP_MASK, kind, shapes, ...shapeOps], shapes};
}

/**
 * What a tape's masks ask of the engine that it does not keep, or null: more at once than
 * MASK_GROUPS_MAX, or more shapes across them than MASK_SHAPES_MAX.
 */
export function tapeMaskProblem(ops: readonly number[]): string | null {
  let groups = 0;
  let shapes = 0;
  for (let index = 0; index < ops.length;) {
    const opcode = ops[index];
    if (opcode === VOP_MASK) {
      if (ops[index + 1] === MASK_NONE) groups = shapes = 0;
      else {
        groups++;
        shapes += ops[index + 2];
      }
      if (groups > MASK_GROUPS_MAX)
        return `more than ${MASK_GROUPS_MAX} clips and masks apply to one shape`;
      if (shapes > MASK_SHAPES_MAX)
        return `the clips and masks on one shape hold more than ${MASK_SHAPES_MAX} shapes`;
    }
    index += 1 + (OP_OPERANDS[opcode] ?? 0);
  }
  return null;
}

/** How many shapes a tape holds. */
export function tapeShapeCount(ops: readonly number[]): number {
  let shapes = 0;
  for (let index = 0; index < ops.length;) {
    if (ops[index] === VOP_SHAPE) shapes++;
    index += 1 + (OP_OPERANDS[ops[index]] ?? 0);
  }
  return shapes;
}

/** The box around a tape's points (SHAPE, MASK, dash and image ops skipped), or null for a tape with none. */
export function tapeBounds(ops: readonly number[]): TapeBounds | null {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  const add = (x: number, y: number): void => {
    if (x < minX) minX = x;
    if (x > maxX) maxX = x;
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
  };
  for (let index = 0; index < ops.length;) {
    const opcode = ops[index];
    const operands = OP_OPERANDS[opcode] ?? 0;
    if (opcode === VOP_ARC) {
      const radius = Math.abs(ops[index + 3]);
      add(ops[index + 1] - radius, ops[index + 2] - radius);
      add(ops[index + 1] + radius, ops[index + 2] + radius);
    } else if (
      opcode !== VOP_SHAPE &&
      opcode !== VOP_MASK &&
      opcode !== VOP_DASH &&
      opcode !== VOP_DASH_OFFSET &&
      opcode !== VOP_IMAGE
    ) {
      for (let operand = 1; operand < operands; operand += 2)
        add(ops[index + operand], ops[index + operand + 1]);
    }
    index += 1 + operands;
  }
  if (!Number.isFinite(minX)) return null;
  return {x: minX, y: minY, width: maxX - minX, height: maxY - minY};
}

/** `url(#id)` as its id, or null. */
export function urlReference(value: unknown): string | null {
  const match =
    typeof value === 'string' &&
    value.trim().match(/^url\(\s*#([^)\s]+)\s*\)$/);
  return match ? match[1] : null;
}

// --- Flatten an <Svg> subtree --------------------------------------------------------------------

const PAINT_DEFAULT: ResolvedPaint = {
  fill: 'black',
  stroke: 'none',
  fillOpacity: 1,
  strokeOpacity: 1,
  opacity: 1,
  strokeWidth: DEFAULT_STROKE_WIDTH,
  strokeLinecap: 'butt',
  strokeLinejoin: 'miter',
  strokeMiterlimit: DEFAULT_MITER_LIMIT,
  fillRule: 'nonzero',
  strokeDasharray: 'none',
  strokeDashoffset: 0,
  fillGradient: null,
  strokeGradient: null,
};

/** The props that make up a shape's paint; a <G> or the <Svg> itself passes them down. */
export const PAINT_PROPS = Object.keys(PAINT_DEFAULT) as PaintProp[];

/** Paint inherited from `base`, with `props` set over it. An opacity multiplies into the one it inherits. */
function mergePaint(base: ResolvedPaint, props: SvgPaintProps): ResolvedPaint {
  const merged: Record<string, unknown> = {...base};
  for (const key of PAINT_PROPS)
    if (props[key] != null)
      merged[key] =
        key === 'opacity'
          ? opacityOf(base.opacity) * opacityOf(props.opacity)
          : props[key];
  return merged as ResolvedPaint;
}

/** Maps the resolved paint to the PAINT_STRIDE-number record [fill, stroke, strokeWidth, miter, cap, join, rule,
 *  fillGradient, strokeGradient]. A gradient is appended to `gradients` and referenced by its 1-BASED index
 *  (0 = solid), the same encoding shapesToVector uses. */
function paintRecord(
  paint: ResolvedPaint,
  scale: number,
  gradients: PackedGradient[],
  fill: PaintSource,
  stroke: PaintSource,
): number[] {
  return [
    fill.color,
    stroke.color,
    num(paint.strokeWidth, DEFAULT_STROKE_WIDTH) * scale,
    num(paint.strokeMiterlimit, DEFAULT_MITER_LIMIT),
    CAP[paint.strokeLinecap] ?? CAP.butt,
    JOIN[paint.strokeLinejoin] ?? JOIN.miter,
    paint.fillRule === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
    fill.gradient ? gradients.push(fill.gradient) : 0,
    stroke.gradient ? gradients.push(stroke.gradient) : 0,
  ];
}

/** Applies a {scaleX, scaleY, translateX, translateY} transform to a path-op array (no leading SHAPE op). */
function transformOps(ops: readonly number[], transform: Transform): number[] {
  const transformed: number[] = [];
  let index = 0;
  const mapX = (coordinate: number) =>
    coordinate * transform.scaleX + transform.translateX;
  const mapY = (coordinate: number) =>
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
function transformCached(entry: PathEntry, transform: Transform): number[] {
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

function isElement(child: any): child is SvgElement {
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
function isEmptyChild(child: unknown): boolean {
  return (
    child == null ||
    typeof child === 'boolean' ||
    (typeof child === 'string' && child.trim() === '')
  );
}

// Shape/grouping tags flattenSvg knows. A tag outside this set is a mistake worth reporting; a known
// tag that produces no geometry (a <Path> with no `d`, a zero-radius <Circle>) is not.
const SVG_TAGS = [
  'Path',
  'Circle',
  'Ellipse',
  'Rect',
  'Line',
  'Polygon',
  'Polyline',
  'Arc',
  'SvgText',
  'G',
  'ClipPath',
  'Mask',
  'Defs',
  'LinearGradient',
  'RadialGradient',
  'Stop',
];

/** Tags that are found by id and draw nothing where they sit, and what holds them. */
const DEFINITION_TAGS = [
  'ClipPath',
  'Mask',
  'Defs',
  'LinearGradient',
  'RadialGradient',
  'Stop',
];

/** The tags a fill or stroke can name. */
export const isGradientTag = (type: unknown): type is GradientElement['type'] =>
  type === 'LinearGradient' || type === 'RadialGradient';

/** No fill or stroke. */
const NO_PAINT: PaintSource = {color: 0, gradient: null};

/**
 * A shape's path ops before any transform. Path-like geometry comes from a `d` string and is cached (parse and
 * transform) by that string; the primitives build a fixed handful of numbers, which is cheaper than any lookup.
 */
function shapeGeometry(
  type: unknown,
  props: ShapeProps,
): {entry: PathEntry | null; ops: number[] | null} {
  // An <SvgText> arrives baked to a `d` by the build.
  if ((type === 'Path' || type === 'SvgText') && props.d)
    return {entry: pathEntry(props.d), ops: null}; // pathEntry coerces the key
  if (type === 'Ellipse')
    return {entry: pathEntry(ellipsePathD(props)), ops: null};
  if (type === 'Polygon' || type === 'Polyline')
    return {
      entry: pathEntry(polyPathD(props.points, type === 'Polygon')),
      ops: null,
    };
  if (type === 'Circle') return {entry: null, ops: circleOps(props)};
  if (type === 'Rect') return {entry: null, ops: rectOps(props)};
  if (type === 'Line') return {entry: null, ops: lineOps(props)};
  if (type === 'Arc')
    return {
      entry: null,
      ops: arcOpsCW(
        num(props.cx, 0),
        num(props.cy, 0),
        num(props.r, 0),
        num(props.startAngle, 0),
        num(props.endAngle, 0),
      ),
    };
  return {entry: null, ops: null};
}

/** A shape's box before any transform, which a gradient across it spans; null for a shape with no geometry. */
export function shapeBounds(type: string, props: object): TapeBounds | null {
  const {entry, ops} = shapeGeometry(type, props as ShapeProps);
  return tapeBounds((entry ? entry.ops : ops) ?? []);
}

/**
 * The paint a `url(#id)` fill or stroke gives: no stops paint nothing and one paints its color, as in SVG; more
 * are placed as a gradient on the shape. Faded by `opacity`.
 */
export function gradientSource(
  element: GradientElement,
  stops: PackedStop[],
  opacity: number,
  box: () => TapeBounds | null,
  transform: Transform,
): PaintSource {
  if (!stops.length) return NO_PAINT;
  if (stops.length === 1)
    return {color: fadeColor(stops[0].color, opacity), gradient: null};
  return {
    color: fadeColor(gradientFallback(stops), opacity),
    // Only a gradient across the box needs it.
    gradient: fadeGradient(
      placeGradient(
        element,
        stops,
        element.props.gradientUnits === 'userSpaceOnUse' ? null : box(),
        transform,
      ),
      opacity,
    ),
  };
}

/** The <Stop>s in a gradient's children, packed. Anything else there is skipped. */
export function elementStops(children: unknown): PackedStop[] {
  const stops: StopInput[] = [];
  const visit = (child: unknown): void => {
    if (Array.isArray(child)) return child.forEach(visit);
    if (!isElement(child)) return;
    if (child.type === REACT_FRAGMENT) return visit(child.props.children);
    if (child.type === 'Stop')
      stops.push({
        color: child.props.stopColor,
        offset: child.props.offset,
        opacity: child.props.stopOpacity,
      });
  };
  visit(children);
  return packStops(stops);
}

let _warnedSvgPaint = false;

/** Warns once about a fill or stroke an <Svg> cannot paint as written. */
function warnSvgPaint(message: string): void {
  if (_warnedSvgPaint) return;
  _warnedSvgPaint = true;
  console.warn(`embedded-react: ${message}`);
}

/** A <Mask>'s box when it gives none: a tenth past its element's on every side. */
const MASK_REGION_DEFAULT = {x: -0.1, y: -0.1, width: 1.2, height: 1.2};

let _warnedSvgMask = false;

/** Warns once about a clip or mask an <Svg> cannot apply as written. */
function warnSvgMask(message: string): void {
  if (_warnedSvgMask) return;
  _warnedSvgMask = true;
  console.warn(`embedded-react: ${message}`);
}

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
function warnSvgChild(child: unknown): void {
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
 * Handles Path/Circle/Ellipse/Rect/Line/Polygon/Polyline shapes, <G> grouping (inherited paint + translate/scale),
 * gradients a fill or stroke names as url(#id), and a root viewBox -> width/height scale. Arrays and fragments are unwrapped at any depth, so a mapped
 * list of shapes draws like an inline one. A shape's fillGradient/strokeGradient descriptor lands in
 * `gradients` and is referenced 1-based from its paint record. Returns empty arrays when there is
 * nothing to draw.
 */
export function flattenSvg(props: SvgProps): VectorTape {
  const ops: number[] = [];
  const paints: number[] = [];
  const gradients: PackedGradient[] = [];

  // Root transform from viewBox vs width/height.
  let root: Transform = {scaleX: 1, scaleY: 1, translateX: 0, translateY: 0};
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

  // Clips, masks and gradients by id, wherever they sit; found on the first reference to one.
  let definitions: Map<string, SvgElement> | null = null;
  const definition = (id: string): SvgElement | undefined => {
    if (!definitions) {
      const found = new Map<string, SvgElement>();
      const collect = (child: unknown): void => {
        if (Array.isArray(child)) return child.forEach(collect);
        if (!isElement(child)) return;
        if (
          (child.type === 'ClipPath' ||
            child.type === 'Mask' ||
            isGradientTag(child.type)) &&
          child.props.id != null
        )
          found.set(String(child.props.id), child);
        collect(child.props.children);
      };
      collect(props.children);
      definitions = found;
    }
    return definitions.get(id);
  };

  // A clip's shapes share a paint per fill rule, added to the table on first use.
  const clipPaints: Record<string, number> = {};
  const clipPaint = (rule: unknown): number => {
    const key = rule === 'evenodd' ? 'evenodd' : 'nonzero';
    if (clipPaints[key] == null) {
      clipPaints[key] = paints.length / PAINT_STRIDE;
      paints.push(
        ...paintRecord(
          {...PAINT_DEFAULT, fillRule: key},
          1,
          gradients,
          {color: OPAQUE_BLACK, gradient: null},
          NO_PAINT,
        ),
      );
    }
    return clipPaints[key];
  };

  // A gradient's stops, packed once per element.
  const stopCache = new Map<SvgElement, PackedStop[]>();

  /**
   * A fill or stroke: a color or a gradient faded by `opacity`. A `url(#id)` places the gradient it names on the
   * shape, whose box before `transform` is `box`; one that names none paints nothing.
   */
  const paintSource = (
    value: unknown,
    descriptor: VectorGradient | null,
    opacity: number,
    box: () => TapeBounds | null,
    transform: Transform,
  ): PaintSource => {
    const id = urlReference(value);
    if (id == null)
      return {
        color: fadeColor(parseColor(value as ColorInput), opacity),
        gradient: descriptor ? packGradient(descriptor, opacity) : null,
      };
    if (descriptor) {
      const gradient = packGradient(descriptor, opacity);
      return {color: gradientFallback(gradient.stops), gradient};
    }
    const found = definition(id);
    if (!found || !isGradientTag(found.type)) {
      warnSvgPaint(
        `${JSON.stringify(value)} names no <LinearGradient> or <RadialGradient> in its <Svg>, and paints ` +
          'nothing. Give the gradient an id and refer to it as url(#id).',
      );
      return NO_PAINT;
    }
    let stops = stopCache.get(found);
    if (!stops) {
      stops = elementStops(found.props.children);
      stopCache.set(found, stops);
    }
    return gradientSource(
      found as unknown as GradientElement,
      stops,
      opacity,
      box,
      transform,
    );
  };

  // A definition's groups under one transform and box, kept so every shape under it shares them.
  const groupCache = new Map<string, MaskGroup[]>();
  const applying = new Set<SvgElement>();

  /** The masks a `clipPath` or `mask` prop puts on an element. */
  const groupsFor = (
    elementProps: ShapeProps,
    transform: Transform,
    bounds: () => TapeBounds | null,
  ): MaskGroup[] => {
    const groups: MaskGroup[] = [];
    for (const [prop, tag] of [
      ['clipPath', 'ClipPath'],
      ['mask', 'Mask'],
    ] as const) {
      const value = elementProps[prop];
      if (value == null || value === 'none') continue;
      const id = urlReference(value);
      const found = id == null ? undefined : definition(id);
      if (!found || found.type !== tag) {
        warnSvgMask(
          `${prop}=${JSON.stringify(value)} names no <${tag}> in its <Svg>, and was left off. ` +
            `Give the <${tag}> an id and refer to it as url(#id).`,
        );
        continue;
      }
      groups.push(...definitionGroups(found, transform, bounds));
    }
    return groups;
  };

  /** A <ClipPath> or <Mask> as mask groups, in the space of the element it is applied to. */
  const definitionGroups = (
    element: SvgElement,
    transform: Transform,
    bounds: () => TapeBounds | null,
  ): MaskGroup[] => {
    const elementProps = element.props;
    const isClip = element.type === 'ClipPath';
    const boxUnits =
      (isClip ? elementProps.clipPathUnits : elementProps.maskContentUnits) ===
      'objectBoundingBox';
    const boxRegion = !isClip && elementProps.maskUnits !== 'userSpaceOnUse';
    const box = boxUnits || boxRegion ? bounds() : null;
    const key =
      `${String(elementProps.id)}|${transform.scaleX},${transform.scaleY},${transform.translateX},` +
      `${transform.translateY}|${box ? `${box.x},${box.y},${box.width},${box.height}` : ''}`;
    const cached = groupCache.get(key);
    if (cached) return cached;
    if (applying.has(element)) {
      warnSvgMask(
        `<${String(element.type)} id="${String(elementProps.id)}"> applies itself, and was left off.`,
      );
      return [];
    }
    applying.add(element);

    // A definition's own clip or mask narrows it.
    const groups = groupsFor(elementProps, transform, bounds);
    // Its shapes sit in the element's space, or across its box.
    const content: Transform =
      boxUnits && box
        ? {
            scaleX: box.width,
            scaleY: box.height,
            translateX: box.x,
            translateY: box.y,
          }
        : transform;
    const firstPaint = paints.length / PAINT_STRIDE;
    const target: FlattenTarget = {
      ops: [],
      paints,
      gradients,
      mode: isClip ? 'clip' : 'mask',
      writer: null,
    };
    walk(
      elementProps.children,
      mergePaint(PAINT_DEFAULT, elementProps),
      content,
      [],
      target,
      elementProps.clipRule,
    );

    if (!isClip) {
      // A mask stops at its box. The box is only needed as a clip when the element reaches past it and the
      // mask paints past it too.
      let region: TapeBounds | null = null;
      if (boxRegion && box) {
        region = {
          x: box.x + num(elementProps.x, MASK_REGION_DEFAULT.x) * box.width,
          y: box.y + num(elementProps.y, MASK_REGION_DEFAULT.y) * box.height,
          width: num(elementProps.width, MASK_REGION_DEFAULT.width) * box.width,
          height:
            num(elementProps.height, MASK_REGION_DEFAULT.height) * box.height,
        };
      } else if (
        !boxRegion &&
        elementProps.width != null &&
        elementProps.height != null
      ) {
        region = {
          x: num(elementProps.x, 0) * transform.scaleX + transform.translateX,
          y: num(elementProps.y, 0) * transform.scaleY + transform.translateY,
          width: num(elementProps.width, 0) * transform.scaleX,
          height: num(elementProps.height, 0) * transform.scaleY,
        };
      }
      const within = (inner: TapeBounds | null, reach: number): boolean =>
        region != null &&
        inner != null &&
        inner.x - reach >= region.x &&
        inner.y - reach >= region.y &&
        inner.x + inner.width + reach <= region.x + region.width &&
        inner.y + inner.height + reach <= region.y + region.height;
      // Half the widest stroke the mask's shapes carry.
      let reach = 0;
      for (
        let index = firstPaint * PAINT_STRIDE + PAINT_STROKE_WIDTH_SLOT;
        index < paints.length;
        index += PAINT_STRIDE
      )
        reach = Math.max(reach, paints[index] / 2);
      if (
        region &&
        target.ops.length &&
        !within(box ?? bounds(), 0) &&
        !within(tapeBounds(target.ops), reach)
      ) {
        const right = region.x + region.width;
        const bottom = region.y + region.height;
        groups.push(
          maskGroup(
            MASK_CLIP,
            [
              VOP_SHAPE,
              clipPaint('nonzero'),
              VOP_MOVE,
              region.x,
              region.y,
              VOP_LINE,
              right,
              region.y,
              VOP_LINE,
              right,
              bottom,
              VOP_LINE,
              region.x,
              bottom,
              VOP_CLOSE,
            ],
            1,
          ),
        );
      }
    }
    groups.push(
      maskGroup(
        isClip
          ? MASK_CLIP
          : elementProps.maskType === 'alpha'
            ? MASK_ALPHA
            : MASK_LUMINANCE,
        target.ops,
        tapeShapeCount(target.ops),
      ),
    );
    applying.delete(element);
    groupCache.set(key, groups);
    return groups;
  };

  // One child at a time, recursing through the wrappers, JSX inserts around shapes: an array (every
  // `{shapes.map(...)}`, and arrays of arrays when those nest) and a fragment, either of which can
  // sit anywhere a shape can. Both are transparent — they carry no paint and no transform.
  const walk = (
    child: unknown,
    paint: ResolvedPaint,
    transform: Transform,
    masks: readonly MaskGroup[],
    target: FlattenTarget,
    clipRule?: unknown,
  ): void => {
    if (isEmptyChild(child)) return;
    if (Array.isArray(child)) {
      for (const item of child)
        walk(item, paint, transform, masks, target, clipRule);
      return;
    }
    if (!isElement(child)) return warnSvgChild(child);
    const childProps = child.props || {};
    if (child.type === REACT_FRAGMENT)
      return walk(
        childProps.children,
        paint,
        transform,
        masks,
        target,
        clipRule,
      );
    // A definition draws nothing where it sits.
    if (DEFINITION_TAGS.includes(child.type as string)) return;
    const merged = mergePaint(paint, childProps);
    const childRule = childProps.clipRule ?? clipRule;
    let childTransform = transform;
    if (child.type === 'G') {
      // Compose a translate/scale for the group.
      const groupScale = num(childProps.scale, 1);
      const groupX = num(childProps.x ?? childProps.translateX, 0);
      const groupY = num(childProps.y ?? childProps.translateY, 0);
      childTransform = {
        scaleX: transform.scaleX * groupScale,
        scaleY: transform.scaleY * groupScale,
        translateX: groupX * transform.scaleX + transform.translateX,
        translateY: groupY * transform.scaleY + transform.translateY,
      };
    } else if (!SVG_TAGS.includes(child.type as string))
      return warnSvgChild(child);

    let childMasks = masks;
    if (childProps.clipPath != null || childProps.mask != null) {
      if (target.mode === 'draw') {
        // The element's box, for a definition measured against it: its own shapes, drawn nowhere.
        let box: TapeBounds | null | undefined;
        const bounds = (): TapeBounds | null => {
          if (box === undefined) {
            const scratch: FlattenTarget = {
              ops: [],
              paints: [],
              gradients: [],
              mode: 'bounds',
              writer: null,
            };
            walk(child, paint, transform, [], scratch);
            box = tapeBounds(scratch.ops);
          }
          return box;
        };
        childMasks = [
          ...masks,
          ...groupsFor(childProps, childTransform, bounds),
        ];
      } else if (target.mode !== 'bounds') {
        warnSvgMask(
          'a shape inside a <ClipPath> or <Mask> has a clipPath or mask of its own, which was left off. ' +
            'Put it on the <ClipPath> or <Mask> instead.',
        );
      }
    }
    if (child.type === 'G')
      return walk(
        childProps.children,
        merged,
        childTransform,
        childMasks,
        target,
        childRule,
      );

    const {entry, ops: shapeOps} = shapeGeometry(child.type, childProps);
    const baseOps = entry ? entry.ops : shapeOps;
    if (!baseOps || baseOps.length === 0) return;

    const shapeTape = entry
      ? transformCached(entry, transform)
      : transformOps(baseOps, transform);
    let paintIndex = 0;
    let dashScale = 0; // above 0 for a shape whose stroke is drawn
    if (target.mode === 'clip') paintIndex = clipPaint(childRule);
    else if (target.mode !== 'bounds') {
      paintIndex = target.paints.length / PAINT_STRIDE;
      const scale = (transform.scaleX + transform.scaleY) / 2; // stroke-width scale (uniform assumed)
      // A gradient across the shape's box takes its box before the transform.
      let box: TapeBounds | null | undefined;
      const shapeBox = (): TapeBounds | null =>
        box === undefined ? (box = tapeBounds(baseOps)) : box;
      const opacity = opacityOf(merged.opacity);
      const record = paintRecord(
        merged,
        scale,
        target.gradients,
        paintSource(
          merged.fill,
          merged.fillGradient,
          opacity * opacityOf(merged.fillOpacity),
          shapeBox,
          transform,
        ),
        paintSource(
          merged.stroke,
          merged.strokeGradient,
          opacity * opacityOf(merged.strokeOpacity),
          shapeBox,
          transform,
        ),
      );
      target.paints.push(...record);
      if (
        record[PAINT_STROKE_WIDTH_SLOT] > 0 &&
        (record[PAINT_STROKE_SLOT] >>> ALPHA_SHIFT !== 0 ||
          record[PAINT_STROKE_GRADIENT_SLOT] !== 0)
      )
        dashScale = scale;
    }
    target.writer?.use(childMasks);
    // Appended one at a time: `shapeTape` can be a long cached tape, and spreading it into push() both
    // copies it into an argument list and risks the call-argument limit.
    target.ops.push(VOP_SHAPE, paintIndex);
    if (dashScale > 0 && merged.strokeDasharray !== 'none')
      pushDash(
        target.ops,
        merged.strokeDasharray,
        merged.strokeDashoffset,
        dashScale,
      );
    for (let index = 0; index < shapeTape.length; index++)
      target.ops.push(shapeTape[index]);
  };

  // The <Svg>'s own paint cascades to every shape, like a <G> wrapping them all.
  walk(props.children, mergePaint(PAINT_DEFAULT, props), root, [], {
    ops,
    paints,
    gradients,
    mode: 'draw',
    writer: new MaskWriter(ops),
  });
  const problem = tapeMaskProblem(ops);
  if (problem) warnSvgMask(`an <Svg> is drawn unmasked: ${problem}.`);
  return {ops, paints, gradients};
}

// --- Imported SVG artifacts (<Svg source>) -------------------------------------------------------

/**
 * Scales a full op-tape (with SHAPE headers) by (scaleX, scaleY): coordinates, and dash lengths by
 * `strokeScale`; paint indices untouched.
 */
function scaleTape(
  ops: readonly number[],
  scaleX: number,
  scaleY: number,
  strokeScale: number,
): number[] {
  const scaled: number[] = [];
  let index = 0;
  while (index < ops.length) {
    const opcode = ops[index++];
    scaled.push(opcode);
    if (opcode === VOP_SHAPE)
      scaled.push(ops[index++]); // paint index — copy, do not scale
    else if (opcode === VOP_MASK)
      scaled.push(ops[index++], ops[index++]); // kind and shape count
    else if (opcode === VOP_DASH || opcode === VOP_DASH_OFFSET)
      scaled.push(ops[index++] * strokeScale, ops[index++] * strokeScale);
    else if (opcode === VOP_IMAGE)
      // Id and alpha, then the matrix from its pixels: each row scales with its axis.
      scaled.push(
        ops[index++],
        ops[index++],
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++] * scaleY,
        ops[index++] * scaleX,
        ops[index++] * scaleY,
      );
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
export function scaleVectorArtifact(
  artifact: VectorArtifact,
  targetWidth: number,
  targetHeight: number,
): VectorTape {
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
  return {
    ops: scaleTape(artifact.ops, scaleX, scaleY, strokeScale),
    paints,
    gradients,
  };
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
 * @param opsLength        Op-tape length (flat float count).
 * @param paintsLength     Paint-table length (PAINT_STRIDE numbers per shape).
 * @param maxOps           Bridge op-tape cap (NativeUI.maxVectorOps).
 * @param maxPaints        Bridge paint cap, in SHAPES (NativeUI.maxVectorPaints).
 * @param gradientsLength  Gradient count (number of gradients referenced by the shapes).
 * @param maxGradients     Bridge gradient cap (NativeUI.maxVectorGrads).
 */
export function warnVectorCaps(
  opsLength: number,
  paintsLength: number,
  maxOps: number,
  maxPaints: number,
  gradientsLength?: number,
  maxGradients?: number,
): void {
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
    maxGradients! > 0 &&
    gradientsLength! > maxGradients!
  ) {
    _warnedVectorGradients = true;
    console.warn(
      `embedded-react: an <Svg> references ${gradientsLength} gradients (> ${maxGradients}) — the extra gradients are ` +
        `dropped and shapes that use them fall back to solid fills/strokes. Reuse fewer distinct gradients across ` +
        `shapes; raising the limit needs ERUI_VECTOR_GRADIENTS_MAX.`,
    );
  }
}
