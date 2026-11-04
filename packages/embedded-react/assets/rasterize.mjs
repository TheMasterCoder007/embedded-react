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

const DEFAULT_SUPERSAMPLE = 4;
const DEFAULT_CURVE_STEPS = 12;
const COVERAGE_MAX = 255;
const SAMPLE_CENTER = 0.5;

/**
 * Flattens opentype path commands into closed contours of {x, y} points.
 *
 * @param {Array} commands  opentype.js Path commands (M/L/Q/C/Z).
 * @param {number} steps    Subdivision steps per Bézier curve.
 * @returns {Array<Array<{x:number,y:number}>>} One point array per contour.
 */
function flattenCommands(commands, steps) {
  const contours = [];
  let contour = null;
  let currentX = 0;
  let currentY = 0;
  let startX = 0;
  let startY = 0;
  for (const command of commands) {
    if (command.type === 'M') {
      if (contour && contour.length > 1) contours.push(contour);
      contour = [{x: command.x, y: command.y}];
      currentX = startX = command.x;
      currentY = startY = command.y;
    } else if (command.type === 'L') {
      contour.push({x: command.x, y: command.y});
      currentX = command.x;
      currentY = command.y;
    } else if (command.type === 'Q') {
      for (let step = 1; step <= steps; step++) {
        const t = step / steps;
        const oneMinusT = 1 - t;
        contour.push({
          x:
            oneMinusT * oneMinusT * currentX +
            2 * oneMinusT * t * command.x1 +
            t * t * command.x,
          y:
            oneMinusT * oneMinusT * currentY +
            2 * oneMinusT * t * command.y1 +
            t * t * command.y,
        });
      }
      currentX = command.x;
      currentY = command.y;
    } else if (command.type === 'C') {
      for (let step = 1; step <= steps; step++) {
        const t = step / steps;
        const oneMinusT = 1 - t;
        contour.push({
          x:
            oneMinusT * oneMinusT * oneMinusT * currentX +
            3 * oneMinusT * oneMinusT * t * command.x1 +
            3 * oneMinusT * t * t * command.x2 +
            t * t * t * command.x,
          y:
            oneMinusT * oneMinusT * oneMinusT * currentY +
            3 * oneMinusT * oneMinusT * t * command.y1 +
            3 * oneMinusT * t * t * command.y2 +
            t * t * t * command.y,
        });
      }
      currentX = command.x;
      currentY = command.y;
    } else if (command.type === 'Z') {
      if (contour) {
        contour.push({x: startX, y: startY}); // close the contour
        if (contour.length > 1) contours.push(contour);
        contour = null;
      }
    }
  }
  if (contour && contour.length > 1) contours.push(contour);
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
  const supersample = opts.ss || DEFAULT_SUPERSAMPLE;
  const steps = opts.steps || DEFAULT_CURVE_STEPS;
  const contours = flattenCommands(path.commands, steps);

  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const contour of contours) {
    for (const point of contour) {
      if (point.x < minX) minX = point.x;
      if (point.x > maxX) maxX = point.x;
      if (point.y < minY) minY = point.y;
      if (point.y > maxY) maxY = point.y;
    }
  }
  if (!Number.isFinite(minX)) {
    return {
      width: 0,
      height: 0,
      xOffset: 0,
      yOffset: 0,
      coverage: new Uint8Array(0),
    };
  }

  const originX = Math.floor(minX);
  const originY = Math.floor(minY);
  const width = Math.ceil(maxX) - originX;
  const height = Math.ceil(maxY) - originY;
  if (width <= 0 || height <= 0) {
    return {
      width: 0,
      height: 0,
      xOffset: 0,
      yOffset: 0,
      coverage: new Uint8Array(0),
    };
  }

  // Non-horizontal edges, normalized so yLow < yHigh; winding is the edge's winding contribution.
  const edges = [];
  for (const contour of contours) {
    for (let index = 0; index + 1 < contour.length; index++) {
      const from = contour[index];
      const to = contour[index + 1];
      if (from.y === to.y) continue;
      if (from.y < to.y)
        edges.push({
          yLow: from.y,
          yHigh: to.y,
          x: from.x,
          slope: (to.x - from.x) / (to.y - from.y),
          winding: 1,
        });
      else
        edges.push({
          yLow: to.y,
          yHigh: from.y,
          x: to.x,
          slope: (from.x - to.x) / (from.y - to.y),
          winding: -1,
        });
    }
  }

  const counts = new Uint16Array(width * height); // covered subsamples per output pixel
  const subsampleWidth = width * supersample;
  for (
    let subsampleRow = 0;
    subsampleRow < height * supersample;
    subsampleRow++
  ) {
    const sampleY = originY + (subsampleRow + SAMPLE_CENTER) / supersample;
    const crossings = [];
    for (const edge of edges) {
      if (sampleY >= edge.yLow && sampleY < edge.yHigh) {
        crossings.push({
          x: edge.x + (sampleY - edge.yLow) * edge.slope,
          winding: edge.winding,
        });
      }
    }
    if (crossings.length < 2) continue;
    crossings.sort((left, right) => left.x - right.x);
    const outputRow = (subsampleRow / supersample) | 0;
    let winding = 0;
    for (let index = 0; index + 1 < crossings.length; index++) {
      winding += crossings[index].winding;
      if (winding === 0) continue;
      // Interior span [crossings[index].x, crossings[index+1].x): tally the subsample columns whose
      // centre falls inside.
      let spanStart = Math.ceil(
        (crossings[index].x - originX) * supersample - SAMPLE_CENTER,
      );
      let spanEnd = Math.ceil(
        (crossings[index + 1].x - originX) * supersample - SAMPLE_CENTER,
      );
      if (spanStart < 0) spanStart = 0;
      if (spanEnd > subsampleWidth) spanEnd = subsampleWidth;
      for (
        let subsampleColumn = spanStart;
        subsampleColumn < spanEnd;
        subsampleColumn++
      ) {
        counts[outputRow * width + ((subsampleColumn / supersample) | 0)]++;
      }
    }
  }

  const maxCoverage = supersample * supersample;
  const coverage = new Uint8Array(width * height);
  for (let index = 0; index < coverage.length; index++) {
    coverage[index] = Math.round((counts[index] * COVERAGE_MAX) / maxCoverage);
  }
  return {width, height, xOffset: originX, yOffset: originY, coverage};
}
