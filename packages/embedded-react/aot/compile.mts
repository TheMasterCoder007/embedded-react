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
 * `npm run aot [demo]` — the Flow B ahead-of-time compiler.
 *
 * Compiles a demo's JSX to a package (dist/app.erpkg) that the Flow B runtime verifies and runs: no QuickJS, no JS
 * at runtime. The program builds the engine node tree and wires state + events, so it fits an MCU with only
 * internal RAM.
 *
 * Supported subset (grows demo by demo; unsupported syntax throws "AOT: ..."):
 *   - View / Text / Pressable / TouchableOpacity / Image / ScrollView elements
 *   - StyleSheet + inline styles → ERProps (static values)
 *   - text with literal + {interpolation} segments (interpolations may reference state)
 *   - useState(initial) → state slots; on* handlers (onPress/onPressIn/onPressOut/onLongPress) → functions
 *   - setState(value) and setState(prev => expr); a small expression subset (literals, identifiers,
 *     + - * / %, comparisons, ?:)
 *
 * The compiler tracks which nodes depend on which state, so a state change re-sets ONLY the dependent
 * nodes — no diffing, no reconciler. See the root README (Flow B).
 *
 *   npm run aot                      # default demo (thermostat) — but use a minimal demo for the slice
 *   npm run aot -- watch-face        # a specific demo by folder name
 *   npm run aot -- <demo> --screen 240x320  # the panels the package keeps layouts for
 *
 * This file is the entry: compilePackage(src) compiles a source string (pure, so it can be unit-tested on
 * inline JSX), and the CLI at the bottom reads a demo's App.jsx and writes dist/app.erpkg. The
 * compiler itself lives in compiler/, one module per stage. The pipeline: parse the App.jsx → collect
 * the module's component, state, hooks & refs → lower each piece (expressions, styles/text, handlers,
 * nodes) → write the lowered app as a package.
 *
 *   compiler/options.mts         resolveOptions — screen size + buffer caps, from opts, then ER_AOT_*
 *   compiler/types.mts           the types the stages share (Env, StatementContext, state/ref records)
 *   compiler/out.mts             Out — the handlers, effects and drawings lowering collects
 *   compiler/diagnostics.mts     aotError / withLoc / formatAotError — locate + hint unsupported syntax
 *   compiler/static-eval.mts     evalStatic — fold the compile-time-constant subset (styles, initials)
 *   compiler/expressions.mts     lowerExprWide — lower a JS expression (state/props/refs) to the IR
 *   compiler/ir/*.mts            the IR the stages lower to (ir/app.mts: the whole lowered app)
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
import {basename, resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {bakeAssetPackBytes} from '../assets/index.mjs';
import {warnMissingGlyphs} from '../assets/glyph-coverage.mjs';
import {analyzeFontSizes, warnFontSizes} from '../assets/font-sizes.mjs';
import {formatAotError} from './compiler/diagnostics.mts';
import {resolveOptions} from './compiler/options.mts';
import {bakeSvgArtifacts} from './compiler/collect.mts';
import {inferWideSlots, sameSlotWidths} from './compiler/expressions.mts';
import {
  assemblePackage,
  bakePackage,
  compilePanel,
  packPackage,
  resourceReport,
} from './compiler/package/assemble.mts';
import {MAX_LAYOUTS, runCheck} from './compiler/package/checks.mts';
import {SizeSearch} from './compiler/package/layouts.mts';
import type {
  CompileOptions,
  ResolvedOptions,
  Screen,
} from './compiler/options.mts';
import type {
  PackageResult,
  PanelProgram,
} from './compiler/package/assemble.mts';
import type {LayoutCheck} from './compiler/package/checks.mts';
import type {IrSlotStore} from './compiler/ir/source.mts';
import type {SlotWidths} from './compiler/ir/types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const aotDir = dirname(fileURLToPath(import.meta.url)); // packages/embedded-react/aot
const repoRoot = resolve(aotDir, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const distDir = resolve(aotDir, '..', 'dist');

/** Where the search for an app's layouts starts. */
const FIRST_SIZE: Screen = {width: 240, height: 320};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

export {bakePackage, bakeSvgArtifacts, packPackage, resourceReport};

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
 * The sizes a package build was told to keep layouts for: opts.screens, opts.screen or ER_AOT_SCREEN_W/H.
 *
 * @param opts  The build's options.
 *
 * @returns The sizes, or null to find every layout.
 */
function requestedScreens(opts: CompileOptions): Screen[] | null {
  if (opts.screens?.length) return opts.screens;
  if (opts.screen) return [opts.screen];
  const fromEnv = (name: string): boolean =>
    (process.env[name] ?? '').trim() !== '';
  return fromEnv('ER_AOT_SCREEN_W') || fromEnv('ER_AOT_SCREEN_H')
    ? [resolveOptions(opts).screen]
    : null;
}

/**
 * The error for an app with too many layouts, naming the value that took the most different values across them.
 *
 * @param panels  The layouts so far.
 * @param named  The build names its panels already.
 *
 * @returns The error.
 */
function tooManyLayouts(panels: PanelProgram[], named: boolean): Error {
  const values = new Map<string, Set<unknown>>();
  for (const {assumptions} of panels) {
    for (const assumption of assumptions) {
      const source = assumption.source ?? 'a value derived from `screen`';
      const seen = values.get(source) ?? new Set<unknown>();
      seen.add(assumption.kind === 'value' ? assumption.value : seen.size);
      values.set(source, seen);
    }
  }
  const [worst] = [...values].sort(
    ([, left], [, right]) => right.size - left.size,
  )[0] ?? ['the app'];
  return new Error(
    `AOT: this app needs more than ${MAX_LAYOUTS} layouts, because \`${worst}\` changes what is compiled at ` +
      'too many sizes.\n\nA breakpoint picks between a few layouts; a value worked out from `screen` belongs in ' +
      'a style, which boot works out. ' +
      (named
        ? 'Or name fewer panels.'
        : 'To build for fixed panels, name them: --screen 240x320,800x480.'),
  );
}

/**
 * Public entry for the package output: compile JSX source to a Flow B program section, and its debug section.
 *
 * With no sizes named, it finds the app's layouts: it compiles one, finds a size that layout's check rejects, and
 * compiles again until every size has a layout. Named sizes keep only the layouts they pick. An AOT error is annotated
 * with file:line:col + a source code-frame (+ hint), so the failure points at the exact unsupported construct.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param demo  Demo name, for the file an error names when opts has none.
 * @param opts  filename, svgArtifacts, ts, screens, release, and the screen and caps resolveOptions documents.
 *
 * @returns The program and debug sections, and what the package holds.
 */
export function compilePackage(
  src: string,
  demo = 'app',
  opts: CompileOptions = {},
): PackageResult {
  const panels: PanelProgram[] = [];
  const checks = (): LayoutCheck[] =>
    panels.map(panel => panel.program.panels[0].check);
  const compileAt = (screen: Screen, searched: boolean): void => {
    const options = resolveOptions({...opts, screen});
    const resolved: ResolvedOptions = searched
      ? {...options, screenFrom: 'search'}
      : options;
    panels.push(
      compileLocated(src, demo, opts, resolved, (wide, stores) =>
        compilePanel(src, resolved, wide, stores),
      ),
    );
  };

  const requested = requestedScreens(opts);
  if (requested) {
    const seen = new Set<string>();
    for (const screen of requested) {
      const {width, height} = resolveOptions({...opts, screen}).screen;
      const size = `${width}x${height}`;
      if (seen.has(size)) {
        throw new Error(`AOT: the panel ${size} is listed twice`);
      }
      seen.add(size);
      if (!checks().some(check => runCheck(check, width, height))) {
        if (panels.length === MAX_LAYOUTS) throw tooManyLayouts(panels, true);
        compileAt({width, height}, false);
      }
    }
  } else {
    const search = new SizeSearch();
    let next: ReturnType<SizeSearch['next']> = FIRST_SIZE;
    while (next && next !== 'gave-up') {
      if (panels.length === MAX_LAYOUTS) throw tooManyLayouts(panels, false);
      compileAt(next, true);
      if (!runCheck(checks().at(-1)!, next.width, next.height)) {
        throw new Error(
          `AOT: the layout for ${next.width}x${next.height} doesn't take that size. This is a compiler bug.`,
        );
      }
      next = search.next(checks());
    }
    if (next === 'gave-up') {
      throw new Error(
        "AOT: can't tell which layouts this app needs: its checks are too intricate to work out for every size. " +
          'Name the panels it runs on: --screen 240x320,800x480.',
      );
    }
  }

  return assemblePackage(
    panels,
    basename(opts.filename || 'App.jsx'),
    opts.release,
  );
}

// CLI entry (`node aot/compile.mts [demo]`), only when run directly: compile a demo into dist/app.erpkg.
if (
  process.argv[1] &&
  resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))
) {
  // Find the demo's App.jsx, listing the available demos when it is missing.
  const args = process.argv.slice(2);
  const screenAt = args.indexOf('--screen');
  const screens =
    screenAt < 0
      ? undefined
      : (args[screenAt + 1] ?? '').split(',').map(size => {
          const match = /^([1-9]\d*)x([1-9]\d*)$/i.exec(size);
          if (!match) {
            console.error(
              `AOT: --screen expects <width>x<height> (e.g. --screen 240x320), got: ${args[screenAt + 1] ?? '(nothing)'}`,
            );
            process.exit(1);
          }
          return {width: Number(match[1]), height: Number(match[2])};
        });
  const demo =
    args.find(
      (arg, index) =>
        !arg.startsWith('--') && (screenAt < 0 || index !== screenAt + 1),
    ) ||
    process.env.DEMO ||
    'thermostat';
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
  let result: PackageResult;
  try {
    result = compilePackage(src, demo, {
      filename: resolve(demosDir, demo, 'App.jsx'),
      svgArtifacts: await bakeSvgArtifacts(src, resolve(demosDir, demo)),
      screen: screens?.[0],
      screens,
    });
  } catch (error: any) {
    // Print just the message, no JS stack: a located AOT error already carries its location, code frame and hint.
    console.error(
      error && error.aotLoc ? error.message : error?.message || String(error),
    );
    process.exit(1);
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

  // The package carries its images as an asset pack.
  const file = packPackage(
    result,
    imageJobs.length ? bakeAssetPackBytes({images: imageJobs}) : undefined,
  );
  mkdirSync(distDir, {recursive: true});
  writeFileSync(resolve(distDir, 'app.erpkg'), file.bytes);
  console.log(`AOT: compiled demo "${demo}" -> dist/app.erpkg`);
  console.log(resourceReport(result, file).join('\n'));
}
