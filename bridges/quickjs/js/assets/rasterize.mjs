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

// Pure-JS glyph rasterizer — fills an opentype.js Path into a grayscale coverage bitmap with no
// native dependencies. Curves are flattened to polylines, then each pixel's coverage is computed by
// scanline-filling at SS× supersampling using the nonzero winding rule (TrueType's convention).
//
// The output convention matches what the engine's text renderer expects (see draw_glyph /
// draw_glyph_aa in engine/text/text_renderer.c): coordinates come from glyph.getPath(0, baseline,
// pixelSize), so xOffset is the glyph's left bearing from the pen and yOffset is the distance from
// the line box top (y = 0) down to the top of the glyph ink.

/**
 * Flattens opentype path commands into closed contours of {x, y} points.
 *
 * @param {Array} commands  opentype.js Path commands (M/L/Q/C/Z).
 * @param {number} steps    Subdivision steps per Bézier curve.
 * @returns {Array<Array<{x:number,y:number}>>} One point array per contour.
 */
function flattenCommands(commands, steps) {
  const contours = [];
  let cur = null;
  let cx = 0;
  let cy = 0; // current point
  let sx = 0;
  let sy = 0; // contour start
  for (const c of commands) {
    if (c.type === 'M') {
      if (cur && cur.length > 1) contours.push(cur);
      cur = [{ x: c.x, y: c.y }];
      cx = sx = c.x;
      cy = sy = c.y;
    } else if (c.type === 'L') {
      cur.push({ x: c.x, y: c.y });
      cx = c.x;
      cy = c.y;
    } else if (c.type === 'Q') {
      for (let i = 1; i <= steps; i++) {
        const t = i / steps;
        const mt = 1 - t;
        cur.push({
          x: mt * mt * cx + 2 * mt * t * c.x1 + t * t * c.x,
          y: mt * mt * cy + 2 * mt * t * c.y1 + t * t * c.y,
        });
      }
      cx = c.x;
      cy = c.y;
    } else if (c.type === 'C') {
      for (let i = 1; i <= steps; i++) {
        const t = i / steps;
        const mt = 1 - t;
        cur.push({
          x: mt * mt * mt * cx + 3 * mt * mt * t * c.x1 + 3 * mt * t * t * c.x2 + t * t * t * c.x,
          y: mt * mt * mt * cy + 3 * mt * mt * t * c.y1 + 3 * mt * t * t * c.y2 + t * t * t * c.y,
        });
      }
      cx = c.x;
      cy = c.y;
    } else if (c.type === 'Z') {
      if (cur) {
        cur.push({ x: sx, y: sy }); // close the contour
        if (cur.length > 1) contours.push(cur);
        cur = null;
      }
    }
  }
  if (cur && cur.length > 1) contours.push(cur);
  return contours;
}

/**
 * Rasterizes a glyph path to an 8-bit coverage bitmap (0-255 per pixel).
 *
 * @param {object} path        opentype.js Path (from glyph.getPath(0, baseline, pixelSize)).
 * @param {object} [opts]
 * @param {number} [opts.ss]    Supersample factor per axis (coverage levels = ss*ss). Default 4.
 * @param {number} [opts.steps] Bézier subdivision steps. Default 12.
 * @returns {{width:number,height:number,xOffset:number,yOffset:number,coverage:Uint8Array}}
 *          An empty (width 0, height 0) result for whitespace / glyphs with no ink.
 */
export function rasterize(path, opts = {}) {
  const ss = opts.ss || 4;
  const steps = opts.steps || 12;
  const contours = flattenCommands(path.commands, steps);

  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const ct of contours) {
    for (const p of ct) {
      if (p.x < minX) minX = p.x;
      if (p.x > maxX) maxX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.y > maxY) maxY = p.y;
    }
  }
  if (!Number.isFinite(minX)) {
    return { width: 0, height: 0, xOffset: 0, yOffset: 0, coverage: new Uint8Array(0) };
  }

  const x0 = Math.floor(minX);
  const y0 = Math.floor(minY);
  const width = Math.ceil(maxX) - x0;
  const height = Math.ceil(maxY) - y0;
  if (width <= 0 || height <= 0) {
    return { width: 0, height: 0, xOffset: 0, yOffset: 0, coverage: new Uint8Array(0) };
  }

  // Non-horizontal edges, normalized so ylo < yhi; dir is the winding contribution.
  const edges = [];
  for (const ct of contours) {
    for (let i = 0; i + 1 < ct.length; i++) {
      const a = ct[i];
      const b = ct[i + 1];
      if (a.y === b.y) continue;
      if (a.y < b.y) edges.push({ ylo: a.y, yhi: b.y, x: a.x, dxdy: (b.x - a.x) / (b.y - a.y), dir: 1 });
      else edges.push({ ylo: b.y, yhi: a.y, x: b.x, dxdy: (a.x - b.x) / (a.y - b.y), dir: -1 });
    }
  }

  const counts = new Uint16Array(width * height); // covered subsamples per output pixel
  const subW = width * ss;
  for (let sy = 0; sy < height * ss; sy++) {
    const sampleY = y0 + (sy + 0.5) / ss;
    const xs = [];
    for (const e of edges) {
      if (sampleY >= e.ylo && sampleY < e.yhi) {
        xs.push({ x: e.x + (sampleY - e.ylo) * e.dxdy, dir: e.dir });
      }
    }
    if (xs.length < 2) continue;
    xs.sort((p, q) => p.x - q.x);
    const outRow = (sy / ss) | 0;
    let wind = 0;
    for (let i = 0; i + 1 < xs.length; i++) {
      wind += xs[i].dir;
      if (wind === 0) continue;
      // Interior span [xs[i].x, xs[i+1].x): tally the subsample columns whose centre falls inside.
      let sxa = Math.ceil((xs[i].x - x0) * ss - 0.5);
      let sxb = Math.ceil((xs[i + 1].x - x0) * ss - 0.5);
      if (sxa < 0) sxa = 0;
      if (sxb > subW) sxb = subW;
      for (let sx = sxa; sx < sxb; sx++) {
        counts[outRow * width + ((sx / ss) | 0)]++;
      }
    }
  }

  const maxCov = ss * ss;
  const coverage = new Uint8Array(width * height);
  for (let i = 0; i < coverage.length; i++) {
    coverage[i] = Math.round((counts[i] * 255) / maxCov);
  }
  return { width, height, xOffset: x0, yOffset: y0, coverage };
}
