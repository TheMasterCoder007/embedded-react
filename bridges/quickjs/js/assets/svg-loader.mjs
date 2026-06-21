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

// esbuild loader for `import logo from './logo.svg'` (Phase 3 Track A). Unlike images (which are recorded
// and baked into a separate asset pack), an SVG's vector op-tape is small numeric data, so it's baked here
// and INLINED into the bundle as the module's exports — a {kind:'vector', ops, paints, width, height}
// artifact that a <Svg source> renders directly. Shared by every Flow A bundler (build.mjs, sim-server.mjs,
// cli.mjs) so the .svg handling lives in one place. (Raster fallback — Track C — will tag {kind:'raster'}.)
import { readFileSync } from 'node:fs';
import { svgToVector } from './bake-svg.mjs';

/**
 * Registers a `.svg` onLoad on an esbuild build that bakes the file to an inline vector artifact.
 *
 * @param {import('esbuild').PluginBuild} build  The esbuild build passed to a plugin's setup().
 */
export function registerSvgVectorLoader(build) {
  build.onLoad({ filter: /\.svg$/i }, async (args) => {
    try {
      const art = await svgToVector(readFileSync(args.path, 'utf8'));
      return { contents: `module.exports = ${JSON.stringify({ kind: 'vector', ...art })};`, loader: 'js' };
    } catch (e) {
      return { errors: [{ text: `embedded-react: failed to bake SVG ${args.path}: ${e.message}` }] };
    }
  });
}
