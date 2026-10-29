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
 *   npm run aot -- <demo> --package  # a Flow B package (dist/app.erpkg) instead of C
 *
 * This file is the entry: compileSource(src) compiles a source string (pure, so it can be unit-tested on
 * inline JSX), and the CLI at the bottom reads a demo's App.jsx and writes dist/app.gen.{c,h}. The
 * compiler itself lives in compiler/, one module per stage. The pipeline: parse the App.jsx → collect
 * the module's component, state, hooks & refs → lower each piece (expressions, styles/text, handlers,
 * nodes) → write the lowered app as app.gen.c.
 *
 *   compiler/options.mts         resolveOptions — screen size + buffer caps, from opts, then ER_AOT_*
 *   compiler/types.mts           the types the stages share (Env, StatementContext, state/ref records)
 *   compiler/out.mts             Out — the handlers, effects and drawings lowering collects
 *   compiler/diagnostics.mts     aotError / withLoc / formatAotError — locate + hint unsupported syntax
 *   compiler/static-eval.mts     evalStatic — fold the compile-time-constant subset (styles, initials)
 *   compiler/c-syntax.mts        cstr / floatLit / i64Lit — JS values spelled as C
 *   compiler/expressions.mts     lowerExprWide — lower a JS expression (state/props/refs) to the IR
 *   compiler/ir/*.mts            the IR the stages lower to (ir/app.mts: the whole lowered app)
 *   compiler/c/*.mts             the C backend: prints the IR as C (emitExpr returns it as a CExpr)
 *   compiler/parse.mts           parseApp — JSX/TSX source → AST (types stripped, `undefined` normalized)
 *   compiler/collect.mts         moduleScope + collect{State, Components, Callbacks, Memos, Effects}
 *   compiler/animations.mts      collect{Anims,Refs}, Easing, interpolate, Animated.start() chains
 *   compiler/style-text.mts      attrExpr, collectStyleAssigns, buildText / text spans
 *   compiler/handlers.mts        on* arrow → statements (setters, refs, timers, effects, updateVector)
 *   compiler/pan-responder.mts   PanResponder.create → engine responder queries + events
 *   compiler/control-flow.mts    components / conditionals / .map — all UNROLL at compile time
 *   compiler/svg.mts             <Svg> subtree → flattenSvg ops/paints → er_node_set_vector_ops
 *   compiler/nodes.mts           emitNode — the element dispatcher + the generic host node
 *   compiler/elements/*.mts      typed components: Dial / Switch / TextInput / Modal / FlatList / …
 *   compiler/keyboard.mts        setKeyboardConfig({...}) → the keyboard config
 *   compiler/lower.mts           lowerApp — run the stages above over the parsed app
 *   compiler/check.mts           the check pass — the values lowering recorded, checked in order
 *   compiler/assemble.mts        writeApp — write the lowered app as app.gen.{c,h}
 *   compiler/package/*.mts       the package backend: the IR as a Flow B program, its container and report
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
import {bakeAssetPackBytes, bakeAssets} from '../assets/index.mjs';
import {warnMissingGlyphs} from '../assets/glyph-coverage.mjs';
import {analyzeFontSizes, warnFontSizes} from '../assets/font-sizes.mjs';
import {formatAotError} from './compiler/diagnostics.mts';
import {resolveOptions} from './compiler/options.mts';
import {bakeSvgArtifacts} from './compiler/collect.mts';
import {demoMarker, compileSourceImpl} from './compiler/assemble.mts';
import {inferWideSlots, sameSlotWidths} from './compiler/expressions.mts';
import {
  assemblePackage,
  compilePanel,
  packPackage,
  resourceReport,
} from './compiler/package/assemble.mts';
import type {CompileOptions, ResolvedOptions} from './compiler/options.mts';
import type {CompileResult} from './compiler/assemble.mts';
import type {PackageResult} from './compiler/package/assemble.mts';
import type {IrSlotStore} from './compiler/ir/source.mts';
import type {SlotWidths} from './compiler/ir/types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const aotDir = dirname(fileURLToPath(import.meta.url)); // packages/embedded-react/aot
const repoRoot = resolve(aotDir, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(aotDir, '..', 'dist');

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

export {bakeSvgArtifacts, demoMarker, packPackage, resourceReport};

/**
 * A state or ref slot is typed from its initial value, so `useState(0)` is an int until a setter stores
 * Date.now() in it. A compile records every value stored into an int slot (env.stores), and the typing pass
 * infers from them which slots have to hold 64 bits, following a timestamp through every slot it reaches; the
 * app is then compiled again with those slots widened, so every read of a slot sees its final type.
 *
 * @param compileOnce  One compile with the given slots widened, recording the values stored into int slots.
 *
 * @returns What the last compile produced.
 */
function compileWidened<Result>(
  compileOnce: (wide: SlotWidths, stores: IrSlotStore[]) => Result,
): Result {
  // The slots found so far that need 64 bits; every pass compiles with all of them widened.
  let wide: SlotWidths = new Map();
  for (;;) {
    // Run one pass, holding on to its error instead of throwing it straight away.
    const stores: IrSlotStore[] = [];
    let result: Result | undefined;
    let error: unknown;
    try {
      result = compileOnce(wide, stores);
    } catch (caught) {
      error = caught;
    }

    // Stop once no slot is inferred to widen; a failed pass with new ones may have failed on a narrow type.
    const inferred = inferWideSlots(stores, wide);
    if (sameSlotWidths(inferred, wide)) {
      if (error) {
        throw error;
      }
      return result!;
    }

    wide = inferred;
  }
}

/**
 * Runs a compilation, annotating an AOT error with file:line:col + a source code-frame (+ hint) so the failure
 * points at the exact unsupported construct.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name, for the file the error names when opts has none.
 * @param opts  The compile's options.
 * @param resolved  The same options, resolved.
 * @param compileOnce  One compile, as compileWidened runs it.
 *
 * @returns What the compile produced.
 */
function compileLocated<Result>(
  src: string,
  demo: string,
  opts: CompileOptions,
  resolved: ResolvedOptions,
  compileOnce: (wide: SlotWidths, stores: IrSlotStore[]) => Result,
): Result {
  try {
    return compileWidened(compileOnce);
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
  return compileLocated(src, demo, opts, resolved, (wide, stores) =>
    compileSourceImpl(src, demo, resolved, wide, stores),
  );
}

/**
 * Public entry for the package output: compile JSX source to a Flow B program section, with a layout for each
 * panel in opts.screens (or the one screen). Each panel is compiled as compileSource compiles for it. Errors are
 * annotated as compileSource's are.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name, for the file an error names when opts has none.
 * @param opts  As compileSource takes them, plus screens.
 *
 * @returns The program section, and what the package holds.
 */
export function compilePackage(
  src: string,
  demo = 'app',
  opts: CompileOptions = {},
): PackageResult {
  const screens = opts.screens?.length ? opts.screens : [opts.screen];
  const seen = new Set<string>();
  const panels = screens.map(screen => {
    const resolved = resolveOptions({...opts, screen});
    const size = `${resolved.screen.width}x${resolved.screen.height}`;
    if (seen.has(size)) {
      throw new Error(`AOT: the panel ${size} is listed twice`);
    }
    seen.add(size);
    return compileLocated(src, demo, opts, resolved, (wide, stores) =>
      compilePanel(src, resolved, wide, stores),
    );
  });
  return assemblePackage(panels);
}

// CLI entry (`node aot/compile.mts [demo]`), only when run directly: compile a demo into dist/app.gen.{c,h}.
if (
  process.argv[1] &&
  resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))
) {
  // Find the demo's App.jsx, listing the available demos when it is missing. --package writes a Flow B package
  // instead of C.
  const args = process.argv.slice(2);
  const asPackage = args.includes('--package');
  const demo =
    args.find(arg => !arg.startsWith('--')) || process.env.DEMO || 'thermostat';
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
  let result: CompileResult | PackageResult;
  try {
    const options: CompileOptions = {
      filename: resolve(demosDir, demo, 'App.jsx'),
      svgArtifacts: await bakeSvgArtifacts(src, resolve(demosDir, demo)),
    };
    result = asPackage
      ? compilePackage(src, demo, options)
      : compileSource(src, demo, options);
  } catch (error: any) {
    // Print just the message, no JS stack: a located AOT error already carries its location, code frame and hint.
    console.error(
      error && error.aotLoc ? error.message : error?.message || String(error),
    );
    process.exit(1);
  }

  // Write the generated C.
  mkdirSync(distDir, {recursive: true});
  if (!('program' in result)) {
    writeFileSync(resolve(distDir, 'app.gen.c'), result.c);
    writeFileSync(resolve(distDir, 'app.gen.h'), result.h);
  }

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

  if ('program' in result) {
    // The package carries its images as an asset pack.
    const file = packPackage(
      result,
      imageJobs.length ? bakeAssetPackBytes({images: imageJobs}) : undefined,
    );
    writeFileSync(resolve(distDir, 'app.erpkg'), file.bytes);
    console.log(`AOT: compiled demo "${demo}" -> dist/app.erpkg`);
    console.log(resourceReport(result, file).join('\n'));
  } else {
    // Bake images with Flow A's baker, even with none (a no-op), so the AOT host can always build and call it.
    const baked = bakeAssets({images: imageJobs, fonts: [], outDir: distDir});
    console.log(
      `AOT: compiled demo "${demo}" -> dist/app.gen.c (${result.nodes} nodes, ${result.state} state, ` +
        `${result.handlers} handler(s), ${result.updates} dynamic) + ${baked.images} image(s) -> dist/assets.generated.c`,
    );
  }
}
