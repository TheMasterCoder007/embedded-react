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
// ER_VOP_* contract in er_scene.h). This module compiles a declarative <Svg> subtree (Path/Circle/
// Rect/Line/Ellipse/G elements) into that form: it parses SVG `d` path data, converts primitive
// shapes to path ops, bakes viewBox + <G> translate/scale into coordinates, and resolves inherited
// paint. The opcodes below MUST stay in sync with er_scene.h.

// Op-tape vocabulary — exported so the build-time SVG baker (assets/bake-svg.mjs) can reuse it without
// duplicating the encoding (and without pulling its SVG-parser dependency into this runtime module).
export const VOP_SHAPE = 0;
export const VOP_MOVE = 1;
export const VOP_LINE = 2;
export const VOP_QUAD = 3;
export const VOP_CUBIC = 4;
export const VOP_ARC = 5;
export const VOP_CLOSE = 6;

export const CAP = { butt: 0, round: 1, square: 2 };
export const JOIN = { miter: 0, round: 1, bevel: 2 };

// Paint record width in the flat paint table: [fill, stroke, strokeWidth, miter, cap, join, fillRule,
// fillGrad, strokeGrad]. fillGrad/strokeGrad are 1-based indices into the gradient table (0 = solid). MUST
// match the bridge's VEC_PAINT_STRIDE (native_ui_bridge.c) and the AOT's paint emission.
export const PAINT_STRIDE = 9;

// Gradient table encoding — a gradient descriptor is { type, stops: [{color, offset}], ax, ay, bx, by, r }.
// type 1 = linear (axis (ax,ay)->(bx,by)), 2 = radial (centre (ax,ay), radius r). The flat float record is
// [type, stopCount, (color, offset) × GRAD_MAX_STOPS, ax, ay, bx, by, r]; MUST match the bridge's
// VEC_GRAD_STRIDE and the engine's ER_VGRAD_MAX_STOPS.
export const GRAD_MAX_STOPS = 8; // MUST match the engine's ER_VGRAD_MAX_STOPS (er_scene.h)
export const GRAD_STRIDE = 2 + GRAD_MAX_STOPS * 2 + 5; // 23
export const GRAD_LINEAR = 1;
export const GRAD_RADIAL = 2;
export const GRAD_CONIC = 3;

/** Linearly interpolates two straight-alpha ARGB8888 colors, per channel. */
function lerpArgb(c0, c1, t) {
  const lp = (sh) => {
    const a = (c0 >>> sh) & 0xff;
    const b = (c1 >>> sh) & 0xff;
    return Math.round(a + (b - a) * t) & 0xff;
  };
  return ((lp(24) << 24) | (lp(16) << 16) | (lp(8) << 8) | lp(0)) >>> 0;
}

/** Gradient color at position t (stops ascending by offset; clamps to the endpoint colors). */
function evalStopsAt(stops, t) {
  if (!stops.length) return 0;
  if (t <= stops[0].offset) return stops[0].color >>> 0;
  const last = stops[stops.length - 1];
  if (t >= last.offset) return last.color >>> 0;
  for (let i = 0; i < stops.length - 1; i++) {
    const a = stops[i];
    const b = stops[i + 1];
    if (t <= b.offset) {
      const span = b.offset - a.offset;
      return lerpArgb(a.color >>> 0, b.color >>> 0, span > 0 ? (t - a.offset) / span : 0);
    }
  }
  return last.color >>> 0;
}

/** Reduces a stop list to at most `max` entries, resampling evenly across the stop range when it's over. */
function capStops(stops, max) {
  if (stops.length <= max) return stops;
  const lo = stops[0].offset;
  const hi = stops[stops.length - 1].offset;
  const out = [];
  for (let i = 0; i < max; i++) {
    const t = max === 1 ? lo : lo + (hi - lo) * (i / (max - 1));
    out.push({ color: evalStopsAt(stops, t), offset: t });
  }
  return out;
}

/** Encodes gradient descriptors into the flat float array NativeUI.setVectorOps takes (GRAD_STRIDE/gradient).
 *  A gradient with more than GRAD_MAX_STOPS stops is resampled down (not truncated) so it still ramps right. */
export function encodeVectorGradients(grads) {
  const out = [];
  if (!grads) return out;
  for (const g of grads) {
    const stops = capStops(g.stops || [], GRAD_MAX_STOPS);
    out.push(g.type, stops.length);
    for (let s = 0; s < GRAD_MAX_STOPS; s++) {
      const st = stops[s];
      out.push(st ? st.color >>> 0 : 0, st ? st.offset : 0);
    }
    out.push(g.ax || 0, g.ay || 0, g.bx || 0, g.by || 0, g.r || 0);
  }
  return out;
}

// Minimal named-color set (extend as needed); everything else goes through hex/rgb parsing.
const NAMED = {
  none: 0,
  transparent: 0,
  black: 0xff000000,
  white: 0xffffffff,
  red: 0xffff0000,
  green: 0xff008000,
  blue: 0xff0000ff,
  gray: 0xff808080,
  grey: 0xff808080,
};

// Cache string->uint32 results: a theme reuses a handful of color strings, so on a continuous drag
// (which recompiles the dial every move) this turns repeated hex parsing into a Map lookup.
const _colorCache = new Map();

/**
 * Parses a CSS color string to a uint32 ARGB8888. Returns 0 (fully transparent) for none/transparent/
 * null, so the engine treats it as "no fill"/"no stroke". Accepts #rgb, #rgba, #rrggbb, #rrggbbaa,
 * rgb()/rgba(), and the small named set above. String results are memoised (see _colorCache).
 */
export function parseColor(c) {
  if (c == null || c === false) return 0;
  if (typeof c === 'number') return c >>> 0;
  const cached = _colorCache.get(c);
  if (cached !== undefined) return cached;
  const v = parseColorString(c);
  _colorCache.set(c, v);
  return v;
}

function parseColorString(c) {
  let s = String(c).trim().toLowerCase();
  if (s in NAMED) return NAMED[s] >>> 0;
  if (s[0] === '#') {
    s = s.slice(1);
    let r, g, b, a = 255;
    if (s.length === 3 || s.length === 4) {
      r = parseInt(s[0] + s[0], 16);
      g = parseInt(s[1] + s[1], 16);
      b = parseInt(s[2] + s[2], 16);
      if (s.length === 4) a = parseInt(s[3] + s[3], 16);
    } else if (s.length === 6 || s.length === 8) {
      r = parseInt(s.slice(0, 2), 16);
      g = parseInt(s.slice(2, 4), 16);
      b = parseInt(s.slice(4, 6), 16);
      if (s.length === 8) a = parseInt(s.slice(6, 8), 16);
    } else {
      return 0xff000000;
    }
    return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0;
  }
  if (s.startsWith('rgb')) {
    const m = s.match(/rgba?\(([^)]+)\)/);
    if (m) {
      const p = m[1].split(',').map((x) => x.trim());
      const r = parseInt(p[0], 10) || 0;
      const g = parseInt(p[1], 10) || 0;
      const b = parseInt(p[2], 10) || 0;
      const a = p[3] != null ? Math.round(parseFloat(p[3]) * 255) : 255;
      return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0;
    }
  }
  return 0xff000000; // unknown -> opaque black (SVG-ish default)
}

// --- SVG `d` path-data parser -------------------------------------------------------------------

// Hoisted so the pattern is compiled once at module load, not on every parsePath call. The /g lastIndex is reset below.
const PATH_RE = /([MmLlHhVvCcSsQqTtAaZz])|(-?\d*\.?\d+(?:[eE][-+]?\d+)?)/g;

/** Tokenizes a `d` string into {cmd, args[]} groups (one per command letter). */
function tokenizePath(d) {
  const out = [];
  const re = PATH_RE;
  re.lastIndex = 0;
  let m;
  let cur = null;
  while ((m = re.exec(d)) !== null) {
    if (m[1]) {
      cur = { cmd: m[1], args: [] };
      out.push(cur);
    } else if (cur) {
      cur.args.push(parseFloat(m[2]));
    }
  }
  return out;
}

/** Converts an SVG elliptical arc (endpoint form) to a list of cubic bezier segments. */
function arcToCubics(x0, y0, rx, ry, phiDeg, largeArc, sweep, x, y) {
  const out = [];
  if (rx === 0 || ry === 0) {
    out.push([x0, y0, x, y, x, y]); // degenerate -> straight line as a cubic
    return out;
  }
  rx = Math.abs(rx);
  ry = Math.abs(ry);
  const phi = (phiDeg * Math.PI) / 180;
  const cosP = Math.cos(phi);
  const sinP = Math.sin(phi);
  const dx = (x0 - x) / 2;
  const dy = (y0 - y) / 2;
  const x1p = cosP * dx + sinP * dy;
  const y1p = -sinP * dx + cosP * dy;
  let rxs = rx * rx;
  let rys = ry * ry;
  const x1ps = x1p * x1p;
  const y1ps = y1p * y1p;
  // Correct out-of-range radii.
  const lam = x1ps / rxs + y1ps / rys;
  if (lam > 1) {
    const s = Math.sqrt(lam);
    rx *= s;
    ry *= s;
    rxs = rx * rx;
    rys = ry * ry;
  }
  let sign = largeArc !== sweep ? 1 : -1;
  let num = rxs * rys - rxs * y1ps - rys * x1ps;
  if (num < 0) num = 0;
  const co = sign * Math.sqrt(num / (rxs * y1ps + rys * x1ps || 1));
  const cxp = (co * rx * y1p) / ry;
  const cyp = (-co * ry * x1p) / rx;
  const cx = cosP * cxp - sinP * cyp + (x0 + x) / 2;
  const cy = sinP * cxp + cosP * cyp + (y0 + y) / 2;
  const ang = (ux, uy, vx, vy) => {
    const dot = ux * vx + uy * vy;
    const len = Math.sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy)) || 1;
    let a = Math.acos(Math.max(-1, Math.min(1, dot / len)));
    if (ux * vy - uy * vx < 0) a = -a;
    return a;
  };
  const theta1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
  let dtheta = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
  if (!sweep && dtheta > 0) dtheta -= 2 * Math.PI;
  if (sweep && dtheta < 0) dtheta += 2 * Math.PI;
  const segs = Math.ceil(Math.abs(dtheta) / (Math.PI / 2));
  const delta = dtheta / segs;
  const t = (4 / 3) * Math.tan(delta / 4);
  let th = theta1;
  let px = x0;
  let py = y0;
  for (let i = 0; i < segs; i++) {
    const th2 = th + delta;
    const cos1 = Math.cos(th);
    const sin1 = Math.sin(th);
    const cos2 = Math.cos(th2);
    const sin2 = Math.sin(th2);
    const e1x = cx + rx * cosP * cos1 - ry * sinP * sin1;
    const e1y = cy + rx * sinP * cos1 + ry * cosP * sin1;
    const e2x = cx + rx * cosP * cos2 - ry * sinP * sin2;
    const e2y = cy + rx * sinP * cos2 + ry * cosP * sin2;
    const d1x = -rx * cosP * sin1 - ry * sinP * cos1;
    const d1y = -rx * sinP * sin1 + ry * cosP * cos1;
    const d2x = -rx * cosP * sin2 - ry * sinP * cos2;
    const d2y = -rx * sinP * sin2 + ry * cosP * cos2;
    out.push([px + t * d1x, py + t * d1y, e2x - t * d2x, e2y - t * d2y, e2x, e2y]);
    px = e2x;
    py = e2y;
    th = th2;
  }
  return out;
}

/**
 * Parses an SVG `d` string into engine path ops (a flat number array, NO leading SHAPE op). Supports
 * M/L/H/V/C/S/Q/T/A/Z and their relative (lowercase) forms; arcs are converted to cubics.
 */
export function parsePath(d) {
  const ops = [];
  const toks = tokenizePath(d);
  let cx = 0;
  let cy = 0;
  let sx = 0;
  let sy = 0; // subpath start
  let prevCtrlX = 0;
  let prevCtrlY = 0;
  let prevCmd = '';
  for (const tk of toks) {
    const C = tk.cmd;
    const rel = C >= 'a' && C <= 'z';
    const a = tk.args;
    const U = C.toUpperCase();
    let i = 0;
    const rx = (v) => (rel ? cx + v : v);
    const ry = (v) => (rel ? cy + v : v);
    if (U === 'M') {
      // First pair is moveto; later pairs are implicit linetos.
      cx = rx(a[i++]);
      cy = ry(a[i++]);
      sx = cx;
      sy = cy;
      ops.push(VOP_MOVE, cx, cy);
      while (i + 1 < a.length) {
        cx = rel ? cx + a[i++] : a[i++];
        cy = rel ? cy + a[i++] : a[i++];
        ops.push(VOP_LINE, cx, cy);
      }
    } else if (U === 'L') {
      while (i + 1 < a.length) {
        cx = rel ? cx + a[i++] : a[i++];
        cy = rel ? cy + a[i++] : a[i++];
        ops.push(VOP_LINE, cx, cy);
      }
    } else if (U === 'H') {
      while (i < a.length) {
        cx = rel ? cx + a[i++] : a[i++];
        ops.push(VOP_LINE, cx, cy);
      }
    } else if (U === 'V') {
      while (i < a.length) {
        cy = rel ? cy + a[i++] : a[i++];
        ops.push(VOP_LINE, cx, cy);
      }
    } else if (U === 'C') {
      while (i + 5 < a.length) {
        const c1x = rx(a[i++]);
        const c1y = ry(a[i++]);
        const c2x = rx(a[i++]);
        const c2y = ry(a[i++]);
        const ex = rx(a[i++]);
        const ey = ry(a[i++]);
        ops.push(VOP_CUBIC, c1x, c1y, c2x, c2y, ex, ey);
        prevCtrlX = c2x;
        prevCtrlY = c2y;
        cx = ex;
        cy = ey;
      }
    } else if (U === 'S') {
      while (i + 3 < a.length) {
        const refl = prevCmd === 'C' || prevCmd === 'S';
        const c1x = refl ? 2 * cx - prevCtrlX : cx;
        const c1y = refl ? 2 * cy - prevCtrlY : cy;
        const c2x = rx(a[i++]);
        const c2y = ry(a[i++]);
        const ex = rx(a[i++]);
        const ey = ry(a[i++]);
        ops.push(VOP_CUBIC, c1x, c1y, c2x, c2y, ex, ey);
        prevCtrlX = c2x;
        prevCtrlY = c2y;
        cx = ex;
        cy = ey;
      }
    } else if (U === 'Q') {
      while (i + 3 < a.length) {
        const qx = rx(a[i++]);
        const qy = ry(a[i++]);
        const ex = rx(a[i++]);
        const ey = ry(a[i++]);
        ops.push(VOP_QUAD, qx, qy, ex, ey);
        prevCtrlX = qx;
        prevCtrlY = qy;
        cx = ex;
        cy = ey;
      }
    } else if (U === 'T') {
      while (i + 1 < a.length) {
        const refl = prevCmd === 'Q' || prevCmd === 'T';
        const qx = refl ? 2 * cx - prevCtrlX : cx;
        const qy = refl ? 2 * cy - prevCtrlY : cy;
        const ex = rx(a[i++]);
        const ey = ry(a[i++]);
        ops.push(VOP_QUAD, qx, qy, ex, ey);
        prevCtrlX = qx;
        prevCtrlY = qy;
        cx = ex;
        cy = ey;
      }
    } else if (U === 'A') {
      while (i + 6 < a.length) {
        const arx = a[i++];
        const ary = a[i++];
        const rot = a[i++];
        const laf = a[i++];
        const swf = a[i++];
        const ex = rx(a[i++]);
        const ey = ry(a[i++]);
        const cubics = arcToCubics(cx, cy, arx, ary, rot, laf, swf, ex, ey);
        for (const c of cubics) ops.push(VOP_CUBIC, c[0], c[1], c[2], c[3], c[4], c[5]);
        cx = ex;
        cy = ey;
      }
    } else if (U === 'Z') {
      ops.push(VOP_CLOSE);
      cx = sx;
      cy = sy;
    }
    prevCmd = U;
  }
  return ops;
}

// --- Primitive shapes -> path ops ----------------------------------------------------------------

function num(v, dflt) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isNaN(n) ? dflt : n;
}

function circleOps(p) {
  const cx = num(p.cx, 0);
  const cy = num(p.cy, 0);
  const r = num(p.r, 0);
  // Start at angle 0, full circular arc.
  return [VOP_MOVE, cx + r, cy, VOP_ARC, cx, cy, r, 0, 2 * Math.PI, 0, VOP_CLOSE];
}

function ellipseOps(p) {
  // Approximate via an arc-free path: four cubic beziers (kappa) — reuse parsePath by emitting an A path.
  const cx = num(p.cx, 0);
  const cy = num(p.cy, 0);
  const rx = num(p.rx, 0);
  const ry = num(p.ry, 0);
  const d = `M ${cx - rx} ${cy} A ${rx} ${ry} 0 1 0 ${cx + rx} ${cy} A ${rx} ${ry} 0 1 0 ${cx - rx} ${cy} Z`;
  return parsePath(d);
}

function rectOps(p) {
  const x = num(p.x, 0);
  const y = num(p.y, 0);
  const w = num(p.width, 0);
  const h = num(p.height, 0);
  return [VOP_MOVE, x, y, VOP_LINE, x + w, y, VOP_LINE, x + w, y + h, VOP_LINE, x, y + h, VOP_CLOSE];
}

function lineOps(p) {
  return [VOP_MOVE, num(p.x1, 0), num(p.y1, 0), VOP_LINE, num(p.x2, 0), num(p.y2, 0)];
}

// Arc convenience: angles in DEGREES, clockwise from 12 o'clock (the gauge/dial convention). Emits a
// native VOP_ARC (no d-string regex / bezier conversion), so it is cheap enough to rebuild every drag
// frame — the key to fast imperative updates.
function arcOpsCW(cx, cy, r, a0deg, a1deg) {
  // The engine's arc angle runs from +X (cos/sin); top-clockwise => subtract 90°.
  const a0 = ((a0deg - 90) * Math.PI) / 180;
  const a1 = ((a1deg - 90) * Math.PI) / 180;
  return [VOP_MOVE, cx + r * Math.cos(a0), cy + r * Math.sin(a0), VOP_ARC, cx, cy, r, a0, a1, 0];
}

// --- Imperative shapes -> op-tape (the fast path; no JSX, no React) -------------------------------

/**
 * Compiles a flat array of primitive shape descriptors into {ops, paints} for NativeUI.setVectorOps.
 * Each descriptor names one primitive plus paint fields, e.g.:
 *   { arc: [cx, cy, r, startDeg, endDeg], stroke: '#f4a261', strokeWidth: 14, cap: 'round' }
 *   { circle: [cx, cy, r], fill: '#16202f', stroke: '#f4a261', strokeWidth: 3 }
 *   { line: [x1, y1, x2, y2], stroke, strokeWidth }
 *   { rect: [x, y, w, h], fill }
 *   { path: 'M..A..', stroke }   // d-string supported but slower (regex + bezier)
 * Arc angles are degrees, clockwise from 12 o'clock. This avoids the d-string parser entirely, so it's
 * cheap enough to call on every pointer move (the imperative drag path).
 */
// Reused output buffers — shapesToVector is on the drag hot path and its result is consumed
// synchronously by NativeUI.setVectorOps, so growing fresh arrays (plus per-shape arrays + spreads)
// every move is pure GC. Push directly into these instead.
const _vecOps = [];
const _vecPaints = [];

export function shapesToVector(shapes) {
  const ops = _vecOps;
  const paints = _vecPaints;
  ops.length = 0;
  paints.length = 0;
  for (let si = 0; si < shapes.length; si++) {
    const s = shapes[si];
    const opStart = ops.length;
    const paintIndex = paints.length / PAINT_STRIDE;
    ops.push(VOP_SHAPE, paintIndex);
    if (s.arc) {
      const cx = s.arc[0];
      const cy = s.arc[1];
      const r = s.arc[2];
      const a0 = ((s.arc[3] - 90) * Math.PI) / 180;
      const a1 = ((s.arc[4] - 90) * Math.PI) / 180;
      ops.push(VOP_MOVE, cx + r * Math.cos(a0), cy + r * Math.sin(a0), VOP_ARC, cx, cy, r, a0, a1, 0);
    } else if (s.circle) {
      const cx = s.circle[0];
      const cy = s.circle[1];
      const r = s.circle[2];
      ops.push(VOP_MOVE, cx + r, cy, VOP_ARC, cx, cy, r, 0, 2 * Math.PI, 0, VOP_CLOSE);
    } else if (s.rect) {
      const x = s.rect[0];
      const y = s.rect[1];
      const w = s.rect[2];
      const h = s.rect[3];
      ops.push(VOP_MOVE, x, y, VOP_LINE, x + w, y, VOP_LINE, x + w, y + h, VOP_LINE, x, y + h, VOP_CLOSE);
    } else if (s.line) {
      ops.push(VOP_MOVE, s.line[0], s.line[1], VOP_LINE, s.line[2], s.line[3]);
    } else if (s.path) {
      const so = parsePath(s.path);
      for (let k = 0; k < so.length; k++) ops.push(so[k]);
    }
    if (ops.length <= opStart + 2) {
      ops.length = opStart; // nothing emitted — roll back the SHAPE header
      continue;
    }
    paints.push(
      parseColor(s.fill),
      parseColor(s.stroke),
      num(s.strokeWidth, 1),
      num(s.miter, 4),
      CAP[s.cap] ?? 0,
      JOIN[s.join] ?? 0,
      s.fillRule === 'evenodd' ? 1 : 0,
      0, // fill_grad
      0 // stroke_grad — the imperative path has no gradients
    );
  }
  return { ops, paints };
}

// --- Flatten an <Svg> subtree --------------------------------------------------------------------

const PAINT_DEFAULT = {
  fill: 'black',
  stroke: 'none',
  strokeWidth: 1,
  strokeLinecap: 'butt',
  strokeLinejoin: 'miter',
  strokeMiterlimit: 4,
  fillRule: 'nonzero',
};

function mergePaint(base, props) {
  const out = { ...base };
  for (const k of Object.keys(PAINT_DEFAULT)) if (props[k] != null) out[k] = props[k];
  return out;
}

/** Maps the resolved paint to the PAINT_STRIDE-number record [fill,stroke,w,miter,cap,join,rule,fillGrad]. */
function paintRecord(paint, scale) {
  return [
    parseColor(paint.fill),
    parseColor(paint.stroke),
    num(paint.strokeWidth, 1) * scale,
    num(paint.strokeMiterlimit, 4),
    CAP[paint.strokeLinecap] ?? 0,
    JOIN[paint.strokeLinejoin] ?? 0,
    paint.fillRule === 'evenodd' ? 1 : 0,
    0, // fill_grad: inline <Svg> children don't reference gradients (baked <Svg source> does)
    0, // stroke_grad
  ];
}

/** Applies a {sx,sy,tx,ty} transform in place to a path-op array (no leading SHAPE op). */
function transformOps(ops, T) {
  const out = [];
  let i = 0;
  const ax = (v) => v * T.sx + T.tx;
  const ay = (v) => v * T.sy + T.ty;
  while (i < ops.length) {
    const op = ops[i++];
    out.push(op);
    if (op === VOP_MOVE || op === VOP_LINE) {
      out.push(ax(ops[i++]), ay(ops[i++]));
    } else if (op === VOP_QUAD) {
      out.push(ax(ops[i++]), ay(ops[i++]), ax(ops[i++]), ay(ops[i++]));
    } else if (op === VOP_CUBIC) {
      out.push(ax(ops[i++]), ay(ops[i++]), ax(ops[i++]), ay(ops[i++]), ax(ops[i++]), ay(ops[i++]));
    } else if (op === VOP_ARC) {
      // center + radius scale (uniform scale assumed for arcs)
      out.push(ax(ops[i++]), ay(ops[i++]), ops[i++] * T.sx, ops[i++], ops[i++], ops[i++]);
    }
    // VOP_CLOSE has no args
  }
  return out;
}

function asArray(children) {
  if (children == null || children === false || children === true) return [];
  return Array.isArray(children) ? children : [children];
}

function isElement(c) {
  return c && typeof c === 'object' && c.type != null && c.props != null;
}

/**
 * Flattens an <Svg> element's props into {ops, paints} flat number arrays for NativeUI.setVectorOps.
 * Handles Path/Circle/Ellipse/Rect/Line shapes, <G> grouping (inherited paint + translate/scale), and
 * a root viewBox -> width/height scale. Returns empty arrays when there is nothing to draw.
 */
export function flattenSvg(props) {
  const ops = [];
  const paints = [];

  // Root transform from viewBox vs width/height.
  let root = { sx: 1, sy: 1, tx: 0, ty: 0 };
  if (props.viewBox && props.width && props.height) {
    const vb = String(props.viewBox).trim().split(/[\s,]+/).map(parseFloat);
    if (vb.length === 4 && vb[2] > 0 && vb[3] > 0) {
      const sx = num(props.width, vb[2]) / vb[2];
      const sy = num(props.height, vb[3]) / vb[3];
      root = { sx, sy, tx: -vb[0] * sx, ty: -vb[1] * sy };
    }
  }

  const walk = (child, paint, T) => {
    for (const c of asArray(child)) {
      if (!isElement(c)) continue;
      const p = c.props || {};
      const merged = mergePaint(paint, p);
      if (c.type === 'G') {
        // Compose a translate/scale for the group, then recurse.
        const s = num(p.scale, 1);
        const gx = num(p.x ?? p.translateX, 0);
        const gy = num(p.y ?? p.translateY, 0);
        const childT = { sx: T.sx * s, sy: T.sy * s, tx: gx * T.sx + T.tx, ty: gy * T.sy + T.ty };
        walk(p.children, merged, childT);
        continue;
      }
      let shapeOps = null;
      if (c.type === 'Path' && p.d) shapeOps = parsePath(p.d);
      else if (c.type === 'Circle') shapeOps = circleOps(p);
      else if (c.type === 'Ellipse') shapeOps = ellipseOps(p);
      else if (c.type === 'Rect') shapeOps = rectOps(p);
      else if (c.type === 'Line') shapeOps = lineOps(p);
      else if (c.type === 'Arc')
        shapeOps = arcOpsCW(num(p.cx, 0), num(p.cy, 0), num(p.r, 0), num(p.startAngle, 0), num(p.endAngle, 0));
      if (!shapeOps || shapeOps.length === 0) continue;

      const paintIndex = paints.length / PAINT_STRIDE;
      const scale = (T.sx + T.sy) / 2; // stroke-width scale (uniform assumed)
      paints.push(...paintRecord(merged, scale));
      ops.push(VOP_SHAPE, paintIndex, ...transformOps(shapeOps, T));
    }
  };

  walk(props.children, PAINT_DEFAULT, root);
  return { ops, paints };
}

// --- Imported SVG artifacts (<Svg source>) -------------------------------------------------------

/** Scales a full op-tape (with SHAPE headers) by (sx, sy): coordinates only, paint indices untouched. */
function scaleTape(ops, sx, sy) {
  const out = [];
  let i = 0;
  while (i < ops.length) {
    const op = ops[i++];
    out.push(op);
    if (op === VOP_SHAPE) out.push(ops[i++]); // paint index — copy, do not scale
    else if (op === VOP_MOVE || op === VOP_LINE) out.push(ops[i++] * sx, ops[i++] * sy);
    else if (op === VOP_QUAD) out.push(ops[i++] * sx, ops[i++] * sy, ops[i++] * sx, ops[i++] * sy);
    else if (op === VOP_CUBIC)
      out.push(ops[i++] * sx, ops[i++] * sy, ops[i++] * sx, ops[i++] * sy, ops[i++] * sx, ops[i++] * sy);
    else if (op === VOP_ARC) out.push(ops[i++] * sx, ops[i++] * sy, ops[i++] * sx, ops[i++], ops[i++], ops[i++]);
    // VOP_CLOSE has no args.
  }
  return out;
}

/**
 * Scales an imported vector artifact ({ops, paints, width, height} from the .svg baker) from its intrinsic
 * size to a target box, also scaling stroke widths. Returns the original arrays untouched when no scaling
 * is needed (the common <Svg source> case where the box equals the intrinsic size).
 */
export function scaleVectorArtifact(art, targetW, targetH) {
  const sx = art.width ? targetW / art.width : 1;
  const sy = art.height ? targetH / art.height : 1;
  const grads = art.gradients || [];
  if (sx === 1 && sy === 1) return { ops: art.ops, paints: art.paints, gradients: grads };
  const sw = Math.sqrt(Math.abs(sx * sy)) || 1; // uniform stroke-width / radius scale
  const paints = art.paints.slice();
  for (let k = 2; k < paints.length; k += PAINT_STRIDE) paints[k] *= sw; // index 2 of each record = strokeWidth
  // Gradient geometry lives in the same coordinate space as the path, so it scales with it (radius uniformly).
  const gradients = grads.map((g) => ({
    ...g,
    ax: (g.ax || 0) * sx,
    ay: (g.ay || 0) * sy,
    bx: (g.bx || 0) * sx,
    by: (g.by || 0) * sy,
    // Radial r is a length (scales); conic r is a start angle (invariant under uniform scale) — don't scale it.
    r: g.type === GRAD_CONIC ? g.r || 0 : (g.r || 0) * sw,
  }));
  return { ops: scaleTape(art.ops, sx, sy), paints, gradients };
}

// --- Bridge-cap diagnostics ----------------------------------------------------------------------

let _warnedVecOps = false;
let _warnedVecPaints = false;
let _warnedVecGrads = false;

/**
 * Warns (once per kind) when an <Svg>'s compiled geometry exceeds the native bridge's caps and will be
 * SILENTLY TRUNCATED. The bridge (native_ui_bridge.c) copies the op-tape into a fixed buffer and caps the
 * paint table; past the cap, geometry/shapes are dropped. The engine has its own pool diagnostics (on
 * stderr, debug builds) for what happens further in; this surfaces the JS->engine boundary truncation in
 * the developer's console. Pass the live caps from NativeUI.maxVectorOps / maxVectorPaints / maxVectorGrads
 * (undefined on an older bridge => no-op).
 *
 * @param {number} opsLen     Op-tape length (flat float count).
 * @param {number} paintsLen  Paint-table length (PAINT_STRIDE numbers per shape).
 * @param {number} maxOps     Bridge op-tape cap (NativeUI.maxVectorOps).
 * @param {number} maxPaints  Bridge paint cap, in SHAPES (NativeUI.maxVectorPaints).
 * @param {number} [gradsLen] Gradient count (number of gradients referenced by the shapes).
 * @param {number} [maxGrads] Bridge gradient cap (NativeUI.maxVectorGrads).
 */
export function warnVectorCaps(opsLen, paintsLen, maxOps, maxPaints, gradsLen, maxGrads) {
  if (!_warnedVecOps && maxOps > 0 && opsLen > maxOps) {
    _warnedVecOps = true;
    console.warn(
      `embedded-react: an <Svg> op-tape is too long (${opsLen} > ${maxOps} floats) and will be truncated — ` +
        `the shape gets cut off. Simplify the path (fewer/coarser curves) or split it across <Svg> nodes; ` +
        `raising the limit needs VEC_BRIDGE_MAX_OPS + ERUI_VECTOR_TAPE_MAX.`
    );
  }
  const shapes = (paintsLen / PAINT_STRIDE) | 0;
  if (!_warnedVecPaints && maxPaints > 0 && shapes > maxPaints) {
    _warnedVecPaints = true;
    console.warn(
      `embedded-react: an <Svg> has ${shapes} shapes (> ${maxPaints}) — the extra shapes won't render. ` +
        `Split them across multiple <Svg> nodes; raising the limit needs VEC_BRIDGE_MAX_PAINTS + ERUI_VECTOR_PAINTS_MAX.`
    );
  }
  if (!_warnedVecGrads && maxGrads > 0 && gradsLen > maxGrads) {
    _warnedVecGrads = true;
    console.warn(
      `embedded-react: an <Svg> references ${gradsLen} gradients (> ${maxGrads}) — the extra gradients are ` +
        `dropped and shapes that use them fall back to solid fills/strokes. Reuse fewer distinct gradients across ` +
        `shapes; raising the limit needs VEC_BRIDGE_MAX_GRADS + ERUI_VECTOR_GRADS_MAX.`
    );
  }
}
