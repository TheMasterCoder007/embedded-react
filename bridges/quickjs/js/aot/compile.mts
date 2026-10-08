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

/*
 * `npm run aot [demo]` — the Flow B ahead-of-time compiler (vertical slice).
 *
 * Compiles a demo's JSX straight to C against er_scene.h: no QuickJS, no JS at runtime. The generated
 * app.gen.c builds the engine node tree directly and wires state + events, so it fits an MCU with only
 * internal RAM.
 *
 * Supported subset (grows demo by demo; unsupported syntax throws "AOT: ..."):
 *   - View / Text / Pressable / TouchableOpacity / Image / ScrollView elements
 *   - StyleSheet + inline styles → ERProps (static values)
 *   - text with literal + {interpolation} segments (interpolations may reference state)
 *   - useState(initial) → C state; on* handlers (onPress/onPressIn/onPressOut/onLongPress) → C functions
 *   - setState(value) and setState(prev => expr); a small C expression subset (literals, identifiers,
 *     + - * / %, comparisons, ?:)
 *
 * The compiler tracks which nodes depend on which state, so a state change re-sets ONLY the dependent
 * nodes (er_node_set_props) — no diffing, no reconciler. See the root README (Flow B).
 *
 *   npm run aot                      # default demo (thermostat) — but use a minimal demo for the slice
 *   npm run aot -- watch-face        # a specific demo by folder name
 *
 * This file is the entry: compileSource(src) compiles a source string (pure, so it can be unit-tested on
 * inline JSX), and the CLI at the bottom reads a demo's App.jsx and writes dist/app.gen.{c,h}. The
 * compiler itself lives in compiler/, one module per stage. The pipeline: parse the App.jsx → collect
 * the module's component, state, hooks & refs → emit each piece to C (expressions, styles/text,
 * handlers, nodes) → assemble app.gen.c.
 *
 *   compiler/options.mts         resolveOptions — screen size + buffer caps, from opts, then ER_AOT_*
 *   compiler/types.mts           the types the stages share (Env, StatementContext, state/ref records)
 *   compiler/out.mts             Out — everything a compile has emitted so far
 *   compiler/diagnostics.mts     aotError / withLoc / formatAotError — locate + hint unsupported syntax
 *   compiler/static-eval.mts     evalStatic — fold the compile-time-constant subset (styles, initials)
 *   compiler/c-syntax.mts        cstr / floatLit / i64Lit — JS values spelled as C
 *   compiler/expressions.mts     emitExpr — lower a JS expression (state/props/refs) to a C expression
 *   compiler/parse.mts           parseApp — JSX/TSX source → AST (types stripped, `undefined` normalized)
 *   compiler/collect.mts         moduleScope + collect{State, Components, Callbacks, Memos, Effects}
 *   compiler/animations.mts      collect{Anims,Refs}, Easing, interpolate, Animated.start() chains
 *   compiler/style-text.mts      attrExpr, collectStyleAssigns, buildText / text spans
 *   compiler/handlers.mts        on* arrow → C statements (setters, refs, timers, effects, updateVector)
 *   compiler/pan-responder.mts   PanResponder.create → engine responder queries + events
 *   compiler/control-flow.mts    components / conditionals / .map — all UNROLL at compile time
 *   compiler/svg.mts             <Svg> subtree → flattenSvg ops/paints → er_node_set_vector_ops
 *   compiler/nodes.mts           emitNode — the element dispatcher + the generic host node
 *   compiler/elements/*.mts      typed components: Dial / Switch / TextInput / Modal / FlatList / …
 *   compiler/keyboard.mts        setKeyboardConfig({...}) → static ERKeyboardConfig tables
 *   compiler/assemble.mts        compileSourceImpl — stitch the above into app.gen.{c,h}
 *
 * The compiler is TypeScript (.mts), which Node runs by stripping the types. tools/stage-npm-package.mjs
 * compiles it to JavaScript for the npm package.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  readFileSync,
  writeFileSync,
  mkdirSync,
  existsSync,
  readdirSync,
} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {bakeAssets} from '../assets/index.mjs';
import {warnMissingGlyphs} from '../assets/glyph-coverage.mjs';
import {analyzeFontSizes, warnFontSizes} from '../assets/font-sizes.mjs';
import {formatAotError} from './compiler/diagnostics.mts';
import {resolveOptions} from './compiler/options.mts';
import {bakeSvgArtifacts} from './compiler/collect.mts';
import {demoMarker, compileSourceImpl} from './compiler/assemble.mts';
import type {CompileOptions, ResolvedOptions} from './compiler/options.mts';
import type {CompileResult} from './compiler/assemble.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const aotDir = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js/aot
const repoRoot = resolve(aotDir, '../../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(aotDir, '..', 'dist');

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

export {bakeSvgArtifacts, demoMarker};

/**
 * A state or ref slot is typed from its initial value, so `useState(0)` is an int until a setter stores
 * Date.now() in it. A compile reports such slots (env.found); this compiles again with them widened to
 * int64_t until none are new, so every read of a slot sees its final type.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name (only used in the generated-by header comment).
 * @param opts  compileSource's options, resolved (see resolveOptions).
 *
 * @returns The generated app.gen.c and app.gen.h, and what they contain.
 */
function compileWidened(
  src: string,
  demo: string,
  opts: ResolvedOptions,
): CompileResult {
  // The slots found so far that need 64 bits; every pass compiles with all of them widened.
  const wide = new Set<string>();
  for (;;) {
    // Run one pass, holding on to its error instead of throwing it straight away.
    const found = new Set<string>();
    let result: CompileResult | undefined;
    let error: unknown;
    try {
      result = compileSourceImpl(src, demo, opts, wide, found);
    } catch (caught) {
      error = caught;
    }

    // Stop once a pass finds no new slot; a failed pass with new ones may have failed on a narrow type, so retry.
    const newSlots = [...found].filter(slot => !wide.has(slot));
    if (!newSlots.length) {
      if (error) {
        throw error;
      }
      return result!;
    }

    for (const slot of newSlots) {
      wide.add(slot);
    }
  }
}

/**
 * Public entry: compile JSX source → { c, h, ... }. On an AOT error, annotate it with file:line:col + a
 * source code-frame (+ hint) so the failure points at the exact unsupported construct.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name (only used in the generated-by header comment).
 * @param opts  filename, svgArtifacts, ts, and the screen and caps resolveOptions documents.
 *
 * @returns The generated app.gen.c and app.gen.h, and what they contain.
 */
export function compileSource(
  src: string,
  demo = 'app',
  opts: CompileOptions = {},
): CompileResult {
  const resolved = resolveOptions(opts);
  try {
    return compileWidened(src, demo, resolved);
  } catch (error: any) {
    // Point an AOT error at its place in the source; any other error passes through unchanged.
    if (
      error &&
      typeof error.message === 'string' &&
      error.message.startsWith('AOT:')
    ) {
      throw formatAotError(
        error,
        src,
        opts.filename || `demos/${demo}/App.jsx`,
        resolved,
      );
    }

    throw error;
  }
}

// CLI entry (`node aot/compile.mts [demo]`), only when run directly: compile a demo into dist/app.gen.{c,h}.
if (
  process.argv[1] &&
  resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))
) {
  // Find the demo's App.jsx, listing the available demos when it is missing.
  const demo = process.argv[2] || process.env.DEMO || 'thermostat';
  const appPath = resolve(demosDir, demo, 'App.jsx');
  const availableDemos = existsSync(demosDir)
    ? readdirSync(demosDir, {withFileTypes: true})
        .filter(entry => entry.isDirectory())
        .map(entry => entry.name)
    : [];
  if (!existsSync(appPath)) {
    console.error(
      `AOT: demo "${demo}" not found (expected ${appPath}). Available: ${availableDemos.join(', ') || '(none)'}`,
    );
    process.exit(1);
  }

  // Compile the app, baking <Svg source> .svg imports first so the compile itself stays pure (no I/O).
  const src = readFileSync(appPath, 'utf8');
  let result;
  try {
    const svgArtifacts = await bakeSvgArtifacts(src, resolve(demosDir, demo));
    result = compileSource(src, demo, {
      filename: resolve(demosDir, demo, 'App.jsx'),
      svgArtifacts,
    });
  } catch (error: any) {
    // Print just the message, no JS stack: a located AOT error already carries its location, code frame and hint.
    console.error(
      error && error.aotLoc ? error.message : error?.message || String(error),
    );
    process.exit(1);
  }

  // Write the generated C.
  mkdirSync(distDir, {recursive: true});
  writeFileSync(resolve(distDir, 'app.gen.c'), result.c);
  writeFileSync(resolve(distDir, 'app.gen.h'), result.h);

  // Locate the images the app imports; each importPath is source-relative to the demo's App.jsx.
  const imageJobs = result.images.map(image => ({
    name: image.name,
    path: resolve(demosDir, demo, image.importPath),
  }));

  // Stop on an imported image that is not on disk.
  for (const job of imageJobs) {
    if (!existsSync(job.path)) {
      console.error(
        `AOT: <Image> asset "${job.name}" not found at ${job.path}`,
      );
      process.exit(1);
    }
  }

  // Flow B renders all text in the built-in font, so check the app's glyphs and font sizes against it.
  warnMissingGlyphs({source: src, jsx: true});
  warnFontSizes({used: analyzeFontSizes(src)});

  // Bake images with Flow A's baker, even with none (a no-op), so the AOT host can always build and call it.
  const baked = bakeAssets({images: imageJobs, fonts: [], outDir: distDir});
  console.log(
    `AOT: compiled demo "${demo}" -> dist/app.gen.c (${result.nodes} nodes, ${result.state} state, ` +
      `${result.handlers} handler(s), ${result.updates} dynamic) + ${baked.images} image(s) -> dist/assets.generated.c`,
  );
}
