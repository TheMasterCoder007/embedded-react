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

// Build-time SVG -> engine op-tape baker (Phase 3 Track A). Turns an .svg FILE into the same
// {ops, paints} op-tape an inline <Svg> compiles to, so `import logo from './logo.svg'` can render as a
// crisp on-device vector. Every transform (matrix/translate/rotate/scale/skew, nested) is BAKED into
// absolute path coordinates here, so the device needs no transform support — full SVG transform fidelity
// for free. svgson (the XML parser) is a build-time dependency only; it never reaches the device bundle.
//
// v1 scope: <path> + <rect>/<circle>/<ellipse>/<line>/<polygon>/<polyline> + <g> nesting + viewBox + solid
// fill/stroke (presentation attrs + inline style + inheritance). url() paints (gradients) bake to nothing
// for now (Track B adds engine gradients; Track C rasterizes the rest). Primitives are emitted as
// line/cubic paths (no native arc op) so an arbitrary matrix bakes cleanly.
import { parse } from 'svgson';
import {
  parsePath,
  parseColor,
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
} from '../src/embedded-react/svg-ops.js';

// --- 2D affine matrix [a, b, c, d, e, f]:  x' = a*x + c*y + e ;  y' = b*x + d*y + f ------------------
const IDENT = [1, 0, 0, 1, 0, 0];

/** Compose m1 ∘ m2 (apply m2 first, then m1). */
function mul(m1, m2) {
  const [a1, b1, c1, d1, e1, f1] = m1;
  const [a2, b2, c2, d2, e2, f2] = m2;
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
function parseTransform(str) {
  let m = IDENT;
  if (!str) return m;
  const re = /(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/g;
  let t;
  while ((t = re.exec(str)) !== null) {
    const fn = t[1];
    const p = t[2].split(/[\s,]+/).map(Number).filter((n) => !Number.isNaN(n));
    let lm = IDENT;
    if (fn === 'matrix') lm = [p[0] || 0, p[1] || 0, p[2] || 0, p[3] || 0, p[4] || 0, p[5] || 0];
    else if (fn === 'translate') lm = [1, 0, 0, 1, p[0] || 0, p[1] || 0];
    else if (fn === 'scale') lm = [p[0] ?? 1, 0, 0, p.length > 1 ? p[1] : (p[0] ?? 1), 0, 0];
    else if (fn === 'rotate') {
      const a = ((p[0] || 0) * Math.PI) / 180;
      const cos = Math.cos(a);
      const sin = Math.sin(a);
      const rot = [cos, sin, -sin, cos, 0, 0];
      // rotate(angle cx cy) rotates around a point.
      lm = p.length >= 3 ? mul(mul([1, 0, 0, 1, p[1], p[2]], rot), [1, 0, 0, 1, -p[1], -p[2]]) : rot;
    } else if (fn === 'skewX') lm = [1, 0, Math.tan(((p[0] || 0) * Math.PI) / 180), 1, 0, 0];
    else if (fn === 'skewY') lm = [1, Math.tan(((p[0] || 0) * Math.PI) / 180), 0, 1, 0, 0];
    m = mul(m, lm);
  }
  return m;
}

/** Applies matrix @p m to a point. */
function pt(m, x, y) {
  return [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];
}

/** Transforms a path op array (no leading SHAPE op) by matrix @p m. parsePath only emits MOVE/LINE/QUAD/
 *  CUBIC/CLOSE (it converts arcs to cubics), so every coordinate pair is a plain point — matrix-safe. */
function transformOps(ops, m) {
  const out = [];
  let i = 0;
  while (i < ops.length) {
    const op = ops[i++];
    out.push(op);
    if (op === VOP_MOVE || op === VOP_LINE) {
      const [x, y] = pt(m, ops[i++], ops[i++]);
      out.push(x, y);
    } else if (op === VOP_QUAD) {
      const [cx, cy] = pt(m, ops[i++], ops[i++]);
      const [x, y] = pt(m, ops[i++], ops[i++]);
      out.push(cx, cy, x, y);
    } else if (op === VOP_CUBIC) {
      const [c1x, c1y] = pt(m, ops[i++], ops[i++]);
      const [c2x, c2y] = pt(m, ops[i++], ops[i++]);
      const [x, y] = pt(m, ops[i++], ops[i++]);
      out.push(c1x, c1y, c2x, c2y, x, y);
    }
    // VOP_CLOSE has no args.
  }
  return out;
}

/** Uniform stroke-width scale factor for a matrix (geometric mean of the axis scales). */
function scaleOf(m) {
  return Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
}

function num(v, dflt = 0) {
  const n = parseFloat(v);
  return Number.isNaN(n) ? dflt : n;
}

// --- Primitive shapes -> path `d` (emitted as lines/arcs; parsePath turns arcs into cubics) ----------
function rectPath(a) {
  const x = num(a.x);
  const y = num(a.y);
  const w = num(a.width);
  const h = num(a.height);
  if (w <= 0 || h <= 0) return '';
  return `M${x} ${y}L${x + w} ${y}L${x + w} ${y + h}L${x} ${y + h}Z`;
}
function circlePath(a) {
  const cx = num(a.cx);
  const cy = num(a.cy);
  const r = num(a.r);
  if (r <= 0) return '';
  return `M${cx - r} ${cy}A${r} ${r} 0 1 0 ${cx + r} ${cy}A${r} ${r} 0 1 0 ${cx - r} ${cy}Z`;
}
function ellipsePath(a) {
  const cx = num(a.cx);
  const cy = num(a.cy);
  const rx = num(a.rx);
  const ry = num(a.ry);
  if (rx <= 0 || ry <= 0) return '';
  return `M${cx - rx} ${cy}A${rx} ${ry} 0 1 0 ${cx + rx} ${cy}A${rx} ${ry} 0 1 0 ${cx - rx} ${cy}Z`;
}
function linePath(a) {
  return `M${num(a.x1)} ${num(a.y1)}L${num(a.x2)} ${num(a.y2)}`;
}
function polyPath(a, close) {
  const v = String(a.points || '')
    .trim()
    .split(/[\s,]+/)
    .map(Number)
    .filter((n) => !Number.isNaN(n));
  if (v.length < 4) return '';
  let d = `M${v[0]} ${v[1]}`;
  for (let i = 2; i + 1 < v.length; i += 2) d += `L${v[i]} ${v[i + 1]}`;
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

function parseStyle(s) {
  const o = {};
  if (!s) return o;
  for (const decl of String(s).split(';')) {
    const i = decl.indexOf(':');
    if (i > 0) o[decl.slice(0, i).trim()] = decl.slice(i + 1).trim();
  }
  return o;
}

function resolvePaint(attrs, inherited) {
  const style = parseStyle(attrs.style);
  const out = { ...inherited };
  for (const k of PAINT_KEYS) {
    const v = style[k] ?? attrs[k];
    if (v != null) out[k] = v;
  }
  return out;
}

/** Resolves a CSS color to a uint32 ARGB. url(...) paints (gradients/patterns) bake to nothing in v1. */
function colorOf(v) {
  if (typeof v === 'string' && v.trim().startsWith('url(')) return 0;
  return parseColor(v);
}

function clamp01(v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/** Multiplies an ARGB color's alpha by @p f (SVG opacity / fill-opacity / stroke-opacity, baked in). */
function applyAlpha(argb, f) {
  if (f >= 1) return argb >>> 0;
  const a = Math.round(((argb >>> 24) & 0xff) * clamp01(f));
  return ((a << 24) | (argb & 0xffffff)) >>> 0;
}

// --- Gradients (<linearGradient> / <radialGradient>) -------------------------------------------------

/** Parses <stop>s into [{ color: uint32 (stop-opacity baked in), offset: 0..1 }], in authored order. */
function parseStops(node) {
  const out = [];
  for (const c of node.children || []) {
    if (c.type !== 'element' || c.name !== 'stop') continue;
    const a = c.attributes || {};
    const style = parseStyle(a.style);
    const col = style['stop-color'] ?? a['stop-color'] ?? 'black';
    const so = clamp01(num(style['stop-opacity'] ?? a['stop-opacity'], 1));
    let off = String(a.offset ?? '0').trim();
    off = off.endsWith('%') ? parseFloat(off) / 100 : parseFloat(off);
    out.push({ color: applyAlpha(parseColor(col), so), offset: clamp01(Number.isNaN(off) ? 0 : off) });
  }
  return out;
}

/** Collects every <linearGradient>/<radialGradient> by id (searched anywhere, including <defs>). */
function collectGradients(root) {
  const map = new Map();
  const visit = (node) => {
    for (const c of node.children || []) {
      if (c.type !== 'element') continue;
      if ((c.name === 'linearGradient' || c.name === 'radialGradient') && c.attributes && c.attributes.id)
        map.set(c.attributes.id, { name: c.name, attrs: c.attributes, stops: parseStops(c) });
      visit(c);
    }
  };
  visit(root);
  return map;
}

/** "url(#id)" -> "id" (else null). */
function urlRef(v) {
  const m = typeof v === 'string' && v.trim().match(/^url\(\s*#([^)\s]+)\s*\)/);
  return m ? m[1] : null;
}

/** A gradient coordinate: "50%" -> 0.5; a plain number passes through. */
function gradCoord(v, dflt) {
  if (v == null) return dflt;
  const s = String(v).trim();
  return s.endsWith('%') ? parseFloat(s) / 100 : parseFloat(s);
}

/** Local-space bounding box of a path-op array (no SHAPE header; MOVE/LINE/QUAD/CUBIC, CLOSE has no args). */
function opsBBox(ops) {
  let minx = Infinity;
  let miny = Infinity;
  let maxx = -Infinity;
  let maxy = -Infinity;
  let i = 0;
  while (i < ops.length) {
    const op = ops[i++];
    const n = op === VOP_MOVE || op === VOP_LINE ? 2 : op === VOP_QUAD ? 4 : op === VOP_CUBIC ? 6 : 0;
    for (let k = 0; k < n; k += 2) {
      const x = ops[i + k];
      const y = ops[i + k + 1];
      if (x < minx) minx = x;
      if (x > maxx) maxx = x;
      if (y < miny) miny = y;
      if (y > maxy) maxy = y;
    }
    i += n;
  }
  if (!Number.isFinite(minx)) return { x: 0, y: 0, w: 0, h: 0 };
  return { x: minx, y: miny, w: maxx - minx, h: maxy - miny };
}

/**
 * Bakes a gradient definition into an engine gradient descriptor in the path's BAKED coordinate space.
 * objectBoundingBox units (the SVG default) map [0,1] onto the shape's local bbox; userSpaceOnUse uses the
 * coords as-is. gradientTransform and the shape's cumulative matrix are both applied. The radial radius is
 * uniform (a non-square objectBoundingBox / non-uniform scale is approximated — a Track B follow-up).
 */
function bakeGradient(def, cm, bbox) {
  const a = def.attrs;
  const obb = (a.gradientUnits || 'objectBoundingBox') !== 'userSpaceOnUse';
  const gt = parseTransform(a.gradientTransform);
  const toBaked = (gx, gy) => {
    const ux = obb ? bbox.x + gx * bbox.w : gx;
    const uy = obb ? bbox.y + gy * bbox.h : gy;
    const [tx, ty] = pt(gt, ux, uy);
    return pt(cm, tx, ty);
  };
  if (def.name === 'radialGradient') {
    const [bcx, bcy] = toBaked(gradCoord(a.cx, 0.5), gradCoord(a.cy, 0.5));
    const rUser = (obb ? gradCoord(a.r, 0.5) * (Math.hypot(bbox.w, bbox.h) / Math.SQRT2) : gradCoord(a.r, 0.5));
    return { type: GRAD_RADIAL, stops: def.stops, ax: bcx, ay: bcy, bx: 0, by: 0, r: rUser * scaleOf(cm) * scaleOf(gt) };
  }
  const [ax, ay] = toBaked(gradCoord(a.x1, 0), gradCoord(a.y1, 0));
  const [bx, by] = toBaked(gradCoord(a.x2, 1), gradCoord(a.y2, 0));
  return { type: GRAD_LINEAR, stops: def.stops, ax, ay, bx, by, r: 0 };
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
  const a = root.attributes || {};

  let vb = String(a.viewBox || '').trim().split(/[\s,]+/).map(Number);
  if (vb.length !== 4 || vb.some((n) => Number.isNaN(n))) vb = [0, 0, num(a.width, 100), num(a.height, 100)];
  const [vx, vy, vw, vh] = vb;
  const width = num(a.width, vw);
  const height = num(a.height, vh);

  // Root transform: map the viewBox user-units onto the intrinsic px box (like the inline <Svg> path).
  const sx = vw ? width / vw : 1;
  const sy = vh ? height / vh : 1;
  const rootM = mul([sx, 0, 0, sy, -vx * sx, -vy * sy], parseTransform(a.transform));

  const ops = [];
  const paints = [];
  const gradients = [];
  const gradDefs = collectGradients(root);

  const walk = (node, m, paint) => {
    for (const child of node.children || []) {
      if (child.type !== 'element') continue;
      const attrs = child.attributes || {};
      const cm = mul(m, parseTransform(attrs.transform));
      const cp = resolvePaint(attrs, paint);

      if (child.name === 'g' || child.name === 'svg') {
        walk(child, cm, cp);
        continue;
      }

      let d = '';
      if (child.name === 'path') d = attrs.d || '';
      else if (child.name === 'rect') d = rectPath(attrs);
      else if (child.name === 'circle') d = circlePath(attrs);
      else if (child.name === 'ellipse') d = ellipsePath(attrs);
      else if (child.name === 'line') d = linePath(attrs);
      else if (child.name === 'polygon') d = polyPath(attrs, true);
      else if (child.name === 'polyline') d = polyPath(attrs, false);
      else continue; // defs / use / text / style / mask / ... -> not vector-baked (raster fallback later)
      if (!d) continue;

      const shapeOps = parsePath(d);
      if (!shapeOps.length) continue;

      const paintIndex = paints.length / PAINT_STRIDE;
      const op = clamp01(num(cp.opacity, 1));
      // A url(#id) fill/stroke referencing a parsed gradient bakes to a 1-based gradient-table index; the
      // solid color stays 0 (colorOf returns 0 for url()). The shape's bbox is computed once, lazily.
      let bbox = null;
      const refGrad = (ref) => {
        if (!ref) return 0;
        const def = gradDefs.get(ref);
        if (!def || def.stops.length < 2) return 0;
        if (!bbox) bbox = opsBBox(shapeOps);
        gradients.push(bakeGradient(def, cm, bbox));
        return gradients.length; // 1-based
      };
      const fillGrad = refGrad(urlRef(cp.fill));
      const strokeGrad = refGrad(urlRef(cp.stroke));
      ops.push(VOP_SHAPE, paintIndex, ...transformOps(shapeOps, cm));
      paints.push(
        applyAlpha(colorOf(cp.fill ?? 'black'), op * clamp01(num(cp['fill-opacity'], 1))),
        applyAlpha(colorOf(cp.stroke ?? 'none'), op * clamp01(num(cp['stroke-opacity'], 1))),
        num(cp['stroke-width'], 1) * scaleOf(cm),
        num(cp['stroke-miterlimit'], 4),
        CAP[cp['stroke-linecap']] ?? 0,
        JOIN[cp['stroke-linejoin']] ?? 0,
        cp['fill-rule'] === 'evenodd' ? 1 : 0,
        fillGrad,
        strokeGrad
      );
    }
  };

  walk(root, rootM, DEFAULT_PAINT);
  return { ops, paints, gradients, width, height };
}
