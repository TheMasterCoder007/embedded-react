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

// esbuild loader for `import logo from './logo.svg'`. Two outcomes, decided at bake time:
//   - VECTOR: the SVG is fully representable as the engine's op-tape — bake it and INLINE a
//     {kind:'vector', ops, paints, gradients, width, height} artifact (small numeric data) into the bundle.
//   - RASTER: the SVG uses features the vector baker can't represent (text, mask, filter, use,
//     pattern, …). We warn, rasterize the whole SVG via resvg to a PNG, register that PNG through the
//     bundler's normal image pipeline (a small `addRasterAsset(name, pngPath)` callback), and inline a
//     {kind:'raster', name, width, height} artifact. A <Svg source> renders kind:'vector' as a vector node
//     and kind:'raster' as an image node (host-config), so the fallback is transparent to the app.
// Shared by every Flow A bundler so the .svg handling lives in one place.
import {readFileSync} from 'node:fs';
import {basename} from 'node:path';
import {svgToVector, svgToRaster, writeRasterPng} from './bake-svg.mjs';

const assetName = p => basename(p).replace(/\.[^.]+$/, '');

/**
 * Registers a `.svg` onLoad on an esbuild build.
 *
 * @param {import('esbuild').PluginBuild} build  The esbuild build passed to a plugin's setup().
 * @param {(name:string, pngPath:string)=>void} [addRasterAsset]  Registers a rasterized-SVG PNG into the
 *        bundler's image collection (same mechanism as `import x from './x.png'`). When omitted, an SVG that
 *        needs raster fallback is baked as a (lossy) vector and a warning notes the dropped content.
 */
export function registerSvgVectorLoader(build, addRasterAsset) {
  build.onLoad({filter: /\.svg$/i}, async args => {
    try {
      const svg = readFileSync(args.path, 'utf8');
      const {dropped = [], ...vec} = await svgToVector(svg);
      if (dropped.length) {
        const feats = dropped.join(', ');
        if (addRasterAsset) {
          console.warn(
            `embedded-react: ${basename(args.path)} uses unsupported SVG feature(s) [${feats}] — rasterizing ` +
              `it as a fallback image. (Raster loses scalability and costs RAM; simplify the SVG to keep it a ` +
              `live vector.)`,
          );
          const {width, height, png} = await svgToRaster(svg);
          const name = assetName(args.path);
          addRasterAsset(name, writeRasterPng(name, png));
          return {
            contents: `module.exports = ${JSON.stringify({kind: 'raster', name, width, height})};`,
            loader: 'js',
          };
        }
        console.warn(
          `embedded-react: ${basename(args.path)} uses unsupported SVG feature(s) [${feats}] and this build has ` +
            `no raster fallback — that content will NOT render. Simplify the SVG, or import it via a bundler that ` +
            `supports the image pipeline.`,
        );
      }
      return {
        contents: `module.exports = ${JSON.stringify({kind: 'vector', ...vec})};`,
        loader: 'js',
      };
    } catch (e) {
      return {
        errors: [
          {
            text: `embedded-react: failed to bake SVG ${args.path}: ${e.message}`,
          },
        ],
      };
    }
  });
}
