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
// inheritance). Features the op-tape can't represent (<text>, <mask>, <filter>, <use>, …) are recorded in
// `dropped` so the caller can rasterize the whole SVG as a fallback image instead. Primitives are emitted as
// line/cubic paths (no native arc op) so an arbitrary matrix bakes cleanly.
import {parse} from 'svgson';
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
  CAP,
  JOIN,
  PAINT_STRIDE,
  GRAD_LINEAR,
  GRAD_RADIAL,
  GRAD_CONIC,
} from '../src/embedded-react/svg-ops.js';

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

// --- 2D affine matrix [a, b, c, d, e, f]:  x' = a*x + c*y + e ;  y' = b*x + d*y + f ------------------
const IDENTITY = [1, 0, 0, 1, 0, 0];

/** Compose outer ∘ inner (apply inner first, then outer). */
function mul(outer, inner) {
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
function parseTransform(transform) {
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
      const rotation = [cos, sin, -sin, cos, 0, 0];
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
function pt(matrix, x, y) {
  return [
    matrix[0] * x + matrix[2] * y + matrix[4],
    matrix[1] * x + matrix[3] * y + matrix[5],
  ];
}

/** Transforms a path op array (no leading SHAPE op) by @p matrix. parsePath only emits MOVE/LINE/QUAD/
 *  CUBIC/CLOSE (it converts arcs to cubics), so every coordinate pair is a plain point — matrix-safe. */
function transformOps(ops, matrix) {
  const transformed = [];
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
function scaleOf(matrix) {
  return (
    Math.sqrt(Math.abs(matrix[0] * matrix[3] - matrix[1] * matrix[2])) || 1
  );
}

function num(value, fallback = 0) {
  const parsed = parseFloat(value);
  return Number.isNaN(parsed) ? fallback : parsed;
}

// --- Primitive shapes -> path `d` (emitted as lines/arcs; parsePath turns arcs into cubics) ----------
function rectPath(attributes) {
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
function circlePath(attributes) {
  const cx = num(attributes.cx);
  const cy = num(attributes.cy);
  const r = num(attributes.r);
  if (r <= 0) return '';
  return `M${cx - r} ${cy}A${r} ${r} 0 1 0 ${cx + r} ${cy}A${r} ${r} 0 1 0 ${cx - r} ${cy}Z`;
}
function ellipsePath(attributes) {
  const cx = num(attributes.cx);
  const cy = num(attributes.cy);
  const rx = num(attributes.rx);
  const ry = num(attributes.ry);
  if (rx <= 0 || ry <= 0) return '';
  return `M${cx - rx} ${cy}A${rx} ${ry} 0 1 0 ${cx + rx} ${cy}A${rx} ${ry} 0 1 0 ${cx - rx} ${cy}Z`;
}
function linePath(attributes) {
  return `M${num(attributes.x1)} ${num(attributes.y1)}L${num(attributes.x2)} ${num(attributes.y2)}`;
}
function polyPath(attributes, close) {
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
];
const DEFAULT_PAINT = {
  fill: 'black',
  stroke: 'none',
  'stroke-width': '1',
  'stroke-linecap': 'butt',
  'stroke-linejoin': 'miter',
  'stroke-miterlimit': '4',
  'fill-rule': 'nonzero',
};

function parseStyle(style) {
  const declarations = {};
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

function resolvePaint(attributes, inherited) {
  const style = parseStyle(attributes.style);
  const paint = {...inherited};
  for (const key of PAINT_KEYS) {
    const value = style[key] ?? attributes[key];
    if (value != null) paint[key] = value;
  }
  return paint;
}

/** Resolves a CSS color to a uint32 ARGB. url(...) paints (gradients/patterns) bake to nothing in v1. */
function colorOf(value) {
  if (typeof value === 'string' && value.trim().startsWith('url(')) return 0;
  return parseColor(value);
}

function clamp01(value) {
  return value < 0 ? 0 : value > 1 ? 1 : value;
}

/** Multiplies an ARGB color's alpha by @p factor (SVG opacity / fill-opacity / stroke-opacity, baked in). */
function applyAlpha(argb, factor) {
  if (factor >= 1) return argb >>> 0;
  const alpha = Math.round(((argb >>> 24) & 0xff) * clamp01(factor));
  return ((alpha << 24) | (argb & 0xffffff)) >>> 0;
}

// --- Gradients (<linearGradient> / <radialGradient>) -------------------------------------------------

/** Parses <stop>s into [{ color: uint32 (stop-opacity baked in), offset: 0..1 }], in authored order. */
function parseStops(node) {
  const stops = [];
  for (const child of node.children || []) {
    if (child.type !== 'element' || child.name !== 'stop') continue;
    const attributes = child.attributes || {};
    const style = parseStyle(attributes.style);
    const color = style['stop-color'] ?? attributes['stop-color'] ?? 'black';
    const stopOpacity = clamp01(
      num(style['stop-opacity'] ?? attributes['stop-opacity'], 1),
    );
    let offset = String(attributes.offset ?? '0').trim();
    offset = offset.endsWith('%')
      ? parseFloat(offset) / PERCENT
      : parseFloat(offset);
    stops.push({
      color: applyAlpha(parseColor(color), stopOpacity),
      offset: clamp01(Number.isNaN(offset) ? 0 : offset),
    });
  }
  return stops;
}

/** Collects every <linearGradient>/<radialGradient> by id (searched anywhere, including <defs>). */
function collectGradients(root) {
  const gradients = new Map();
  const visit = node => {
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
function urlRef(value) {
  const match =
    typeof value === 'string' && value.trim().match(/^url\(\s*#([^)\s]+)\s*\)/);
  return match ? match[1] : null;
}

/** A gradient coordinate: "50%" -> 0.5; a plain number passes through; NaN or infinite returns `fallback`. */
function gradCoord(value, fallback) {
  if (value == null) return fallback;
  const text = String(value).trim();
  const parsed = text.endsWith('%')
    ? parseFloat(text) / PERCENT
    : parseFloat(text);
  return Number.isFinite(parsed) ? parsed : fallback;
}

/** Local-space bounding box of a path-op array (no SHAPE header; MOVE/LINE/QUAD/CUBIC, CLOSE has no args). */
function opsBBox(ops) {
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
function bakeGradient(definition, shapeMatrix, bbox) {
  const attributes = definition.attributes;
  const objectBoundingBox =
    (attributes.gradientUnits || 'objectBoundingBox') !== 'userSpaceOnUse';
  const gradientMatrix = parseTransform(attributes.gradientTransform);
  const toBaked = (gradientX, gradientY) => {
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
 * @param {string} svgString  The full SVG file contents.
 * @returns {Promise<{ops:number[], paints:number[], width:number, height:number}>} op-tape + intrinsic
 *          size (px). `ops`/`paints` are plain arrays ready to inline into the bundle; `width`/`height`
 *          are the SVG's rendered size for layout (a <Svg source> scales the tape to its box).
 */
export async function svgToVector(svgString) {
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

  const ops = [];
  const paints = [];
  const gradients = [];
  const gradientDefinitions = collectGradients(root);

  // Elements we intentionally skip without losing visible content (gradient defs are collected separately,
  // the rest are metadata). Anything else hitting the skip branch — text/image/use/mask/filter/pattern/… —
  // is unsupported VISUAL content, recorded in `dropped` so the loader can warn + fall back to raster.
  const SVG_IGNORABLE = new Set([
    'defs',
    'title',
    'desc',
    'metadata',
    'linearGradient',
    'radialGradient',
    'conicGradient',
  ]);
  const dropped = new Set();

  const walk = (node, matrix, paint) => {
    for (const child of node.children || []) {
      if (child.type !== 'element') continue;
      const childAttributes = child.attributes || {};
      const childMatrix = mul(
        matrix,
        parseTransform(childAttributes.transform),
      );
      const childPaint = resolvePaint(childAttributes, paint);

      if (child.name === 'g' || child.name === 'svg') {
        walk(child, childMatrix, childPaint);
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

      // A filter/mask/clip applied to an otherwise-bakeable shape still loses fidelity as a vector → flag it.
      for (const feature of ['filter', 'mask', 'clip-path']) {
        if (childAttributes[feature] && childAttributes[feature] !== 'none')
          dropped.add(feature);
      }
      // A dashed stroke can't be represented in the op-tape (the stroke is baked solid). Flag it so the SVG
      // falls back to raster (resvg renders the dashes) — but only when a visible stroke is actually dashed
      // by a positive pattern, so a stray stroke-dasharray on an unstroked/zero-width shape isn't penalized.
      const dash = childPaint['stroke-dasharray'];
      const stroked =
        childPaint.stroke &&
        childPaint.stroke !== 'none' &&
        num(childPaint['stroke-width'], DEFAULT_STROKE_WIDTH) > 0;
      if (stroked && dash && dash !== 'none' && /[1-9]/.test(dash))
        dropped.add('stroke-dasharray');

      const shapeOps = parsePath(d);
      if (!shapeOps.length) continue;

      const paintIndex = paints.length / PAINT_STRIDE;
      const opacity = clamp01(num(childPaint.opacity, 1));
      // A url(#id) fill/stroke referencing a parsed gradient bakes to a 1-based gradient-table index; the
      // solid color stays 0 (colorOf returns 0 for url()). The shape's bbox is computed once, lazily.
      let bbox = null;
      const gradientIndex = reference => {
        if (!reference) return 0;
        const definition = gradientDefinitions.get(reference);
        if (!definition || definition.stops.length < MIN_GRADIENT_STOPS)
          return 0;
        if (!bbox) bbox = opsBBox(shapeOps);
        gradients.push(bakeGradient(definition, childMatrix, bbox));
        return gradients.length; // 1-based
      };
      const fillGradient = gradientIndex(urlRef(childPaint.fill));
      const strokeGradient = gradientIndex(urlRef(childPaint.stroke));
      ops.push(VOP_SHAPE, paintIndex, ...transformOps(shapeOps, childMatrix));
      paints.push(
        applyAlpha(
          colorOf(childPaint.fill ?? 'black'),
          opacity * clamp01(num(childPaint['fill-opacity'], 1)),
        ),
        applyAlpha(
          colorOf(childPaint.stroke ?? 'none'),
          opacity * clamp01(num(childPaint['stroke-opacity'], 1)),
        ),
        num(childPaint['stroke-width'], DEFAULT_STROKE_WIDTH) *
          scaleOf(childMatrix),
        num(childPaint['stroke-miterlimit'], DEFAULT_MITER_LIMIT),
        CAP[childPaint['stroke-linecap']] ?? 0,
        JOIN[childPaint['stroke-linejoin']] ?? 0,
        childPaint['fill-rule'] === 'evenodd'
          ? FILL_RULE_EVEN_ODD
          : FILL_RULE_NONZERO,
        fillGradient,
        strokeGradient,
      );
    }
  };

  walk(root, rootMatrix, DEFAULT_PAINT);
  return {ops, paints, gradients, width, height, dropped: [...dropped].sort()};
}

/**
 * Rasterizes an SVG to an RGBA PNG at its intrinsic size — the raster fallback for SVGs that use features
 * the vector baker can't represent (text, masks, filters, patterns, …). resvg is
 * imported lazily, so vector-only builds never load it. The PNG flows through the normal image pipeline
 * (bake-image → asset pack), and a <Svg source> whose artifact is {kind:'raster'} renders it as an image.
 *
 * @param {string} svgString  The SVG document.
 * @returns {Promise<{width:number, height:number, png:Buffer}>}  Rendered size and PNG bytes.
 */
export async function svgToRaster(svgString) {
  const {Resvg} = await import('@resvg/resvg-js');
  // resvg (usvg) requires the SVG namespace; svgson (the vector baker) is lenient and many hand-authored
  // SVGs omit it, so inject it when absent or resvg rejects the document ("does not have a root node").
  let svg = String(svgString);
  if (!/<svg\b[^>]*\sxmlns\s*=/i.test(svg))
    svg = svg.replace(/<svg\b/i, '<svg xmlns="http://www.w3.org/2000/svg"');
  const resvg = new Resvg(svg, {background: 'rgba(0,0,0,0)'}); // transparent so it composites
  const rendered = resvg.render();
  const png = rendered.asPng();
  const {width, height} = rendered;
  rendered.free?.();
  return {width, height, png};
}

/**
 * Writes a rasterized-SVG PNG to a temp file and returns its path (content-hashed so rebuilds are stable).
 * The image pipeline (bakeImage) reads PNGs by path, so the raster fallback writes one here and registers it
 * as an asset. Shared by the Flow A loader and the Flow B (AOT) baker.
 *
 * @param {string} name   Asset name (the SVG's basename) — used in the filename.
 * @param {Buffer} png    PNG bytes from svgToRaster.
 * @returns {string}      Absolute path to the written .png.
 */
export function writeRasterPng(name, png) {
  const directory = join(tmpdir(), 'embedded-react-svg-raster');
  mkdirSync(directory, {recursive: true});
  const file = join(
    directory,
    `${name}-${createHash('sha1').update(png).digest('hex').slice(0, RASTER_HASH_LENGTH)}.png`,
  );
  writeFileSync(file, png);
  return file;
}
