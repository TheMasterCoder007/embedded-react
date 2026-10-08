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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import {
  evalStaticOrThrow,
  evalStatic,
  foldScope,
  evalStaticOr,
} from './static-eval.mts';
import {i64Lit, floatLit} from './c-syntax.mts';
import {INT_MAX, emitExpr} from './expressions.mts';
import {NO_WIDE, isFn} from './collect.mts';
import {APP_UPDATE_CALL, compileStmts} from './handlers.mts';
import {isPanCreate} from './pan-responder.mts';
import type * as t from '@babel/types';
import type {
  AnimRecord,
  Env,
  FunctionNode,
  Local,
  RefRecord,
  Scope,
  StateTable,
  StatementContext,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An `Animated.<kind>(…)` call: the only shape isAnimatedCall accepts. */
export type AnimatedCall = t.CallExpression & {
  callee: t.MemberExpression & {property: t.Identifier};
};

/** Reads a config object literal's member by key; undefined when the key is absent. */
type ConfigGetter = (key: string) => t.Node | undefined;

/** One atomic animation (timing / spring / decay) on one value, with its absolute start delay. */
interface AnimEntry {
  /** The animated value's C handle. */
  cVar: string;
  kind: string;
  get: ConfigGetter;
  delayMs: number;
  /** Repeat forever on the engine's own cfg.loop. */
  loop: boolean;
}

/** A flattened composition: its atomic entries and its own run length (null when not fixed). */
interface FlatAnimation {
  entries: AnimEntry[];
  duration: number | null;
}

/** A `.interpolate({...})` config, folded: the breakpoints and the ER_EXTRAPOLATE_* mode at each end. */
export interface Interpolation {
  input: number[];
  output: number[];
  exLeft: string;
  exRight: string;
}

/** How an on_complete chain repeats (see emitAnimChain). */
interface LoopSpec {
  /** Repeats; negative loops forever. */
  iterations: number;
  /** The value to put back where it started before each repeat, or null. */
  resetTo: string | null;
  /** Milliseconds before each repeat. */
  trailingDelay: number;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Style key → the ERAnimProp(s) an animated value binds to. */
export const ANIM_STYLE_PROPS: Record<string, string[]> = {
  opacity: ['ER_PROP_OPACITY'],
  backgroundColor: ['ER_PROP_BACKGROUND_COLOR'],
  color: ['ER_PROP_COLOR'],
};

/** Transform key → the ERAnimProp(s) an animated value binds to. */
export const ANIM_TRANSFORM_PROPS: Record<string, string[]> = {
  scale: ['ER_PROP_SCALE_X', 'ER_PROP_SCALE_Y'],
  scaleX: ['ER_PROP_SCALE_X'],
  scaleY: ['ER_PROP_SCALE_Y'],
  translateX: ['ER_PROP_TRANSLATE_X'],
  translateY: ['ER_PROP_TRANSLATE_Y'],
  rotate: ['ER_PROP_ROTATE_Z'],
  rotateZ: ['ER_PROP_ROTATE_Z'],
};

const ANIM_KINDS = new Set([
  'timing',
  'spring',
  'decay',
  'sequence',
  'parallel',
  'stagger',
  'delay',
  'loop',
]);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Collects `const x = useAnimatedValue(initial)` → Map (name → {cVar, initCode}). `prefix` namespaces the
 * C var (`s_av_<prefix><name>`) so each inlined child instance gets its own engine value handle.
 *
 * @param fnBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param prefix  The C name prefix: none for the App, `c<n>_` for an inlined instance.
 *
 * @returns The animated values, by name.
 */
export function collectAnims(
  fnBody: t.BlockStatement | t.Expression,
  scope: Scope,
  prefix = '',
): Map<string, AnimRecord> {
  // Collect animated value records by their JS binding name.
  const anims = new Map<string, AnimRecord>();

  // Only block-bodied functions can contain hook declarations to collect.
  if (fnBody.type !== 'BlockStatement') return anims;

  // Scan top-level declarations in the function body for useAnimatedValue hooks to allocate animated slots.
  for (const stmt of fnBody.body) {
    // Only variable declarations can introduce bindings we need to inspect.
    if (stmt.type !== 'VariableDeclaration') continue;

    // Collect useAnimatedValue declarations and register their engine-side animated slots.
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        (init.callee as t.Identifier).name === 'useAnimatedValue' &&
        decl.id.type === 'Identifier'
      ) {
        const initVal = init.arguments[0]
          ? evalStaticOrThrow(
              init.arguments[0],
              scope,
              `AOT: the initial value of useAnimatedValue "${decl.id.name}" must be a compile-time constant`,
              'give it a finite starting number: useAnimatedValue(0).',
            )
          : 0;
        // An animated value is a float slot; null or a non-finite number would reach C as `NaNf`.
        // Refuse here, at the declaration, with its location.
        if (typeof initVal !== 'number' || !Number.isFinite(initVal)) {
          const e = aotError(
            `AOT: the initial value of useAnimatedValue "${decl.id.name}" is ${initVal === undefined ? 'undefined' : String(initVal)}`,
            'give it a finite starting number: useAnimatedValue(0).',
          );
          if (init.arguments[0]?.loc) {
            e.aotLoc = init.arguments[0].loc.start;
          }
          throw e;
        }
        anims.set(decl.id.name, {
          cVar: `s_av_${prefix}${decl.id.name}`,
          initCode: floatLit(initVal),
        });
      }
    }
  }

  return anims;
}

/**
 * Collects `const r = useRef(initial)` refs → Map (name → {cVar, cType, initCode, kind}). Two kinds:
 *  - VALUE ref (numeric initial): a mutable C slot (escape-hatch state that does NOT re-render;
 *    `.current` reads/writes).
 *  - NODE ref (`useRef()` / `useRef(null)`): holds an `ERNode*`, captured by `ref={r}` on an element and
 *    used as the target of imperative calls like updateVector(r, …). kind === 'node'.
 *
 * @param fnBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param prefix  The C name prefix: none for the App, `c<n>_` for an inlined instance.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The refs, by name.
 */
export function collectRefs(
  fnBody: t.BlockStatement | t.Expression,
  scope: Scope,
  prefix = '',
  wide: Set<string> = NO_WIDE,
): Map<string, RefRecord> {
  // Collect ref records by their JS binding name.
  const refs = new Map<string, RefRecord>();

  // Only block-bodied functions can contain ref declarations to collect.
  if (fnBody.type !== 'BlockStatement') return refs;

  // Collect useRef declarations, splitting null/empty node refs from numeric value refs.
  for (const stmt of fnBody.body) {
    // Only variable declarations can introduce bindings we need to inspect.
    if (stmt.type !== 'VariableDeclaration') continue;

    // Find useRef bindings and lower each supported initializer to either a node ref or value ref record.
    for (const decl of stmt.declarations) {
      const init = decl.init;
      if (
        init?.type === 'CallExpression' &&
        (init.callee as t.Identifier).name === 'useRef' &&
        decl.id.type === 'Identifier'
      ) {
        const arg = init.arguments[0];
        // A PanResponder ref — collectPanResponders owns it
        if (isPanCreate(arg)) continue;

        // Empty or null useRef values are node refs, initialized to no bound ERNode.
        const cVar = `s_ref_${prefix}${decl.id.name}`;
        if (!arg || arg.type === 'NullLiteral') {
          refs.set(decl.id.name, {
            cVar,
            cType: 'ERNode*',
            initCode: 'NULL',
            kind: 'node',
            used: false,
          });
          continue;
        }

        // Value refs must start from a finite compile-time number.
        const value = evalStaticOrThrow(
          arg,
          scope,
          `AOT: useRef initial for "${decl.id.name}" must be a number (value ref) or null/empty (node ref)`,
          'a value ref needs a compile-time number: useRef(0).',
        );
        if (typeof value !== 'number' || !Number.isFinite(value)) {
          const error = aotError(
            `AOT: useRef initial for "${decl.id.name}" must be a number (value ref) or null/empty (node ref)`,
            `got ${value === undefined ? 'undefined' : String(value)}.`,
          );
          if (arg.loc) {
            error.aotLoc = arg.loc.start;
          }
          throw error;
        }

        // A ref a timestamp is written to holds 64 bits (see compileWidened).
        const cType = !Number.isInteger(value)
          ? 'float'
          : wide.has(cVar)
            ? 'i64'
            : 'int';
        refs.set(decl.id.name, {
          cVar,
          cType,
          initCode:
            cType === 'float'
              ? floatLit(value)
              : cType === 'i64'
                ? i64Lit(value)
                : String(value),
          kind: 'value',
          used: false,
        });
      }
    }
  }

  return refs;
}

/**
 * The polynomial family of an `Easing.quad` / `Easing.cubic` node, for in/out/inOut composition.
 *
 * @param node  The easing expression.
 *
 * @returns 'QUAD' or 'CUBIC', or null for anything else.
 */
function easingFamily(node: t.Node | null | undefined): string | null {
  if (
    node?.type === 'MemberExpression' &&
    (node.object as t.Identifier)?.name === 'Easing'
  ) {
    if ((node.property as t.Identifier).name === 'quad') return 'QUAD';
    if ((node.property as t.Identifier).name === 'cubic') return 'CUBIC';
  }
  return null;
}

/**
 * Maps an `Easing.*` node → { ease: 'ER_EASE_*', bezier: [x1,y1,x2,y2] | null }. Handles the bare curves
 * (linear/ease/quad/cubic/bounce/elastic), the in/out/inOut wrappers around quad/cubic, and
 * Easing.bezier(x1,y1,x2,y2). No easing → ER_EASE_EASE_IN_OUT (RN's timing default); unknown → same.
 *
 * @param node  The `easing:` expression, if there is one.
 * @param env  The expression environment for folding bezier control points.
 *
 * @returns The engine easing, and its control points when it is a bezier.
 */
function easingInfo(
  node: t.Node | null | undefined,
  env: Env,
): {ease: string; bezier: number[] | null} {
  // Use React Native's timing default whenever no easing is supplied (or the expression is unsupported).
  const FALLBACK = {ease: 'ER_EASE_EASE_IN_OUT', bezier: null};
  if (!node) return FALLBACK;

  // Bare member: Easing.linear / Easing.ease / Easing.quad (== quad-in) / ...
  if (
    node.type === 'MemberExpression' &&
    (node.object as t.Identifier)?.name === 'Easing'
  ) {
    const map: Record<string, string> = {
      linear: 'ER_EASE_LINEAR',
      ease: 'ER_EASE_EASE',
      quad: 'ER_EASE_QUAD_IN',
      cubic: 'ER_EASE_CUBIC_IN',
      bounce: 'ER_EASE_BOUNCE_OUT',
      elastic: 'ER_EASE_ELASTIC_OUT',
    };
    return {
      ease: map[(node.property as t.Identifier).name] || 'ER_EASE_EASE_IN_OUT',
      bezier: null,
    };
  }

  // Call: Easing.bezier(...), Easing.elastic(n), Easing.in/out/inOut(inner)
  if (
    node.type === 'CallExpression' &&
    node.callee.type === 'MemberExpression' &&
    (node.callee.object as t.Identifier)?.name === 'Easing'
  ) {
    const fn = (node.callee.property as t.Identifier).name;
    if (fn === 'bezier') {
      const cps = node.arguments
        .slice(0, 4)
        .map(a => Number(evalStaticOr(a, env, 0)));
      return cps.length === 4
        ? {ease: 'ER_EASE_BEZIER', bezier: cps}
        : FALLBACK;
    }

    if (fn === 'elastic') {
      return {ease: 'ER_EASE_ELASTIC_OUT', bezier: null};
    }

    if (fn === 'in' || fn === 'out' || fn === 'inOut') {
      const fam = easingFamily(node.arguments[0]);
      const dir = fn === 'in' ? 'IN' : fn === 'out' ? 'OUT' : 'IN_OUT';
      return fam ? {ease: `ER_EASE_${fam}_${dir}`, bezier: null} : FALLBACK;
    }
  }

  return FALLBACK;
}

/**
 * Pushes `cfg.easing = …;` (and bezier control points for Easing.bezier) onto a timing config's C lines.
 *
 * @param lines  The config's C lines, appended to.
 * @param configName  The config variable's name.
 * @param easingNode  The `easing:` expression, if there is one.
 * @param env  The expression environment.
 */
function pushEasing(
  lines: string[],
  configName: string,
  easingNode: t.Node | undefined,
  env: Env,
): void {
  const {ease, bezier} = easingInfo(easingNode, env);
  lines.push(`        ${configName}.easing = ${ease};`);
  if (bezier) {
    lines.push(
      `        ${configName}.bezier_x1 = ${floatLit(bezier[0])}; ${configName}.bezier_y1 = ${floatLit(bezier[1])};`,
    );
    lines.push(
      `        ${configName}.bezier_x2 = ${floatLit(bezier[2])}; ${configName}.bezier_y2 = ${floatLit(bezier[3])};`,
    );
  }
}

/**
 * Parses a `.interpolate({ inputRange, outputRange, extrapolate })` config object → a static
 * { input, output, exLeft, exRight } descriptor (ranges must be static, equal-length, 2..8 points).
 *
 * @param cfgNode  The config argument.
 * @param env  The expression environment.
 *
 * @returns The folded interpolation.
 */
export function parseInterp(
  cfgNode: t.Node | null | undefined,
  env: Env,
): Interpolation {
  // Interpolation ranges must be known at compile time so the generated C can bake the breakpoints directly.
  if (cfgNode?.type !== 'ObjectExpression') {
    throw aotError(
      'AOT: .interpolate() needs a config object literal { inputRange, outputRange }',
    );
  }

  // Read a named property from the interpolation config, accepting both identifier and string-literal keys.
  const get = (key: string) =>
    (cfgNode.properties as t.ObjectProperty[]).find(
      property =>
        ((property.key as t.Identifier).name ??
          (property.key as t.StringLiteral).value) === key,
    )?.value;

  // Fold an interpolation range into numeric breakpoints; ranges must be array literals for AOT emission.
  const arr = (node: t.Node | undefined, name: string): number[] => {
    if (node?.type !== 'ArrayExpression') {
      throw aotError(`AOT: .interpolate() ${name} must be an array literal`);
    }
    return node.elements.map(e => Number(evalStatic(e!, env.consts ?? {})));
  };

  // inputRange and outputRange define the piecewise mapping and must line up point-for-point.
  const input = arr(get('inputRange'), 'inputRange');
  const output = arr(get('outputRange'), 'outputRange');
  if (input.length < 2 || input.length !== output.length) {
    throw aotError(
      'AOT: .interpolate() inputRange and outputRange must be the same length (>= 2)',
    );
  }
  if (input.length > 8) {
    throw aotError(
      'AOT: .interpolate() supports up to 8 breakpoints (ER_INTERPOLATE_MAX_POINTS)',
    );
  }

  // Convert RN extrapolation strings to engine enum names, defaulting to extend like React Native.
  const ex = (node: t.Node | undefined): string => {
    const extrapolateMode = node
      ? String(evalStaticOr(node, env, 'extend'))
      : 'extend';
    return extrapolateMode === 'clamp'
      ? 'ER_EXTRAPOLATE_CLAMP'
      : extrapolateMode === 'identity'
        ? 'ER_EXTRAPOLATE_IDENTITY'
        : 'ER_EXTRAPOLATE_EXTEND';
  };

  // RN's `extrapolate` applies to both sides; side-specific props override it.
  const both = get('extrapolate');
  return {
    input,
    output,
    exLeft: ex(get('extrapolateLeft') ?? both),
    exRight: ex(get('extrapolateRight') ?? both),
  };
}

/**
 * Builds a `get(key)` accessor over an `Animated.*(value, config)` call's config object literal.
 *
 * @param cfgObj  The config argument, if there is one.
 *
 * @returns The accessor; it reads undefined for every key when there is no config.
 */
function animConfigGetter(cfgObj: t.Node | null | undefined): ConfigGetter {
  return key =>
    (
      (cfgObj as t.ObjectExpression | undefined)?.properties as
        | t.ObjectProperty[]
        | undefined
    )?.find(
      property =>
        ((property.key as t.Identifier).name ??
          (property.key as t.StringLiteral).value) === key,
    )?.value;
}

/**
 * Emits one atomic animation (timing/spring/decay) → a scoped ERAnimConfig + er_anim_value_animate.
 * `delayMs` is the absolute start delay (how composition offsets are realized); `loop` repeats a timing;
 * `onCompleteCb` (optional) is a C function name set as cfg.on_complete — used to chain sequence steps.
 *
 * @param entry  The animation.
 * @param env  The expression environment.
 * @param idx  Makes the config variable's name unique within the body.
 * @param onCompleteCb  The C function to run when it completes, if any.
 *
 * @returns The C block that starts it.
 */
function emitAnimEntry(
  entry: AnimEntry,
  env: Env,
  idx: string | number,
  onCompleteCb: string | null | undefined,
): string[] {
  const {cVar, kind, get, delayMs, loop} = entry;

  // Each emitted animation gets its own scoped ERAnimConfig local, so chained/parallel starts don't reuse state.
  const configName = `cfg${idx}`;
  const lines = [
    '    {',
    `        ERAnimConfig ${configName};`,
    `        memset(&${configName}, 0, sizeof(${configName}));`,
  ];

  // Fill the type-specific config fields, using RN-like defaults when the app leaves a field out.
  if (kind === 'spring') {
    lines.push(`        ${configName}.type = ER_ANIM_SPRING;`);
    lines.push(
      `        ${configName}.stiffness = ${floatLit(evalStaticOr(get('stiffness'), env, 200))};`,
    );
    lines.push(
      `        ${configName}.damping = ${floatLit(evalStaticOr(get('damping'), env, 18))};`,
    );
    lines.push(
      `        ${configName}.mass = ${floatLit(evalStaticOr(get('mass'), env, 1))};`,
    );
  } else if (kind === 'decay') {
    lines.push(`        ${configName}.type = ER_ANIM_DECAY;`);
    lines.push(
      `        ${configName}.deceleration = ${floatLit(evalStaticOr(get('deceleration'), env, 0.998))};`,
    );
    lines.push(
      `        ${configName}.velocity = ${floatLit(evalStaticOr(get('velocity'), env, 0))};`,
    );
  } else {
    lines.push(`        ${configName}.type = ER_ANIM_TIMING;`);
    lines.push(
      `        ${configName}.duration_ms = ${Math.round(Number(evalStaticOr(get('duration'), env, 250)))};`,
    );
    pushEasing(lines, configName, get('easing'), env);
  }

  // Add composition/control fields shared by all animation kinds.
  if (delayMs > 0) {
    lines.push(`        ${configName}.delay_ms = ${delayMs};`);
  }
  if (loop) {
    lines.push(`        ${configName}.loop = true;`);
  }
  if (onCompleteCb) {
    lines.push(`        ${configName}.on_complete = ${onCompleteCb};`);
  }

  // Decay is velocity-driven and has no toValue target; every other type needs one.
  const toNode = get('toValue');
  if (!toNode && kind !== 'decay') {
    throw aotError(`AOT: Animated.${kind}() config needs a toValue`);
  }

  // Start the engine animation, using the current value as decay's placeholder target.
  const toCode = toNode
    ? emitExpr(toNode, env).code
    : `er_anim_value_get(${cVar})`;
  lines.push(
    `        er_anim_value_animate(${cVar}, (float)(${toCode}), &${configName});`,
    '    }',
  );

  return lines;
}

/**
 * Flattens an Animated composition (timing/spring/decay/sequence/parallel/stagger/delay/loop) into a flat
 * list of atomic entries, each with an ABSOLUTE start delay (ms) — composition is realized purely through
 * per-entry delay_ms (no engine grouping needed for standalone values). Returns { entries, duration } where
 * `duration` is this node's own run length in ms, used to offset later siblings in a sequence/stagger;
 * null = unknown (spring/decay/loop), which is illegal to sequence anything after.
 *
 * @param node  The composition.
 * @param env  The expression environment.
 * @param baseDelay  When the composition starts, in ms.
 * @param loop  The composition repeats forever (inside an Animated.loop).
 *
 * @returns Its atomic entries and its run length.
 */
function flattenAnim(
  node: t.Node | null | undefined,
  env: Env,
  baseDelay: number,
  loop: boolean,
): FlatAnimation {
  // Resolve a named animation handle, then require one of the supported Animated.* calls.
  node = resolveAnim(node, env);
  if (!isAnimatedCall(node)) {
    throw aotError(
      'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
    );
  }

  // Atomic animations drive one useAnimatedValue directly; timing has a known length, spring/decay do not.
  const kind = node.callee.property.name;
  const args = node.arguments;
  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    const valRef = args[0];
    if (valRef?.type !== 'Identifier' || !env.animations?.has(valRef.name)) {
      throw aotError(
        `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
      );
    }
    const cVar = env.animations.get(valRef.name)!.cVar;
    const get = animConfigGetter(args[1]);
    const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
    const duration =
      kind === 'timing'
        ? ownDelay + Math.round(Number(evalStaticOr(get('duration'), env, 250)))
        : null;
    return {
      entries: [{cVar, kind, get, delayMs: baseDelay + ownDelay, loop}],
      duration,
    };
  }

  // Animated.delay contributes time to a composition but starts no engine animation itself.
  if (kind === 'delay') {
    return {
      entries: [],
      duration: Math.round(Number(evalStaticOr(args[0], env, 0))),
    };
  }

  // Composition nodes flatten into atomic animations with absolute delay_ms offsets.
  if (kind === 'sequence' || kind === 'parallel' || kind === 'stagger') {
    const list = kind === 'stagger' ? args[1] : args[0];
    const staggerMs =
      kind === 'stagger'
        ? Math.round(Number(evalStaticOr(args[0], env, 0)))
        : 0;
    if (list?.type !== 'ArrayExpression') {
      throw aotError(`AOT: Animated.${kind}(...) needs an array of animations`);
    }

    // Sequence advances its running offset; parallel/stagger start children from the same base with offsets.
    const entries: AnimEntry[] = [];
    let off = baseDelay; // running offset (sequence)
    let groupDur = 0; // max end-time relative to baseDelay (parallel/stagger)
    let index = 0;
    for (const child of list.elements) {
      if (!child) continue;
      const start = kind === 'sequence' ? off : baseDelay + index * staggerMs;
      const flatAnim: FlatAnimation = flattenAnim(child, env, start, loop);
      entries.push(...flatAnim.entries);
      if (kind === 'sequence') {
        if (flatAnim.duration == null) {
          throw aotError(
            'AOT: an Animated.sequence entry needs a known duration — use Animated.timing / Animated.delay (a spring/decay/loop inside a sequence is not supported; it has no fixed length to offset the next entry by)',
          );
        }
        off += flatAnim.duration;
      } else {
        const end = start - baseDelay + (flatAnim.duration ?? 0);
        if (end > groupDur) {
          groupDur = end;
        }
      }

      index++;
    }

    // The same value can't appear twice in a flat (delay_ms) composition: er_anim_value_animate cancels the
    // running anim on a value, so concurrent/flat-sequenced same-value steps cancel each other. (A
    // top-level Animated.sequence is handled separately via on_complete chaining, which does support this.)
    const seen = new Set<string>();
    for (const entry of entries) {
      if (seen.has(entry.cVar)) {
        throw aotError(
          'AOT: the same animated value is driven more than once in this composition — concurrent/flat same-value steps cancel each other. Use a top-level Animated.sequence(...) for multi-step animation of one value.',
        );
      }
      seen.add(entry.cVar);
    }
    return {
      entries,
      duration: kind === 'sequence' ? off - baseDelay : groupDur,
    };
  }

  // A nested loop can flatten only when it wraps a single atomic animation; complex loops need start-time chaining.
  if (kind === 'loop') {
    const flatAnim: FlatAnimation = flattenAnim(args[0], env, baseDelay, true);
    if (flatAnim.entries.length !== 1) {
      throw aotError(
        'AOT: an Animated.loop inside another composition can only wrap a single Animated.timing/spring/decay',
        'a loop around a sequence works when the loop is the animation you start: Animated.loop(Animated.sequence([...])).start().',
      );
    }
    return {entries: flatAnim.entries, duration: null};
  }

  throw aotError(`AOT: Animated.${kind}(...) is not a supported animation`);
}

/**
 * True for an `Animated.timing/spring/…/loop(…)` call: an animation, not a value.
 *
 * @param node  Any node, or nothing.
 *
 * @returns Whether it is such a call.
 */
export const isAnimatedCall = (
  node: t.Node | null | undefined,
): node is AnimatedCall =>
  node?.type === 'CallExpression' &&
  node.callee.type === 'MemberExpression' &&
  (node.callee.object as t.Identifier)?.name === 'Animated' &&
  ANIM_KINDS.has((node.callee.property as t.Identifier)?.name);

/**
 * An animation handle (`anim` after `const anim = Animated.loop(…)`) resolved to the call it names.
 *
 * @param node  An animation, or a handler local bound to one.
 * @param env  The expression environment.
 *
 * @returns The animation the node stands for.
 */
export const resolveAnim = (
  node: t.Node | null | undefined,
  env: Env,
): t.Node | null | undefined =>
  node?.type === 'Identifier' && env.animLocals?.has(node.name)
    ? env.animLocals!.get(node.name)
    : node;

/**
 * One Animated.timing/spring/decay call as a chain step, started `extraDelay` ms later than its own delay.
 *
 * @param node  The call.
 * @param env  The expression environment.
 * @param extraDelay  Milliseconds added to its own delay.
 *
 * @returns The step.
 */
function animStep(node: AnimatedCall, env: Env, extraDelay: number): AnimEntry {
  const kind = node.callee.property.name;

  // A chain step must drive one collected useAnimatedValue.
  const valRef = node.arguments[0];
  if (valRef?.type !== 'Identifier' || !env.animations?.has(valRef.name)) {
    throw aotError(
      `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
    );
  }

  // Fold this step's own delay and add any delay inherited from an enclosing sequence/delay.
  const get = animConfigGetter(node.arguments[1]);
  const ownDelay = Math.round(Number(evalStaticOr(get('delay'), env, 0)));
  return {
    cVar: env.animations.get(valRef.name)!.cVar,
    kind,
    get,
    delayMs: extraDelay + ownDelay,
    loop: false,
  };
}

/**
 * The C handle of every value an animation drives.
 *
 * @param node  The animation.
 * @param env  The expression environment.
 * @param accumulator  Collects the handles.
 *
 * @returns `accumulator`.
 */
export function animValues(
  node: t.Node | null | undefined,
  env: Env,
  accumulator: Set<string> = new Set(),
): Set<string> {
  node = resolveAnim(node, env);

  // Reject props that the FlatList-to-ScrollView rewrite cannot preserve.
  if (!isAnimatedCall(node)) {
    throw aotError(
      'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
    );
  }

  // Recurse into composed animations, skipping delay nodes because they drive no animated value.
  const kind = node.callee.property.name;
  const args = node.arguments;
  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    accumulator.add(animStep(node, env, 0).cVar);
  } else if (kind === 'loop') {
    animValues(args[0], env, accumulator);
  } else if (kind !== 'delay') {
    const list = kind === 'stagger' ? args[1] : args[0];
    for (const child of (list as t.ArrayExpression | undefined)?.elements ??
      []) {
      if (child) {
        animValues(child, env, accumulator);
      }
    }
  }

  return accumulator;
}

/**
 * The steps of an `Animated.sequence([...])` for an on_complete chain. A delay() folds into the next step's
 * delay_ms; `trailingDelay` is what follows the last step. A nested parallel/stagger/loop throws.
 *
 * @param seqNode  The sequence call.
 * @param env  The expression environment.
 *
 * @returns The steps, and the delay after the last one.
 */
function sequenceSteps(
  seqNode: AnimatedCall,
  env: Env,
): {steps: AnimEntry[]; trailingDelay: number} {
  const list = seqNode.arguments[0];

  // Sequence composition is lowered at build time, so its children must be an array literal.
  if (list?.type !== 'ArrayExpression') {
    throw aotError('AOT: Animated.sequence(...) needs an array of animations');
  }

  // Fold delay entries into the next real animation step so the sequence can be emitted as an on-complete chain.
  const steps: AnimEntry[] = [];
  let pendingDelay = 0;
  for (const element of list.elements) {
    if (!element) continue;
    const child = resolveAnim(element, env);

    // Reject non-animation entries in the sequence.
    if (!isAnimatedCall(child)) {
      throw aotError(
        'AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay(...)',
      );
    }

    // Fold delay-only entries into the following animation step.
    const kind = child.callee.property.name;
    if (kind === 'delay') {
      pendingDelay += Math.round(
        Number(evalStaticOr(child.arguments[0], env, 0)),
      );
      continue;
    }

    // Keep sequence chains flat: delay plus timing/spring/decay only.
    if (kind !== 'timing' && kind !== 'spring' && kind !== 'decay') {
      throw aotError(
        'AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay (a nested ' +
          'parallel/stagger/loop inside a sequence is not yet supported — keep the sequence flat)',
      );
    }

    steps.push(animStep(child, env, pendingDelay));
    pendingDelay = 0;
  }

  return {steps, trailingDelay: pendingDelay};
}

/**
 * Emits steps as an on_complete CHAIN: step 0 starts inline, and each step's completion callback starts the
 * next. Unlike the flat delay_ms path, this is correct when steps share a value — er_anim_value_animate
 * cancels the running anim on a value, so starting them together would cancel all but the last — and needs
 * no fixed duration. A step that is interrupted (finished == false) ends the chain, as it does in JS.
 *
 * `loop` makes the last step start the first again: {iterations (negative = forever), resetTo (the value to
 * put back where it started before each repeat, or null), trailingDelay (ms before each repeat)}.
 *
 * @param steps  The chain's steps.
 * @param env  The expression environment.
 * @param ctx  The statement compiler's context.
 * @param doneCb  The completion callback to run when the chain ends, if any.
 * @param loop  How the chain repeats, or null for once.
 *
 * @returns The C that starts step 0.
 */
function emitAnimChain(
  steps: AnimEntry[],
  env: Env,
  ctx: StatementContext,
  doneCb: string | null,
  loop: LoopSpec | null = null,
): string[] {
  // GLOBAL: callback names are file-scope, so must be unique across handlers
  const seqId = ctx.out.seqN++;
  const cb = (i: number) => `er_seqcb_${seqId}_${i}`;
  const done = (finished: string) =>
    doneCb ? [`        ${doneCb}(${finished}, NULL);`] : [];
  const interrupted = [
    '    if (!finished)',
    '    {',
    ...done('false'),
    '        return;',
    '    }',
  ];

  // Built from the tail so each step knows its successor's callback name.
  const last = loop ? cb(0) : doneCb;
  for (let index = steps.length - 1; index >= 1; index--) {
    const next = index < steps.length - 1 ? cb(index + 1) : last;
    ctx.out.animCbs.push({
      name: cb(index),
      body: [
        ...interrupted,
        ...emitAnimEntry(steps[index], env, `${seqId}_${index}`, next),
      ],
    });
  }

  // Start the first step inline; it either chains to step 1 or to the final completion callback.
  const next0 = steps.length > 1 ? cb(1) : last;
  const first = emitAnimEntry(steps[0], env, `${seqId}_0`, next0);
  if (!loop) return first;

  // The repeat: cb(0) runs when the last step finishes and starts step 0 again.
  const lines: string[] = [];
  const repeat = [...interrupted];
  if (loop.iterations >= 0) {
    const passCount = `s_loop${seqId}_n`;
    ctx.out.effectDecls.push(`static int ${passCount};`);
    lines.push(`    ${passCount} = 1;`);
    repeat.push(
      `    if (${passCount} >= ${loop.iterations})`,
      '    {',
      ...done('true'),
      '        return;',
      '    }',
      `    ${passCount}++;`,
    );
  }

  // Capture the value at loop start so each repeat can resetBeforeIteration back to it.
  if (loop.resetTo) {
    const from = `s_loop${seqId}_from`;
    ctx.out.effectDecls.push(`static float ${from};`);
    lines.push(`    ${from} = er_anim_value_get(${loop.resetTo});`);
    repeat.push(`    er_anim_value_set(${loop.resetTo}, ${from});`);
  }

  // Restart from step 0, adding any trailing sequence delay before the next iteration.
  const again = {...steps[0], delayMs: steps[0].delayMs + loop.trailingDelay};
  repeat.push(...emitAnimEntry(again, env, `${seqId}_0`, next0));
  ctx.out.animCbs.push({name: cb(0), body: repeat});

  return [...lines, ...first];
}

/**
 * Compiles `Animated.loop(animation, config?).start()`, or returns null for the flat path. An endless loop of
 * one timing is the engine's own cfg.loop; anything else (a sequence, a spring or decay, a counted loop) is a
 * chain whose last step starts the first again. As in JS, a looped single animation goes back to where it
 * started before each repeat (resetBeforeIteration), and a looped sequence carries on from where it ended.
 *
 * @param loopNode  The `Animated.loop(…)` call.
 * @param env  The expression environment.
 * @param ctx  The statement compiler's context.
 * @param doneCb  The completion callback to run when the loop ends, if any.
 *
 * @returns The C that starts it, or null when the flat path (the engine's own cfg.loop) handles it.
 */
function compileLoopStart(
  loopNode: AnimatedCall,
  env: Env,
  ctx: StatementContext,
  doneCb: string | null,
): string[] | null {
  // Fold the loop count at build time; absent or noninteger iterations mean repeat forever.
  const inner = resolveAnim(loopNode.arguments[0], env);
  const kind = isAnimatedCall(inner) ? inner.callee.property.name : null;
  const get = animConfigGetter(loopNode.arguments[1]);
  let iterations = -1;
  if (get('iterations')) {
    let value: unknown;
    try {
      value = evalStatic(get('iterations')!, foldScope(env, env.consts ?? {}));
    } catch {
      throw aotError(
        'AOT: Animated.loop iterations must be known at build time',
        'use a number or a module-level constant.',
      );
    }

    // JS loops forever on anything but a whole number.
    iterations = Number.isInteger(value)
      ? Math.min(value as number, INT_MAX)
      : -1;
  }

  // Use the engine's native loop only for endless reset timing loops; other loop shapes need callback chaining.
  const reset = evalStaticOr(get('resetBeforeIteration'), env, true) !== false;
  let steps: AnimEntry[];
  let trailingDelay = 0;
  let resetTo: string | null = null;
  if (kind === 'sequence') {
    ({steps, trailingDelay} = sequenceSteps(inner as AnimatedCall, env));
  } else if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    if (kind === 'timing' && iterations < 0 && reset) return null;
    steps = [animStep(inner as AnimatedCall, env, 0)];
    if (reset) {
      resetTo = steps[0].cVar;
    }
  } else {
    throw aotError(
      'AOT: Animated.loop can repeat a timing, spring, decay or sequence (looping a parallel/stagger is not yet supported)',
    );
  }

  // Nothing to start for an empty sequence.
  if (!steps.length) {
    return [];
  }

  // A zero-iteration loop starts nothing but still reports successful completion.
  if (iterations === 0) {
    return doneCb ? [`    ${doneCb}(true, NULL);`] : [];
  }

  // A timing with no duration and no delay completes inside er_anim_value_animate, so a loop made only of
  // those would restart itself forever without returning.
  const instant = (animEntry: AnimEntry) =>
    animEntry.kind === 'timing' &&
    animEntry.delayMs === 0 &&
    Math.round(Number(evalStaticOr(animEntry.get('duration'), env, 250))) === 0;
  if (trailingDelay === 0 && steps.every(instant)) {
    throw aotError(
      'AOT: every step of this Animated.loop finishes instantly, so it would never stop repeating',
      'give a step a duration or a delay.',
    );
  }

  return emitAnimChain(steps, env, ctx, doneCb, {
    iterations,
    resetTo,
    trailingDelay,
  });
}

/**
 * Compiles a `.start(onComplete)` completion callback to a file-scope C fn (ERAnimCompleteFn) set as the
 * animation's on_complete; its body runs setters/refs/etc. and re-applies state via app_update if needed.
 *
 * @param fnNode  The callback.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The C function's name.
 */
function emitCompletionCb(
  fnNode: FunctionNode,
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): string {
  // Generate a file-scope completion callback and expose start(({finished}) => ...) as a local C int.
  const id = ctx.out.seqN++;
  const name = `er_donecb_${id}`;
  const locals = new Map<string, Local>(env.locals);
  const param = fnNode.params[0];
  if (param?.type === 'ObjectPattern') {
    for (const property of param.properties as t.ObjectProperty[]) {
      if (
        ((property.key as t.Identifier)?.name ??
          (property.key as t.StringLiteral)?.value) === 'finished'
      ) {
        locals.set((property.value as t.Identifier)?.name ?? 'finished', {
          code: 'finished',
          cType: 'int',
        });
      }
    }
  }

  // Compile the callback body as statements, then re-render if it changed React state.
  const body = fnNode.body;
  const list: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  const cctx: StatementContext = {
    stateChanged: false,
    animIdx: 0,
    out: ctx.out,
  };
  const lines = compileStmts(list, {...env, locals}, state, cctx, '    ');
  if (cctx.stateChanged) {
    lines.push(APP_UPDATE_CALL);
  }

  ctx.out.animCbs.push({name, body: lines});

  return name;
}

/**
 * Compiles `<animation>.start()` — a single Animated.timing/spring/decay or a composition
 * (sequence/parallel/stagger/delay/loop). A sequence, and a loop the engine cannot repeat on its own, become
 * an on_complete chain (emitAnimChain); everything else flattens to one ERAnimConfig + er_anim_value_animate
 * per atomic entry, composition expressed through per-entry delay_ms. Native-driven; sets no React state.
 *
 * @param expr  The `.start(…)` call.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The C that starts the animation.
 */
export function compileAnimateStart(
  expr: t.CallExpression,
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): string[] {
  // Resolve the animation being started and handle cases that need callback chaining before falling back to flat emission.
  const doneCb = isFn(expr.arguments[0])
    ? emitCompletionCb(expr.arguments[0], env, state, ctx)
    : null;
  const receiver = resolveAnim((expr.callee as t.MemberExpression).object, env);
  const kind = isAnimatedCall(receiver) ? receiver.callee.property.name : null;
  if (kind === 'sequence') {
    const {steps} = sequenceSteps(receiver as AnimatedCall, env);
    return steps.length ? emitAnimChain(steps, env, ctx, doneCb) : [];
  }
  if (kind === 'loop') {
    const lines = compileLoopStart(receiver as AnimatedCall, env, ctx, doneCb);
    if (lines) return lines;
  }

  // The flat path has no "after all entries finish" join point, so completion callbacks are limited to one entry.
  const {entries} = flattenAnim(receiver, env, 0, false);
  if (doneCb && entries.length > 1) {
    throw aotError(
      'AOT: a .start(onComplete) callback on a parallel/stagger animation is not yet supported',
      'attach the completion callback to a single animation or an Animated.sequence(...). For "after all parallel anims", restructure as a sequence.',
    );
  }

  // Emit each flattened entry independently; when allowed, attach completion to the final emitted entry.
  const lines: string[] = [];
  entries.forEach((animEntry: AnimEntry, index) =>
    lines.push(
      ...emitAnimEntry(
        animEntry,
        env,
        ctx.animIdx++,
        index === entries.length - 1 ? doneCb : null,
      ),
    ),
  );

  return lines;
}
