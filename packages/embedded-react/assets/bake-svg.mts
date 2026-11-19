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

// Build-time SVG -> engine op-tape baker. Turns an .svg FILE into the same
// {ops, paints} op-tape an inline <Svg> compiles to, so `import logo from './logo.svg'` can render as a
// crisp on-device vector. Every transform (matrix/translate/rotate/scale/skew, nested) is BAKED into
// absolute path coordinates here, so the device needs no transform support — full SVG transform fidelity
// for free. svgson (the XML parser) is a build-time dependency only; it never reaches the device bundle.
//
// Supports <path> + <rect>/<circle>/<ellipse>/<line>/<polygon>/<polyline> + <g> nesting + viewBox + solid
// AND gradient fill/stroke (linear/radial/conic via url() refs; presentation attrs + inline style +
// inheritance). <text> and <tspan> bake to glyph outlines from a font file (svg-text.mts). `clip-path` and
// `mask` bake to mask ops over the shapes they apply to. Features the op-tape can't represent (<filter>,
// <use>, …) are recorded in `dropped` so the caller can rasterize the whole SVG as a fallback image instead. Primitives are emitted as line/cubic paths (no native arc op)
// so an arbitrary matrix bakes cleanly.
import {parse} from 'svgson';
import type {INode} from 'svgson';
import {mkdirSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {createHash} from 'node:crypto';
import {
  parsePath,
  parseColor,
  rectRadii,
  VOP_SHAPE,
  VOP_MOVE,
  VOP_LINE,
  VOP_QUAD,
  VOP_CUBIC,
  VOP_CLOSE,
  CAP,
  JOIN,
  PAINT_STRIDE,
  GRAD_LINEAR,
  GRAD_RADIAL,
  GRAD_CONIC,
  MASK_ALPHA,
  MASK_CLIP,
  MASK_LUMINANCE,
  MaskWriter,
  maskGroup,
  maskReference,
  tapeBounds,
  tapeMaskProblem,
  tapeShapeCount,
} from '../src/embedded-react/svg-ops.mts';
import type {
  MaskGroup,
  PackedGradient,
  PackedStop,
  VectorArtifact,
} from '../src/embedded-react/svg-ops.mts';
import {
  DEFAULT_FONT_SIZE,
  anchorOffset,
  baselineShift,
  checkGlyphs,
  fontFamilyName,
  fontFile,
  parseFontFamilies,
  parseFontWeight,
  parseLetterSpacing,
  parseTextAnchor,
  resolveFont,
  runAdvance,
  runOps,
} from './svg-text.mts';
import type {FontSearch, TextAnchor, TextRun} from './svg-text.mts';
import type opentype from 'opentype.js';

/** A 2D affine matrix [a, b, c, d, e, f]. */
export type Matrix = readonly [number, number, number, number, number, number];

/** An element's attributes, as written. */
type Attributes = Record<string, string>;

/** A shape's paint attributes, resolved through inheritance. */
type Paint = Record<string, string>;

/** A <linearGradient>, <radialGradient> or <conicGradient>. */
interface GradientDefinition {
  name: string;
  attributes: Attributes;
  stops: PackedStop[];
}

interface BBox {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Where a walk writes its shapes, and what it writes them as. */
interface Target {
  ops: number[];
  /** `draw` paints shapes, `clip` and `mask` collect a definition's, `bounds` only measures. */
  mode: 'draw' | 'clip' | 'mask' | 'bounds';
  writer: MaskWriter | null;
}

/** A baked .svg file. */
export interface BakedSvg extends VectorArtifact {
  gradients: PackedGradient[];
  /** SVG features the op-tape can't represent, sorted. */
  dropped: string[];
  /** The fonts its text uses, for a raster fallback to draw the same ones. */
  fonts: RasterFonts;
}

/** The font files a rasterized SVG draws its text with. */
export interface RasterFonts {
  files: string[];
  /** The family the first one names itself, which stands in for every family the SVG asks for. */
  family: string;
}

const HALF_TURN_DEGREES = 180;
const PERCENT = 100;
const DEFAULT_SVG_SIZE = 100;
const DEFAULT_STROKE_WIDTH = 1;
const DEFAULT_MITER_LIMIT = 4;
const FILL_RULE_NONZERO = 0;
const FILL_RULE_EVEN_ODD = 1;
const GRADIENT_CENTER = 0.5; // objectBoundingBox defaults
const GRADIENT_RADIUS = 0.5;
const MIN_GRADIENT_STOPS = 2;
const RASTER_HASH_LENGTH = 8;
/** A <mask>'s box when it gives none: a tenth past its element's on every side. */
const MASK_REGION_DEFAULT = {x: -0.1, y: -0.1, width: 1.2, height: 1.2};

// --- 2D affine matrix [a, b, c, d, e, f]:  x' = a*x + c*y + e ;  y' = b*x + d*y + f ------------------
const IDENTITY: Matrix = [1, 0, 0, 1, 0, 0];

/** Compose outer ∘ inner (apply inner first, then outer). */
function mul(outer: Matrix, inner: Matrix): Matrix {
  const [a1, b1, c1, d1, e1, f1] = outer;
  const [a2, b2, c2, d2, e2, f2] = inner;
  return [
    a1 * a2 + c1 * b2,
    b1 * a2 + d1 * b2,
    a1 * c2 + c1 * d2,
    b1 * c2 + d1 * d2,
    a1 * e2 + c1 * f2 + e1,
    b1 * e2 + d1 * f2 + f1,
  ];
}

/** Parses an SVG `transform` attribute (a chain of functions) into a single matrix. */
export function parseTransform(transform: string | undefined): Matrix {
  let matrix = IDENTITY;
  if (!transform) return matrix;
  const pattern = /(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/g;
  let match;
  while ((match = pattern.exec(transform)) !== null) {
    const functionName = match[1];
    const values = match[2]
      .split(/[\s,]+/)
      .map(Number)
      .filter(value => !Number.isNaN(value));
    let stepMatrix = IDENTITY;
    if (functionName === 'matrix')
      stepMatrix = [
        values[0] || 0,
        values[1] || 0,
        values[2] || 0,
        values[3] || 0,
        values[4] || 0,
        values[5] || 0,
      ];
    else if (functionName === 'translate')
      stepMatrix = [1, 0, 0, 1, values[0] || 0, values[1] || 0];
    else if (functionName === 'scale')
      stepMatrix = [
        values[0] ?? 1,
        0,
        0,
        values.length > 1 ? values[1] : (values[0] ?? 1),
        0,
        0,
      ];
    else if (functionName === 'rotate') {
      const radians = ((values[0] || 0) * Math.PI) / HALF_TURN_DEGREES;
      const cos = Math.cos(radians);
      const sin = Math.sin(radians);
      const rotation: Matrix = [cos, sin, -sin, cos, 0, 0];
      // rotate(angle cx cy) rotates around a point.
      stepMatrix =
        values.length >= 3
          ? mul(mul([1, 0, 0, 1, values[1], values[2]], rotation), [
              1,
              0,
              0,
              1,
              -values[1],
              -values[2],
            ])
          : rotation;
    } else if (functionName === 'skewX')
      stepMatrix = [
        1,
        0,
        Math.tan(((values[0] || 0) * Math.PI) / HALF_TURN_DEGREES),
        1,
        0,
        0,
      ];
    else if (functionName === 'skewY')
      stepMatrix = [
        1,
        Math.tan(((values[0] || 0) * Math.PI) / HALF_TURN_DEGREES),
        0,
        1,
        0,
        0,
      ];
    matrix = mul(matrix, stepMatrix);
  }
  return matrix;
}

/** Applies @p matrix to a point. */
function pt(matrix: Matrix, x: number, y: number): [number, number] {
  return [
    matrix[0] * x + matrix[2] * y + matrix[4],
    matrix[1] * x + matrix[3] * y + matrix[5],
  ];
}

/** Transforms a path op array (no leading SHAPE op) by @p matrix. parsePath only emits MOVE/LINE/QUAD/
 *  CUBIC/CLOSE (it converts arcs to cubics), so every coordinate pair is a plain point — matrix-safe. */
export function transformOps(ops: readonly number[], matrix: Matrix): number[] {
  const transformed: number[] = [];
  let index = 0;
  while (index < ops.length) {
    const opcode = ops[index++];
    transformed.push(opcode);
    if (opcode === VOP_MOVE || opcode === VOP_LINE) {
      const [x, y] = pt(matrix, ops[index++], ops[index++]);
      transformed.push(x, y);
    } else if (opcode === VOP_QUAD) {
      const [controlX, controlY] = pt(matrix, ops[index++], ops[index++]);
      const [x, y] = pt(matrix, ops[index++], ops[index++]);
      transformed.push(controlX, controlY, x, y);
    } else if (opcode === VOP_CUBIC) {
      const [control1X, control1Y] = pt(matrix, ops[index++], ops[index++]);
      const [control2X, control2Y] = pt(matrix, ops[index++], ops[index++]);
      const [x, y] = pt(matrix, ops[index++], ops[index++]);
      transformed.push(control1X, control1Y, control2X, control2Y, x, y);
    }
    // VOP_CLOSE has no args.
  }
  return transformed;
}

/** Uniform stroke-width scale factor for a matrix (geometric mean of the axis scales). */
function scaleOf(matrix: Matrix): number {
  return (
    Math.sqrt(Math.abs(matrix[0] * matrix[3] - matrix[1] * matrix[2])) || 1
  );
}

function num(value: string | undefined, fallback = 0): number {
  const parsed = parseFloat(value as string);
  return Number.isNaN(parsed) ? fallback : parsed;
}

// --- Primitive shapes -> path `d` (emitted as lines/arcs; parsePath turns arcs into cubics) ----------
function rectPath(attributes: Attributes): string {
  const x = num(attributes.x);
  const y = num(attributes.y);
  const width = num(attributes.width);
  const height = num(attributes.height);
  if (width <= 0 || height <= 0) return '';
  const [rx, ry] = rectRadii(attributes.rx, attributes.ry, width, height);
  if (!rx || !ry)
    return `M${x} ${y}L${x + width} ${y}L${x + width} ${y + height}L${x} ${y + height}Z`;
  // Quarter-ellipse corners; parsePath lowers each `A` to the same single cubic the runtime emits.
  return (
    `M${x + rx} ${y}L${x + width - rx} ${y}A${rx} ${ry} 0 0 1 ${x + width} ${y + ry}` +
    `L${x + width} ${y + height - ry}A${rx} ${ry} 0 0 1 ${x + width - rx} ${y + height}` +
    `L${x + rx} ${y + height}A${rx} ${ry} 0 0 1 ${x} ${y + height - ry}` +
    `L${x} ${y + ry}A${rx} ${ry} 0 0 1 ${x + rx} ${y}Z`
  );
}
function circlePath(attributes: Attributes): string {
  const cx = num(attributes.cx);
  const cy = num(attributes.cy);
  const r = num(attributes.r);
  if (r <= 0) return '';
  return `M${cx - r} ${cy}A${r} ${r} 0 1 0 ${cx + r} ${cy}A${r} ${r} 0 1 0 ${cx - r} ${cy}Z`;
}
function ellipsePath(attributes: Attributes): string {
  const cx = num(attributes.cx);
  const cy = num(attributes.cy);
  const rx = num(attributes.rx);
  const ry = num(attributes.ry);
  if (rx <= 0 || ry <= 0) return '';
  return `M${cx - rx} ${cy}A${rx} ${ry} 0 1 0 ${cx + rx} ${cy}A${rx} ${ry} 0 1 0 ${cx - rx} ${cy}Z`;
}
function linePath(attributes: Attributes): string {
  return `M${num(attributes.x1)} ${num(attributes.y1)}L${num(attributes.x2)} ${num(attributes.y2)}`;
}
function polyPath(attributes: Attributes, close: boolean): string {
  const coordinates = String(attributes.points || '')
    .trim()
    .split(/[\s,]+/)
    .map(Number)
    .filter(value => !Number.isNaN(value));
  if (coordinates.length < 4) return '';
  let d = `M${coordinates[0]} ${coordinates[1]}`;
  for (let index = 2; index + 1 < coordinates.length; index += 2)
    d += `L${coordinates[index]} ${coordinates[index + 1]}`;
  return close ? `${d}Z` : d;
}

// --- Paint resolution (presentation attrs + inline style + inheritance) ------------------------------
const PAINT_KEYS = [
  'fill',
  'stroke',
  'stroke-width',
  'stroke-linecap',
  'stroke-linejoin',
  'stroke-miterlimit',
  'fill-rule',
  'opacity',
  'fill-opacity',
  'stroke-opacity',
  'stroke-dasharray',
  'clip-rule',
  'font-family',
  'font-size',
  'font-weight',
  'letter-spacing',
  'text-anchor',
  'dominant-baseline',
];

/** The keys that decide a shape's paint record; text runs that agree on them share one shape. */
const SHAPE_PAINT_KEYS = PAINT_KEYS.filter(
  key => !/^(font|letter|text|dominant)-/.test(key),
);

/** <text> attributes this baker does not lay out; a file that uses one falls back to a raster. */
const UNSUPPORTED_TEXT_ATTRIBUTES = ['rotate', 'textLength', 'writing-mode'];

/** A position attribute on <text> or <tspan>, or the text it holds, in document order. */
type TextItem =
  | {x?: number; y?: number; dx: number; dy: number}
  | {text: string; paint: Paint};

/** Text between two absolute positions; `text-anchor` aligns it as one piece. */
interface TextChunk {
  x: number;
  anchor: TextAnchor;
  runs: Array<{run: TextRun; dx: number; y: number; paint: Paint}>;
}
const DEFAULT_PAINT: Paint = {
  fill: 'black',
  stroke: 'none',
  'stroke-width': '1',
  'stroke-linecap': 'butt',
  'stroke-linejoin': 'miter',
  'stroke-miterlimit': '4',
  'fill-rule': 'nonzero',
};

function parseStyle(style: string | undefined): Record<string, string> {
  const declarations: Record<string, string> = {};
  if (!style) return declarations;
  for (const declaration of String(style).split(';')) {
    const colon = declaration.indexOf(':');
    if (colon > 0)
      declarations[declaration.slice(0, colon).trim()] = declaration
        .slice(colon + 1)
        .trim();
  }
  return declarations;
}

function resolvePaint(attributes: Attributes, inherited: Paint): Paint {
  const style = parseStyle(attributes.style);
  const paint = {...inherited};
  for (const key of PAINT_KEYS) {
    const value = style[key] ?? attributes[key];
    if (value != null) paint[key] = value;
  }
  return paint;
}

/** Resolves a CSS color to a uint32 ARGB. url(...) paints (gradients/patterns) bake to nothing in v1. */
function colorOf(value: string): number {
  if (typeof value === 'string' && value.trim().startsWith('url(')) return 0;
  return parseColor(value);
}

function clamp01(value: number): number {
  return value < 0 ? 0 : value > 1 ? 1 : value;
}

/** Multiplies an ARGB color's alpha by @p factor (SVG opacity / fill-opacity / stroke-opacity, baked in). */
function applyAlpha(argb: number, factor: number): number {
  if (factor >= 1) return argb >>> 0;
  const alpha = Math.round(((argb >>> 24) & 0xff) * clamp01(factor));
  return ((alpha << 24) | (argb & 0xffffff)) >>> 0;
}

// --- Gradients (<linearGradient> / <radialGradient>) -------------------------------------------------

/** Parses <stop>s into [{ color: uint32 (stop-opacity baked in), offset: 0..1 }], in authored order. */
function parseStops(node: INode): PackedStop[] {
  const stops: PackedStop[] = [];
  for (const child of node.children || []) {
    if (child.type !== 'element' || child.name !== 'stop') continue;
    const attributes = child.attributes || {};
    const style = parseStyle(attributes.style);
    const color = style['stop-color'] ?? attributes['stop-color'] ?? 'black';
    const stopOpacity = clamp01(
      num(style['stop-opacity'] ?? attributes['stop-opacity'], 1),
    );
    const offsetText = String(attributes.offset ?? '0').trim();
    const offset = offsetText.endsWith('%')
      ? parseFloat(offsetText) / PERCENT
      : parseFloat(offsetText);
    stops.push({
      color: applyAlpha(parseColor(color), stopOpacity),
      offset: clamp01(Number.isNaN(offset) ? 0 : offset),
    });
  }
  return stops;
}

/** Collects every <linearGradient>/<radialGradient> by id (searched anywhere, including <defs>). */
function collectGradients(root: INode): Map<string, GradientDefinition> {
  const gradients = new Map<string, GradientDefinition>();
  const visit = (node: INode): void => {
    for (const child of node.children || []) {
      if (child.type !== 'element') continue;
      if (
        (child.name === 'linearGradient' ||
          child.name === 'radialGradient' ||
          child.name === 'conicGradient') &&
        child.attributes &&
        child.attributes.id
      )
        gradients.set(child.attributes.id, {
          name: child.name,
          attributes: child.attributes,
          stops: parseStops(child),
        });
      visit(child);
    }
  };
  visit(root);
  return gradients;
}

/** "url(#id)" -> "id" (else null). */
function urlRef(value: string | undefined): string | null {
  const match =
    typeof value === 'string' && value.trim().match(/^url\(\s*#([^)\s]+)\s*\)/);
  return match ? match[1] : null;
}

/** An attribute a style declaration may also give, such as `clip-path`. */
function styled(attributes: Attributes, name: string): string | undefined {
  return parseStyle(attributes.style)[name] ?? attributes[name];
}

/** Collects every <clipPath> and <mask> by id. */
function collectMasks(root: INode): Map<string, INode> {
  const definitions = new Map<string, INode>();
  const visit = (node: INode): void => {
    for (const child of node.children || []) {
      if (child.type !== 'element') continue;
      if (
        (child.name === 'clipPath' || child.name === 'mask') &&
        child.attributes?.id
      )
        definitions.set(child.attributes.id, child);
      visit(child);
    }
  };
  visit(root);
  return definitions;
}

/** A length that may be a percentage: "50%" -> 0.5. */
function fraction(value: string | undefined, fallback: number): number {
  return gradCoord(value, fallback);
}

/** A gradient coordinate: "50%" -> 0.5; a plain number passes through; NaN or infinite returns `fallback`. */
function gradCoord(value: string | undefined, fallback: number): number {
  if (value == null) return fallback;
  const text = String(value).trim();
  const parsed = text.endsWith('%')
    ? parseFloat(text) / PERCENT
    : parseFloat(text);
  return Number.isFinite(parsed) ? parsed : fallback;
}

/** Local-space bounding box of a path-op array (no SHAPE header; MOVE/LINE/QUAD/CUBIC, CLOSE has no args). */
function opsBBox(ops: readonly number[]): BBox {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  let index = 0;
  while (index < ops.length) {
    const opcode = ops[index++];
    const coordinateCount =
      opcode === VOP_MOVE || opcode === VOP_LINE
        ? 2
        : opcode === VOP_QUAD
          ? 4
          : opcode === VOP_CUBIC
            ? 6
            : 0;
    for (let offset = 0; offset < coordinateCount; offset += 2) {
      const x = ops[index + offset];
      const y = ops[index + offset + 1];
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
    index += coordinateCount;
  }
  if (!Number.isFinite(minX)) return {x: 0, y: 0, width: 0, height: 0};
  return {x: minX, y: minY, width: maxX - minX, height: maxY - minY};
}

/**
 * Bakes a gradient definition into an engine gradient descriptor in the path's BAKED coordinate space.
 * objectBoundingBox units (the SVG default) map [0,1] onto the shape's local bbox; userSpaceOnUse uses the
 * coords as-is. Both the gradientTransform and the shape's cumulative matrix are applied. Linear gradients
 * are exact under any affine transform (the endpoints map directly). The engine's radial is CIRCULAR, so an
 * elliptical radial — from a non-square objectBoundingBox or a non-uniform/sheared transform — is reduced to
 * its best-fit circle: the radius is the geometric mean of the two axis radii (radii multiply by sqrt(|det|)
 * under a transform, and a non-square OBB contributes sqrt(w*h)). Conic likewise keeps only the rotation.
 */
function bakeGradient(
  definition: GradientDefinition,
  shapeMatrix: Matrix,
  bbox: BBox,
): PackedGradient {
  const attributes = definition.attributes;
  const objectBoundingBox =
    (attributes.gradientUnits || 'objectBoundingBox') !== 'userSpaceOnUse';
  const gradientMatrix = parseTransform(attributes.gradientTransform);
  const toBaked = (gradientX: number, gradientY: number) => {
    const userX = objectBoundingBox
      ? bbox.x + gradientX * bbox.width
      : gradientX;
    const userY = objectBoundingBox
      ? bbox.y + gradientY * bbox.height
      : gradientY;
    const [transformedX, transformedY] = pt(gradientMatrix, userX, userY);
    return pt(shapeMatrix, transformedX, transformedY);
  };
  if (definition.name === 'radialGradient') {
    const [centerX, centerY] = toBaked(
      gradCoord(attributes.cx, GRADIENT_CENTER),
      gradCoord(attributes.cy, GRADIENT_CENTER),
    );
    // objectBoundingBox radius r=0.5 spans half the box; for a non-square box the geometric mean sqrt(w*h)
    // is the best-fit circle (vs the diagonal, which oversizes a wide/tall box).
    const userRadius = objectBoundingBox
      ? gradCoord(attributes.r, GRADIENT_RADIUS) *
        Math.sqrt(bbox.width * bbox.height)
      : gradCoord(attributes.r, GRADIENT_RADIUS);
    return {
      type: GRAD_RADIAL,
      stops: definition.stops,
      ax: centerX,
      ay: centerY,
      bx: 0,
      by: 0,
      r: userRadius * scaleOf(shapeMatrix) * scaleOf(gradientMatrix),
    };
  }
  if (definition.name === 'conicGradient') {
    const [centerX, centerY] = toBaked(
      gradCoord(attributes.cx, GRADIENT_CENTER),
      gradCoord(attributes.cy, GRADIENT_CENTER),
    );
    // `from` (degrees, clockwise from the top, CSS conic-gradient convention) -> radians, plus the rotation
    // baked into the gradient + path transforms so the sweep orients correctly in the final coordinate space.
    const rotation =
      Math.atan2(shapeMatrix[1], shapeMatrix[0]) +
      Math.atan2(gradientMatrix[1], gradientMatrix[0]);
    return {
      type: GRAD_CONIC,
      stops: definition.stops,
      ax: centerX,
      ay: centerY,
      bx: 0,
      by: 0,
      r: (num(attributes.from, 0) * Math.PI) / HALF_TURN_DEGREES + rotation,
    };
  }
  const [ax, ay] = toBaked(
    gradCoord(attributes.x1, 0),
    gradCoord(attributes.y1, 0),
  );
  const [bx, by] = toBaked(
    gradCoord(attributes.x2, 1),
    gradCoord(attributes.y2, 0),
  );
  return {type: GRAD_LINEAR, stops: definition.stops, ax, ay, bx, by, r: 0};
}

/**
 * Compiles an SVG document string into the engine vector op-tape.
 *
 * @param svgString  The full SVG file contents.
 * @param fonts  Where the fonts its text names are found. Without it, a file with text fails.
 * @returns op-tape + intrinsic
 *          size (px). `ops`/`paints` are plain arrays ready to inline into the bundle; `width`/`height`
 *          are the SVG's rendered size for layout (a <Svg source> scales the tape to its box).
 */
export async function svgToVector(
  svgString: string,
  fonts: FontSearch = {imports: new Map(), directories: []},
): Promise<BakedSvg> {
  const root = await parse(String(svgString));
  const attributes = root.attributes || {};

  let viewBox = String(attributes.viewBox || '')
    .trim()
    .split(/[\s,]+/)
    .map(Number);
  if (viewBox.length !== 4 || viewBox.some(value => Number.isNaN(value)))
    viewBox = [
      0,
      0,
      num(attributes.width, DEFAULT_SVG_SIZE),
      num(attributes.height, DEFAULT_SVG_SIZE),
    ];
  const [viewBoxX, viewBoxY, viewBoxWidth, viewBoxHeight] = viewBox;
  const width = num(attributes.width, viewBoxWidth);
  const height = num(attributes.height, viewBoxHeight);

  // Root transform: map the viewBox user-units onto the intrinsic px box (like the inline <Svg> path).
  const scaleX = viewBoxWidth ? width / viewBoxWidth : 1;
  const scaleY = viewBoxHeight ? height / viewBoxHeight : 1;
  const rootMatrix = mul(
    [scaleX, 0, 0, scaleY, -viewBoxX * scaleX, -viewBoxY * scaleY],
    parseTransform(attributes.transform),
  );

  const ops: number[] = [];
  const paints: number[] = [];
  const gradients: PackedGradient[] = [];
  const gradientDefinitions = collectGradients(root);

  // Elements we intentionally skip without losing visible content (gradient defs are collected separately,
  // the rest are metadata). Anything else hitting the skip branch — image/use/mask/filter/pattern/… —
  // is unsupported VISUAL content, recorded in `dropped` so the loader can warn + fall back to raster.
  const SVG_IGNORABLE = new Set([
    'defs',
    'title',
    'desc',
    'metadata',
    'linearGradient',
    'radialGradient',
    'conicGradient',
    'clipPath',
    'mask',
  ]);
  const dropped = new Set<string>();
  const usedFonts = new Set<opentype.Font>();
  const maskDefinitions = collectMasks(root);

  // A clip's shapes share a paint per fill rule, added to the table on first use.
  const clipPaints: Record<string, number> = {};
  const clipPaint = (rule: string | undefined): number => {
    const key = rule === 'evenodd' ? 'evenodd' : 'nonzero';
    if (clipPaints[key] == null) {
      clipPaints[key] = paints.length / PAINT_STRIDE;
      paints.push(
        parseColor('black'),
        0,
        0,
        DEFAULT_MITER_LIMIT,
        0,
        0,
        key === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
        0,
        0,
      );
    }
    return clipPaints[key];
  };

  /** Appends one shape: its paint record and its ops, moved by @p matrix. */
  const emitShape = (
    shapeOps: readonly number[],
    matrix: Matrix,
    paint: Paint,
    attributes: Attributes,
    masks: readonly MaskGroup[],
    target: Target,
  ): void => {
    if (target.mode === 'bounds') {
      target.ops.push(VOP_SHAPE, 0, ...transformOps(shapeOps, matrix));
      return;
    }
    // A filter applied to an otherwise-bakeable shape still loses fidelity as a vector → flag it.
    const filter = styled(attributes, 'filter');
    if (filter && filter !== 'none') dropped.add('filter');
    if (target.mode === 'clip') {
      target.ops.push(
        VOP_SHAPE,
        clipPaint(paint['clip-rule']),
        ...transformOps(shapeOps, matrix),
      );
      return;
    }
    // A dashed stroke can't be represented in the op-tape (the stroke is baked solid). Flag it so the SVG
    // falls back to raster (resvg renders the dashes) — but only when a visible stroke is actually dashed
    // by a positive pattern, so a stray stroke-dasharray on an unstroked/zero-width shape isn't penalized.
    const dash = paint['stroke-dasharray'];
    const stroked =
      paint.stroke &&
      paint.stroke !== 'none' &&
      num(paint['stroke-width'], DEFAULT_STROKE_WIDTH) > 0;
    if (stroked && dash && dash !== 'none' && /[1-9]/.test(dash))
      dropped.add('stroke-dasharray');

    const paintIndex = paints.length / PAINT_STRIDE;
    const opacity = clamp01(num(paint.opacity, 1));
    // A url(#id) fill/stroke referencing a parsed gradient bakes to a 1-based gradient-table index; the
    // solid color stays 0 (colorOf returns 0 for url()). The shape's bbox is computed once, lazily.
    let bbox: BBox | null = null;
    const gradientIndex = (reference: string | null): number => {
      if (!reference) return 0;
      const definition = gradientDefinitions.get(reference);
      if (!definition || definition.stops.length < MIN_GRADIENT_STOPS) return 0;
      if (!bbox) bbox = opsBBox(shapeOps);
      gradients.push(bakeGradient(definition, matrix, bbox));
      return gradients.length; // 1-based
    };
    const fillGradient = gradientIndex(urlRef(paint.fill));
    const strokeGradient = gradientIndex(urlRef(paint.stroke));
    target.writer?.use(masks);
    target.ops.push(VOP_SHAPE, paintIndex, ...transformOps(shapeOps, matrix));
    paints.push(
      applyAlpha(
        colorOf(paint.fill ?? 'black'),
        opacity * clamp01(num(paint['fill-opacity'], 1)),
      ),
      applyAlpha(
        colorOf(paint.stroke ?? 'none'),
        opacity * clamp01(num(paint['stroke-opacity'], 1)),
      ),
      num(paint['stroke-width'], DEFAULT_STROKE_WIDTH) * scaleOf(matrix),
      num(paint['stroke-miterlimit'], DEFAULT_MITER_LIMIT),
      CAP[paint['stroke-linecap']] ?? 0,
      JOIN[paint['stroke-linejoin']] ?? 0,
      paint['fill-rule'] === 'evenodd' ? FILL_RULE_EVEN_ODD : FILL_RULE_NONZERO,
      fillGradient,
      strokeGradient,
    );
  };

  /** Flattens a <text>'s content into positions and text, in document order. */
  const textItems = (node: INode, paint: Paint, items: TextItem[]): void => {
    const attributes = node.attributes || {};
    const single = (key: string): number | undefined => {
      const values = String(attributes[key] ?? '')
        .trim()
        .split(/[\s,]+/)
        .filter(Boolean);
      if (values.length > 1) dropped.add(`text ${key} list`);
      return values.length === 1 ? num(values[0]) : undefined;
    };
    for (const key of UNSUPPORTED_TEXT_ATTRIBUTES) {
      if (attributes[key]) dropped.add(`text ${key}`);
    }
    items.push({
      x: single('x'),
      y: single('y'),
      dx: single('dx') ?? 0,
      dy: single('dy') ?? 0,
    });
    for (const child of node.children || []) {
      if (child.type === 'text') items.push({text: child.value, paint});
      else if (child.name === 'tspan')
        textItems(child, resolvePaint(child.attributes || {}, paint), items);
      else if (child.type === 'element' && !SVG_IGNORABLE.has(child.name))
        dropped.add(child.name);
    }
  };

  /** Bakes a <text> to glyph outlines: one shape per stretch of runs that share a paint. */
  const bakeText = (
    node: INode,
    matrix: Matrix,
    paint: Paint,
    masks: readonly MaskGroup[],
    target: Target,
  ): void => {
    const items: TextItem[] = [];
    const droppedBefore = dropped.size;
    textItems(node, paint, items);

    // XML whitespace collapses to single spaces, and none leads or trails the element.
    const preserve = (node.attributes || {})['xml:space'] === 'preserve';
    let endsWithSpace = true;
    let lastText: {text: string} | null = null;
    for (const item of items) {
      if (!('text' in item)) continue;
      item.text = item.text.replace(preserve ? /\s/g : /\s+/g, ' ');
      if (!preserve && endsWithSpace) item.text = item.text.replace(/^ /, '');
      if (item.text) {
        endsWithSpace = item.text.endsWith(' ');
        lastText = item;
      }
    }
    if (lastText && !preserve) lastText.text = lastText.text.replace(/ $/, '');

    // Every run's font and glyphs are checked, whichever way the file ends up drawn.
    for (const item of items) {
      if (!('text' in item) || !item.text) continue;
      const font = resolveFont(
        parseFontFamilies(item.paint['font-family']),
        parseFontWeight(item.paint['font-weight']),
        fonts,
      );
      checkGlyphs(font, item.text);
      usedFonts.add(font);
    }
    // Text this baker cannot lay out leaves the whole file to the raster fallback.
    if (dropped.size > droppedBefore) return;

    const shapes: Array<{paint: Paint; ops: number[]}> = [];
    let penX = 0;
    let penY = 0;
    let pendingDx = 0;
    let chunk: TextChunk = {x: 0, anchor: 'start', runs: []};
    const flush = (): void => {
      const width = chunk.runs.reduce(
        (total, placed) => total + placed.dx + runAdvance(placed.run),
        0,
      );
      let cursor = chunk.x + anchorOffset(chunk.anchor, width);
      for (const placed of chunk.runs) {
        cursor += placed.dx;
        const glyphOps = runOps(placed.run, cursor, placed.y);
        cursor += runAdvance(placed.run);
        const last = shapes[shapes.length - 1];
        if (
          last &&
          SHAPE_PAINT_KEYS.every(key => last.paint[key] === placed.paint[key])
        )
          last.ops.push(...glyphOps);
        else shapes.push({paint: placed.paint, ops: glyphOps});
      }
      penX = cursor;
    };

    for (const item of items) {
      if (!('text' in item)) {
        if (item.x !== undefined || item.y !== undefined) {
          flush();
          chunk = {x: item.x ?? penX, anchor: 'start', runs: []};
          penY = item.y ?? penY;
        }
        pendingDx += item.dx;
        penY += item.dy;
        continue;
      }
      if (!item.text) continue;
      const fontSize = num(item.paint['font-size'], DEFAULT_FONT_SIZE);
      const font = resolveFont(
        parseFontFamilies(item.paint['font-family']),
        parseFontWeight(item.paint['font-weight']),
        fonts,
      );
      if (chunk.runs.length === 0)
        chunk.anchor = parseTextAnchor(item.paint['text-anchor']);
      chunk.runs.push({
        run: {
          text: item.text,
          font,
          fontSize,
          letterSpacing: parseLetterSpacing(
            item.paint['letter-spacing'],
            fontSize,
          ),
        },
        dx: pendingDx,
        y:
          penY + baselineShift(font, fontSize, item.paint['dominant-baseline']),
        paint: item.paint,
      });
      pendingDx = 0;
    }
    flush();

    for (const shape of shapes) {
      if (shape.ops.length)
        emitShape(
          shape.ops,
          matrix,
          shape.paint,
          node.attributes || {},
          masks,
          target,
        );
    }
  };

  // A definition's groups under one matrix and box, kept so every shape under it shares them.
  const groupCache = new Map<string, MaskGroup[]>();
  const applying = new Set<INode>();

  /** The masks an element's `clip-path` and `mask` put on it. */
  const groupsFor = (
    attributes: Attributes,
    matrix: Matrix,
    bounds: () => BBox,
  ): MaskGroup[] => {
    const groups: MaskGroup[] = [];
    for (const [name, tag] of [
      ['clip-path', 'clipPath'],
      ['mask', 'mask'],
    ]) {
      const value = styled(attributes, name);
      if (!value || value === 'none') continue;
      const id = maskReference(value);
      const definition = id == null ? undefined : maskDefinitions.get(id);
      if (!definition || definition.name !== tag) {
        dropped.add(name); // a shape function, or a reference to nothing
        continue;
      }
      groups.push(...definitionGroups(definition, matrix, bounds));
    }
    return groups;
  };

  /** A <clipPath> or <mask> as mask groups, in the space of the element it is applied to. */
  const definitionGroups = (
    definition: INode,
    matrix: Matrix,
    bounds: () => BBox,
  ): MaskGroup[] => {
    const attributes = definition.attributes || {};
    const isClip = definition.name === 'clipPath';
    const boxUnits =
      (isClip ? attributes.clipPathUnits : attributes.maskContentUnits) ===
      'objectBoundingBox';
    const boxRegion = !isClip && attributes.maskUnits !== 'userSpaceOnUse';
    const box = boxUnits || boxRegion ? bounds() : null;
    const key = `${attributes.id}|${matrix.join(',')}|${box ? `${box.x},${box.y},${box.width},${box.height}` : ''}`;
    const cached = groupCache.get(key);
    if (cached) return cached;
    if (applying.has(definition)) {
      dropped.add(`${definition.name} that applies itself`);
      return [];
    }
    applying.add(definition);

    // A definition's own clip or mask narrows it.
    const groups = groupsFor(attributes, matrix, bounds);
    // Its shapes sit in the element's space, or across its box; a <clipPath> may be transformed too.
    let content = matrix;
    if (boxUnits && box)
      content = mul(content, [box.width, 0, 0, box.height, box.x, box.y]);
    if (isClip) content = mul(content, parseTransform(attributes.transform));
    const firstPaint = paints.length / PAINT_STRIDE;
    const target: Target = {
      ops: [],
      mode: isClip ? 'clip' : 'mask',
      writer: null,
    };
    walk(
      definition,
      content,
      resolvePaint(attributes, DEFAULT_PAINT),
      [],
      target,
    );

    if (!isClip) {
      // A mask stops at its box. The box is only needed as a clip when the element reaches past it and the
      // mask paints past it too.
      let region: BBox | null = null;
      if (boxRegion && box) {
        region = {
          x: box.x + fraction(attributes.x, MASK_REGION_DEFAULT.x) * box.width,
          y: box.y + fraction(attributes.y, MASK_REGION_DEFAULT.y) * box.height,
          width:
            fraction(attributes.width, MASK_REGION_DEFAULT.width) * box.width,
          height:
            fraction(attributes.height, MASK_REGION_DEFAULT.height) *
            box.height,
        };
      } else if (
        !boxRegion &&
        attributes.width != null &&
        attributes.height != null &&
        !/%/.test(
          `${attributes.x}${attributes.y}${attributes.width}${attributes.height}`,
        )
      ) {
        region = {
          x: num(attributes.x),
          y: num(attributes.y),
          width: num(attributes.width),
          height: num(attributes.height),
        };
      }
      const element = region ? (box ?? bounds()) : null;
      const covered =
        region &&
        element &&
        region.x <= element.x &&
        region.y <= element.y &&
        region.x + region.width >= element.x + element.width &&
        region.y + region.height >= element.y + element.height;
      if (region && !covered) {
        const corners = [
          pt(matrix, region.x, region.y),
          pt(matrix, region.x + region.width, region.y),
          pt(matrix, region.x + region.width, region.y + region.height),
          pt(matrix, region.x, region.y + region.height),
        ];
        const painted = tapeBounds(target.ops);
        let reach = 0;
        for (
          let index = firstPaint * PAINT_STRIDE + 2;
          index < paints.length;
          index += PAINT_STRIDE
        )
          reach = Math.max(reach, paints[index] / 2);
        // Compared as boxes when the element's space is upright; a turned box is always kept.
        const upright = matrix[1] === 0 && matrix[2] === 0;
        const left = Math.min(corners[0][0], corners[2][0]);
        const right = Math.max(corners[0][0], corners[2][0]);
        const top = Math.min(corners[0][1], corners[2][1]);
        const bottom = Math.max(corners[0][1], corners[2][1]);
        const inside =
          upright &&
          painted &&
          painted.x - reach >= left &&
          painted.y - reach >= top &&
          painted.x + painted.width + reach <= right &&
          painted.y + painted.height + reach <= bottom;
        if (painted && !inside) {
          const rect = [VOP_SHAPE, clipPaint('nonzero')];
          corners.forEach(([x, y], corner) =>
            rect.push(corner === 0 ? VOP_MOVE : VOP_LINE, x, y),
          );
          rect.push(VOP_CLOSE);
          groups.push(maskGroup(MASK_CLIP, rect, 1));
        }
      }
    }
    const maskType = styled(attributes, 'mask-type');
    groups.push(
      maskGroup(
        isClip ? MASK_CLIP : maskType === 'alpha' ? MASK_ALPHA : MASK_LUMINANCE,
        target.ops,
        tapeShapeCount(target.ops),
      ),
    );
    applying.delete(definition);
    groupCache.set(key, groups);
    return groups;
  };

  /** An element's box in its own space: its shapes' points, strokes left out. */
  const localBounds = (node: INode, paint: Paint): BBox => {
    const scratch: Target = {ops: [], mode: 'bounds', writer: null};
    // Its own `transform` is part of the space the box is measured in, so it is left off here.
    const unplaced = {
      ...node,
      attributes: {...(node.attributes || {}), transform: ''},
    };
    walk({...node, children: [unplaced]}, IDENTITY, paint, [], scratch);
    return tapeBounds(scratch.ops) ?? {x: 0, y: 0, width: 0, height: 0};
  };

  const walk = (
    node: INode,
    matrix: Matrix,
    paint: Paint,
    masks: readonly MaskGroup[],
    target: Target,
  ): void => {
    for (const child of node.children || []) {
      if (child.type !== 'element') continue;
      const childAttributes = child.attributes || {};
      const childMatrix = mul(
        matrix,
        parseTransform(childAttributes.transform),
      );
      const childPaint = resolvePaint(childAttributes, paint);

      let childMasks = masks;
      const applied = [
        styled(childAttributes, 'clip-path'),
        styled(childAttributes, 'mask'),
      ].some(value => value && value !== 'none');
      if (applied && !SVG_IGNORABLE.has(child.name)) {
        if (target.mode === 'draw') {
          let box: BBox | undefined;
          childMasks = [
            ...masks,
            ...groupsFor(
              childAttributes,
              childMatrix,
              () => (box ??= localBounds(child, paint)),
            ),
          ];
        } else if (target.mode !== 'bounds') {
          // A clip or mask on a shape inside one would have to be worked out shape by shape.
          dropped.add(`${node.name} child with a clip-path or mask`);
        }
      }

      if (child.name === 'g' || child.name === 'svg') {
        const filter = styled(childAttributes, 'filter');
        if (filter && filter !== 'none') dropped.add('filter');
        walk(child, childMatrix, childPaint, childMasks, target);
        continue;
      }
      if (child.name === 'text') {
        bakeText(child, childMatrix, childPaint, childMasks, target);
        continue;
      }

      let d = '';
      if (child.name === 'path') d = childAttributes.d || '';
      else if (child.name === 'rect') d = rectPath(childAttributes);
      else if (child.name === 'circle') d = circlePath(childAttributes);
      else if (child.name === 'ellipse') d = ellipsePath(childAttributes);
      else if (child.name === 'line') d = linePath(childAttributes);
      else if (child.name === 'polygon') d = polyPath(childAttributes, true);
      else if (child.name === 'polyline') d = polyPath(childAttributes, false);
      else {
        if (!SVG_IGNORABLE.has(child.name)) dropped.add(child.name); // unsupported visual element → raster
        continue;
      }
      if (!d) continue;

      const shapeOps = parsePath(d);
      if (shapeOps.length)
        emitShape(
          shapeOps,
          childMatrix,
          childPaint,
          childAttributes,
          childMasks,
          target,
        );
    }
  };

  walk(root, rootMatrix, resolvePaint(attributes, DEFAULT_PAINT), [], {
    ops,
    mode: 'draw',
    writer: new MaskWriter(ops),
  });
  // More clips and masks on one shape than the engine keeps: the file is left to the raster fallback.
  if (tapeMaskProblem(ops)) dropped.add('clip-path and mask nesting');
  return {
    ops,
    paints,
    gradients,
    width,
    height,
    dropped: [...dropped].sort(),
    fonts: {
      files: [...usedFonts].map(fontFile),
      family: usedFonts.size ? fontFamilyName([...usedFonts][0]) : '',
    },
  };
}

/**
 * Rasterizes an SVG to an RGBA PNG at its intrinsic size — the raster fallback for SVGs that use features
 * the vector baker can't represent (text, masks, filters, patterns, …). resvg is
 * imported lazily, so vector-only builds never load it. The PNG flows through the normal image pipeline
 * (bake-image → asset pack), and a <Svg source> whose artifact is {kind:'raster'} renders it as an image.
 *
 * @param svgString  The SVG document.
 * @param fonts  The fonts svgToVector found for its text. Text is drawn with these and never with the build
 *               machine's own fonts, so the image is the same wherever it is built.
 * @returns Rendered size and PNG bytes.
 */
export async function svgToRaster(
  svgString: string,
  fonts: RasterFonts = {files: [], family: ''},
): Promise<{width: number; height: number; png: Buffer}> {
  const {Resvg} = await import('@resvg/resvg-js');
  // resvg (usvg) requires the SVG namespace; svgson (the vector baker) is lenient and many hand-authored
  // SVGs omit it, so inject it when absent or resvg rejects the document ("does not have a root node").
  let svg = String(svgString);
  if (!/<svg\b[^>]*\sxmlns\s*=/i.test(svg))
    svg = svg.replace(/<svg\b/i, '<svg xmlns="http://www.w3.org/2000/svg"');
  const resvg = new Resvg(svg, {
    background: 'rgba(0,0,0,0)', // transparent so it composites
    font: {
      loadSystemFonts: false,
      fontFiles: fonts.files,
      defaultFontFamily: fonts.family,
    },
  });
  const rendered = resvg.render();
  const png = rendered.asPng();
  const {width, height} = rendered;
  // Only the wasm build has free().
  (rendered as {free?: () => void}).free?.();
  return {width, height, png};
}

/**
 * Writes a rasterized-SVG PNG to a temp file and returns its path (content-hashed so rebuilds are stable).
 * The image pipeline (bakeImage) reads PNGs by path, so the raster fallback writes one here and registers it
 * as an asset. Shared by the Flow A loader and the Flow B (AOT) baker.
 *
 * @param name  Asset name (the SVG's basename) — used in the filename.
 * @param png   PNG bytes from svgToRaster.
 * @returns Absolute path to the written .png.
 */
export function writeRasterPng(name: string, png: Uint8Array): string {
  const directory = join(tmpdir(), 'embedded-react-svg-raster');
  mkdirSync(directory, {recursive: true});
  const file = join(
    directory,
    `${name}-${createHash('sha1').update(png).digest('hex').slice(0, RASTER_HASH_LENGTH)}.png`,
  );
  writeFileSync(file, png);
  return file;
}
