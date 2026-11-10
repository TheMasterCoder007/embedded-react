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

import {check, checkNumber} from './check.mts';
import {aotError} from './diagnostics.mts';
import {
  evalStaticOrThrow,
  evalStatic,
  foldScope,
  evalStaticOr,
} from './static-eval.mts';
import {INT_MAX, lowerExpr} from './expressions.mts';
import {NO_WIDE, isFn, wideInitial} from './collect.mts';
import {lowerBody} from './handlers.mts';
import {isPanCreate} from './pan-responder.mts';
import {parseColorValue} from '../style-map.mts';
import type * as t from '@babel/types';
import type {SlotWidths} from './ir/types.mts';
import type {
  IrAnimation,
  IrAnimationStart,
  IrAnimCurve,
  IrCompleteAfter,
  IrCompletion,
  IrCompletionWait,
  IrStartAnimations,
  IrStartChain,
} from './ir/animations.mts';
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
  /** The animated value's handle. */
  slotName: string;
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
  /** The numeric outputs; empty when the outputRange holds colors. */
  output: number[];
  /** ARGB8888 outputs, when the outputRange holds colors. */
  colors?: number[];
  extrapolateLeft: string;
  extrapolateRight: string;
}

/** How an on_complete chain repeats (see lowerChain). */
interface LoopSpec {
  /** Repeats; negative loops forever. */
  iterations: number;
  /** The value to put back where it started before each repeat, or null. */
  resetTo: string | null;
  /** Milliseconds before each repeat, and after the last pass before the loop completes. */
  trailingDelay: number;
  /** The loop's composition key, which the wait after its last pass belongs to. */
  key: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The style keys an animated value drives only through an interpolation onto colors. */
export const ANIMATED_COLOR_KEYS = new Set(['backgroundColor', 'color']);

/** Style key → the ERAnimationProp(s) an animated value binds to. */
export const ANIMATED_STYLE_PROPS: Record<string, string[]> = {
  opacity: ['ER_PROP_OPACITY'],
  backgroundColor: ['ER_PROP_BACKGROUND_COLOR'],
  color: ['ER_PROP_COLOR'],
};

/** Transform key → the ERAnimationProp(s) an animated value binds to. */
export const ANIMATED_TRANSFORM_PROPS: Record<string, string[]> = {
  scale: ['ER_PROP_SCALE_X', 'ER_PROP_SCALE_Y'],
  scaleX: ['ER_PROP_SCALE_X'],
  scaleY: ['ER_PROP_SCALE_Y'],
  translateX: ['ER_PROP_TRANSLATE_X'],
  translateY: ['ER_PROP_TRANSLATE_Y'],
  rotate: ['ER_PROP_ROTATE_Z'],
  rotateZ: ['ER_PROP_ROTATE_Z'],
};

const ANIMATION_KINDS = new Set([
  'timing',
  'spring',
  'decay',
  'sequence',
  'parallel',
  'stagger',
  'delay',
  'loop',
]);

// What an Animated config's field is when the app leaves it out, as React Native has it.
const DEFAULT_TIMING_DURATION_MS = 250;
const DEFAULT_SPRING_STIFFNESS = 200;
const DEFAULT_SPRING_DAMPING = 18;
const DEFAULT_DECAY_DECELERATION = 0.998;

/** The numbers Easing.bezier takes: x1, y1, x2, y2. */
const BEZIER_CONTROL_POINTS = 4;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Collects `const x = useAnimatedValue(initial)` → Map (name → {slotName, initial}). `prefix` namespaces the
 * handle (`anim_<prefix><name>`) so each inlined child instance gets its own engine value handle.
 *
 * @param functionBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param prefix  The name prefix: none for the App, `c<n>_` for an inlined instance.
 *
 * @returns The animated values, by name.
 */
export function collectAnims(
  functionBody: t.BlockStatement | t.Expression,
  scope: Scope,
  prefix = '',
): Map<string, AnimRecord> {
  // Collect animated value records by their JS binding name.
  const animations = new Map<string, AnimRecord>();

  // Only block-bodied functions can contain hook declarations to collect.
  if (functionBody.type !== 'BlockStatement') return animations;

  // Scan top-level declarations in the function body for useAnimatedValue hooks to allocate animated slots.
  for (const statement of functionBody.body) {
    // Only variable declarations can introduce bindings we need to inspect.
    if (statement.type !== 'VariableDeclaration') continue;

    // Collect useAnimatedValue declarations and register their engine-side animated slots.
    for (const declarator of statement.declarations) {
      const initializer = declarator.init;
      if (
        initializer?.type === 'CallExpression' &&
        (initializer.callee as t.Identifier).name === 'useAnimatedValue' &&
        declarator.id.type === 'Identifier'
      ) {
        const initialValue = initializer.arguments[0]
          ? evalStaticOrThrow(
              initializer.arguments[0],
              scope,
              `AOT: the initial value of useAnimatedValue "${declarator.id.name}" must be a compile-time constant`,
              'give it a finite starting number: useAnimatedValue(0).',
            )
          : 0;
        // An animated value is a float slot, which null or a non-finite number doesn't fit.
        // Refuse here, at the declaration, with its location.
        if (
          typeof initialValue !== 'number' ||
          !Number.isFinite(initialValue)
        ) {
          const error = aotError(
            `AOT: the initial value of useAnimatedValue "${declarator.id.name}" is ${initialValue === undefined ? 'undefined' : String(initialValue)}`,
            'give it a finite starting number: useAnimatedValue(0).',
          );
          if (initializer.arguments[0]?.loc) {
            error.aotLoc = initializer.arguments[0].loc.start;
          }
          throw error;
        }
        checkNumber(initialValue);
        animations.set(declarator.id.name, {
          slotName: `anim_${prefix}${declarator.id.name}`,
          initial: initialValue,
        });
      }
    }
  }

  return animations;
}

/**
 * Collects `const r = useRef(initial)` refs → Map (name → {slotName, slotKind, initial, kind}). Two kinds:
 *  - VALUE ref (numeric initial): a mutable slot (escape-hatch state that does NOT re-render;
 *    `.current` reads/writes).
 *  - NODE ref (`useRef()` / `useRef(null)`): holds an node handle, captured by `ref={r}` on an element and
 *    used as the target of imperative calls like updateVector(r, …). kind === 'node'.
 *
 * @param functionBody  The component's body.
 * @param scope  Compile-time constants the initial values fold against.
 * @param prefix  The name prefix: none for the App, `c<n>_` for an inlined instance.
 * @param wide  Slots to declare 64-bit (see compileWidened).
 *
 * @returns The refs, by name.
 */
export function collectRefs(
  functionBody: t.BlockStatement | t.Expression,
  scope: Scope,
  prefix = '',
  wide: SlotWidths = NO_WIDE,
): Map<string, RefRecord> {
  // Collect ref records by their JS binding name.
  const refs = new Map<string, RefRecord>();

  // Only block-bodied functions can contain ref declarations to collect.
  if (functionBody.type !== 'BlockStatement') return refs;

  // Collect useRef declarations, splitting null/empty node refs from numeric value refs.
  for (const statement of functionBody.body) {
    // Only variable declarations can introduce bindings we need to inspect.
    if (statement.type !== 'VariableDeclaration') continue;

    // Find useRef bindings and lower each supported initializer to either a node ref or value ref record.
    for (const declarator of statement.declarations) {
      const initializer = declarator.init;
      if (
        initializer?.type === 'CallExpression' &&
        (initializer.callee as t.Identifier).name === 'useRef' &&
        declarator.id.type === 'Identifier'
      ) {
        const argument = initializer.arguments[0];
        // A PanResponder ref — collectPanResponders owns it
        if (isPanCreate(argument)) continue;

        // Empty or null useRef values are node refs, initialized to no bound ERNode.
        const slotName = `ref_${prefix}${declarator.id.name}`;
        if (!argument || argument.type === 'NullLiteral') {
          refs.set(declarator.id.name, {
            slotName,
            slotKind: 'node',
            kind: 'node',
            used: false,
          });
          continue;
        }

        // Value refs must start from a finite compile-time number.
        const value = evalStaticOrThrow(
          argument,
          scope,
          `AOT: useRef initial for "${declarator.id.name}" must be a number (value ref) or null/empty (node ref)`,
          'a value ref needs a compile-time number: useRef(0).',
        );
        if (typeof value !== 'number' || !Number.isFinite(value)) {
          const error = aotError(
            `AOT: useRef initial for "${declarator.id.name}" must be a number (value ref) or null/empty (node ref)`,
            `got ${value === undefined ? 'undefined' : String(value)}.`,
          );
          if (argument.loc) {
            error.aotLoc = argument.loc.start;
          }
          throw error;
        }

        // A ref a timestamp or a number past the int range is written to hold 64 bits (see compileWidened).
        const initialWidth = wideInitial(value);
        const widened =
          Number.isInteger(value) && initialWidth !== 'float'
            ? (wide.get(slotName) ??
              (initialWidth === 'number' ? 'number' : undefined))
            : undefined;
        const slotKind =
          !Number.isInteger(value) || initialWidth === 'float'
            ? 'float'
            : widened
              ? 'i64'
              : 'int';
        if (slotKind === 'float') {
          checkNumber(value);
        }
        refs.set(declarator.id.name, {
          slotName,
          slotKind,
          initial: value,
          kind: 'value',
          used: false,
          ...(widened ? {widened} : {}),
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
 * @param environment  The expression environment for folding bezier control points.
 *
 * @returns The engine easing, and its control points when it is a bezier.
 */
function easingInfo(
  node: t.Node | null | undefined,
  environment: Env,
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
    const easing = (node.callee.property as t.Identifier).name;
    if (easing === 'bezier') {
      const controlPoints = node.arguments
        .slice(0, BEZIER_CONTROL_POINTS)
        .map(argument => Number(evalStaticOr(argument, environment, 0)));
      return controlPoints.length === BEZIER_CONTROL_POINTS
        ? {ease: 'ER_EASE_BEZIER', bezier: controlPoints}
        : FALLBACK;
    }

    if (easing === 'elastic') {
      return {ease: 'ER_EASE_ELASTIC_OUT', bezier: null};
    }

    if (easing === 'in' || easing === 'out' || easing === 'inOut') {
      const family = easingFamily(node.arguments[0]);
      const direction =
        easing === 'in' ? 'IN' : easing === 'out' ? 'OUT' : 'IN_OUT';
      return family
        ? {ease: `ER_EASE_${family}_${direction}`, bezier: null}
        : FALLBACK;
    }
  }

  return FALLBACK;
}

/**
 * Parses a `.interpolate({ inputRange, outputRange, extrapolate })` config object → a static
 * { input, output, exLeft, exRight } descriptor (ranges must be static, equal-length, 2..8 points).
 *
 * @param configNode  The config argument.
 * @param environment  The expression environment.
 *
 * @returns The folded interpolation.
 */
export function parseInterp(
  configNode: t.Node | null | undefined,
  environment: Env,
): Interpolation {
  // Interpolation ranges must be known at compile time so the package can bake the breakpoints directly.
  if (configNode?.type !== 'ObjectExpression') {
    throw aotError(
      'AOT: .interpolate() needs a config object literal { inputRange, outputRange }',
    );
  }

  // Read a named property from the interpolation config, accepting both identifier and string-literal keys.
  const get = (key: string) =>
    (configNode.properties as t.ObjectProperty[]).find(
      property =>
        ((property.key as t.Identifier).name ??
          (property.key as t.StringLiteral).value) === key,
    )?.value;

  // Fold an interpolation range into its breakpoints; ranges must be array literals for AOT emission.
  const arr = (node: t.Node | undefined, name: string): unknown[] => {
    if (node?.type !== 'ArrayExpression') {
      throw aotError(`AOT: .interpolate() ${name} must be an array literal`);
    }
    return node.elements.map(element =>
      evalStatic(element!, environment.constants ?? {}),
    );
  };

  // inputRange and outputRange define the piecewise mapping and must line up point-for-point. An
  // outputRange of strings maps onto colors.
  const input = arr(get('inputRange'), 'inputRange').map(Number);
  const outputValues = arr(get('outputRange'), 'outputRange');
  // Colors parse as strictly as a static style color, so a typo is an error rather than black.
  const colors = outputValues.every(value => typeof value === 'string')
    ? outputValues.map(color => {
        try {
          return parseColorValue(color);
        } catch (error) {
          throw aotError(
            `AOT: .interpolate() outputRange has an ${(error as Error).message}`,
          );
        }
      })
    : undefined;
  if (!colors && outputValues.some(value => typeof value !== 'number')) {
    throw aotError(
      'AOT: .interpolate() outputRange must be all numbers or all colors',
    );
  }
  const output = colors ? [] : (outputValues as number[]);
  if (input.length < 2 || input.length !== outputValues.length) {
    throw aotError(
      'AOT: .interpolate() inputRange and outputRange must be the same length (>= 2)',
    );
  }
  check({kind: 'interpolationPoints', count: input.length});

  // Convert RN extrapolation strings to engine enum names, defaulting to extend like React Native.
  const ex = (node: t.Node | undefined): string => {
    const extrapolateMode = node
      ? String(evalStaticOr(node, environment, 'extend'))
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
    ...(colors && {colors}),
    extrapolateLeft: ex(get('extrapolateLeft') ?? both),
    extrapolateRight: ex(get('extrapolateRight') ?? both),
  };
}

/**
 * Builds a `get(key)` accessor over an `Animated.*(value, config)` call's config object literal.
 *
 * @param configObject  The config argument, if there is one.
 *
 * @returns The accessor; it reads undefined for every key when there is no config.
 */
function animConfigGetter(
  configObject: t.Node | null | undefined,
): ConfigGetter {
  return key =>
    (
      (configObject as t.ObjectExpression | undefined)?.properties as
        t.ObjectProperty[] | undefined
    )?.find(
      property =>
        ((property.key as t.Identifier).name ??
          (property.key as t.StringLiteral).value) === key,
    )?.value;
}

/**
 * A folded config number that will be printed as a float, checked now so a value a float can't hold is reported
 * where the animation is lowered.
 *
 * @param rawValue  The folded value.
 *
 * @returns The number.
 */
function configFloat(rawValue: unknown): number {
  checkNumber(rawValue);
  return Number(rawValue);
}

/**
 * A folded config time in whole milliseconds, checked so a time too long to hold is reported at the field.
 *
 * @param node  The field's value, if the app wrote one.
 * @param environment  The expression environment.
 * @param defaultValue  The time when the field is left out or not known at build time.
 * @param field  What the time is, for the error.
 *
 * @returns The time.
 */
function configMs(
  node: t.Node | null | undefined,
  environment: Env,
  defaultValue: number,
  field: string,
): number {
  const rawValue = evalStaticOr(node, environment, defaultValue);
  check({kind: 'milliseconds', field, value: rawValue}, node?.loc?.start);
  return Math.round(Number(rawValue));
}

/**
 * A timing's folded duration (250 ms when it is left out, as in React Native).
 *
 * @param get  The timing's config.
 * @param environment  The expression environment.
 *
 * @returns The duration.
 */
const timingDuration = (get: ConfigGetter, environment: Env): number =>
  configMs(
    get('duration'),
    environment,
    DEFAULT_TIMING_DURATION_MS,
    `Animated.timing's "duration"`,
  );

/**
 * An animation's own delay (0 when it is left out).
 *
 * @param kind  timing, spring or decay.
 * @param get  The animation's config.
 * @param environment  The expression environment.
 *
 * @returns The delay.
 */
const ownDelayMs = (
  kind: string,
  get: ConfigGetter,
  environment: Env,
): number =>
  configMs(get('delay'), environment, 0, `Animated.${kind}'s "delay"`);

/**
 * A time added up from a composition's times, checked like them: each fits an int, but a sum of them need not.
 *
 * @param value  The sum.
 *
 * @returns The sum.
 */
function totalMs(value: number): number {
  check({kind: 'milliseconds', field: "this animation's total time", value});
  return value;
}

/**
 * Lowers one atomic animation (timing/spring/decay). `delayMs` is the absolute start delay (how composition
 * offsets are realized) and `loop` repeats a timing on the engine's own loop.
 *
 * @param entry  The animation.
 * @param environment  The expression environment.
 *
 * @returns The animation.
 */
function lowerAnimation(entry: AnimEntry, environment: Env): IrAnimation {
  const {slotName, kind, get, delayMs, loop} = entry;

  // Fold the curve's fields, using RN-like defaults when the app leaves one out.
  let curve: IrAnimCurve;
  if (kind === 'spring') {
    curve = {
      kind: 'spring',
      stiffness: configFloat(
        evalStaticOr(get('stiffness'), environment, DEFAULT_SPRING_STIFFNESS),
      ),
      damping: configFloat(
        evalStaticOr(get('damping'), environment, DEFAULT_SPRING_DAMPING),
      ),
      mass: configFloat(evalStaticOr(get('mass'), environment, 1)),
    };
  } else if (kind === 'decay') {
    curve = {
      kind: 'decay',
      deceleration: configFloat(
        evalStaticOr(
          get('deceleration'),
          environment,
          DEFAULT_DECAY_DECELERATION,
        ),
      ),
      velocity: configFloat(evalStaticOr(get('velocity'), environment, 0)),
    };
  } else {
    const durationMs = timingDuration(get, environment);
    const {ease, bezier} = easingInfo(get('easing'), environment);
    curve = {
      kind: 'timing',
      durationMs,
      easing: ease,
      bezier: bezier ? bezier.map(configFloat) : null,
    };
  }

  // Decay is velocity-driven and has no toValue target; every other type needs one.
  const toNode = get('toValue');
  if (!toNode && kind !== 'decay') {
    throw aotError(`AOT: Animated.${kind}() config needs a toValue`);
  }

  return {
    handle: slotName,
    curve,
    to: toNode ? lowerExpr(toNode, environment) : null,
    delayMs: totalMs(delayMs),
    isLooping: loop,
  };
}

/**
 * Flattens an Animated composition (timing/spring/decay/sequence/parallel/stagger/delay/loop) into a flat
 * list of atomic entries, each with an ABSOLUTE start delay (ms) — composition is realized purely through
 * per-entry delay_ms (no engine grouping needed for standalone values). Returns { entries, duration } where
 * `duration` is this node's own run length in ms, used to offset later siblings in a sequence/stagger;
 * null = unknown (spring/decay/loop), which is illegal to sequence anything after.
 *
 * @param node  The composition.
 * @param environment  The expression environment.
 * @param baseDelay  When the composition starts, in ms.
 * @param loop  The composition repeats forever (inside an Animated.loop).
 *
 * @returns Its atomic entries and its run length.
 */
function flattenAnim(
  node: t.Node | null | undefined,
  environment: Env,
  baseDelay: number,
  loop: boolean,
): FlatAnimation {
  // Resolve a named animation handle, then require one of the supported Animated.* calls.
  node = resolveAnim(node, environment);
  if (!isAnimatedCall(node)) {
    throw aotError(
      'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
    );
  }

  // Atomic animations drive one useAnimatedValue directly; timing has a known length, spring/decay do not.
  const kind = node.callee.property.name;
  const callArguments = node.arguments;
  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    const valueRef = callArguments[0];
    if (
      valueRef?.type !== 'Identifier' ||
      !environment.animations?.has(valueRef.name)
    ) {
      throw aotError(
        `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
      );
    }
    const slotName = environment.animations.get(valueRef.name)!.slotName;
    const get = animConfigGetter(callArguments[1]);
    const ownDelay = ownDelayMs(kind, get, environment);
    const duration =
      kind === 'timing' ? ownDelay + timingDuration(get, environment) : null;
    return {
      entries: [{slotName, kind, get, delayMs: baseDelay + ownDelay, loop}],
      duration,
    };
  }

  // Animated.delay contributes time to a composition but starts no engine animation itself.
  if (kind === 'delay') {
    return {
      entries: [],
      duration: configMs(
        callArguments[0],
        environment,
        0,
        "Animated.delay's time",
      ),
    };
  }

  // Composition nodes flatten into atomic animations with absolute delay_ms offsets.
  if (kind === 'sequence' || kind === 'parallel' || kind === 'stagger') {
    const list = kind === 'stagger' ? callArguments[1] : callArguments[0];
    const staggerMs =
      kind === 'stagger'
        ? configMs(
            callArguments[0],
            environment,
            0,
            "Animated.stagger's interval",
          )
        : 0;
    if (list?.type !== 'ArrayExpression') {
      throw aotError(`AOT: Animated.${kind}(...) needs an array of animations`);
    }

    // Sequence advances its running offset; parallel/stagger start children from the same base with offsets.
    const entries: AnimEntry[] = [];
    let offset = baseDelay; // running offset (sequence)
    let groupDuration = 0; // max end-time relative to baseDelay (parallel/stagger)
    let index = 0;
    for (const child of list.elements) {
      if (!child) continue;
      const start =
        kind === 'sequence' ? offset : baseDelay + index * staggerMs;
      const flatAnimation: FlatAnimation = flattenAnim(
        child,
        environment,
        start,
        loop,
      );
      entries.push(...flatAnimation.entries);
      if (kind === 'sequence') {
        if (flatAnimation.duration == null) {
          throw aotError(
            'AOT: an Animated.sequence entry needs a known duration — use Animated.timing / Animated.delay (a spring/decay/loop inside a sequence is not supported; it has no fixed length to offset the next entry by)',
          );
        }
        offset += flatAnimation.duration;
      } else {
        const end = start - baseDelay + (flatAnimation.duration ?? 0);
        if (end > groupDuration) {
          groupDuration = end;
        }
      }

      index++;
    }

    // The same value can't appear twice in a flat (delay_ms) composition: er_animation_value_animate cancels the
    // running anim on a value, so concurrent/flat-sequenced same-value steps cancel each other. (A
    // top-level Animated.sequence is handled separately via on_complete chaining, which does support this.)
    const seen = new Set<string>();
    for (const entry of entries) {
      if (seen.has(entry.slotName)) {
        throw aotError(
          'AOT: the same animated value is driven more than once in this composition — concurrent/flat same-value steps cancel each other. Use a top-level Animated.sequence(...) for multi-step animation of one value.',
        );
      }
      seen.add(entry.slotName);
    }
    return {
      entries,
      duration: kind === 'sequence' ? offset - baseDelay : groupDuration,
    };
  }

  // A nested loop can flatten only when it wraps a single atomic animation; complex loops need start-time chaining.
  if (kind === 'loop') {
    const flatAnimation: FlatAnimation = flattenAnim(
      callArguments[0],
      environment,
      baseDelay,
      true,
    );
    if (flatAnimation.entries.length !== 1) {
      throw aotError(
        'AOT: an Animated.loop inside another composition can only wrap a single Animated.timing/spring/decay',
        'a loop around a sequence works when the loop is the animation you start: Animated.loop(Animated.sequence([...])).start().',
      );
    }
    return {entries: flatAnimation.entries, duration: null};
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
  ANIMATION_KINDS.has((node.callee.property as t.Identifier)?.name);

/**
 * An animation handle (`anim` after `const anim = Animated.loop(…)`) resolved to the call it names.
 *
 * @param node  An animation, or a handler local bound to one.
 * @param environment  The expression environment.
 *
 * @returns The animation the node stands for.
 */
export const resolveAnim = (
  node: t.Node | null | undefined,
  environment: Env,
): t.Node | null | undefined =>
  node?.type === 'Identifier' && environment.animationLocals?.has(node.name)
    ? environment.animationLocals!.get(node.name)
    : node;

/**
 * One Animated.timing/spring/decay call as a chain step, started `extraDelay` ms later than its own delay.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 * @param extraDelay  Milliseconds added to its own delay.
 *
 * @returns The step.
 */
function animStep(
  node: AnimatedCall,
  environment: Env,
  extraDelay: number,
): AnimEntry {
  const kind = node.callee.property.name;

  // A chain step must drive one collected useAnimatedValue.
  const valueRef = node.arguments[0];
  if (
    valueRef?.type !== 'Identifier' ||
    !environment.animations?.has(valueRef.name)
  ) {
    throw aotError(
      `AOT: Animated.${kind}() first argument must be a useAnimatedValue`,
    );
  }

  // Fold this step's own delay and add any delay inherited from an enclosing sequence/delay.
  const get = animConfigGetter(node.arguments[1]);
  const ownDelay = ownDelayMs(kind, get, environment);
  return {
    slotName: environment.animations.get(valueRef.name)!.slotName,
    kind,
    get,
    delayMs: extraDelay + ownDelay,
    loop: false,
  };
}

/**
 * The handle of every value an animation drives.
 *
 * @param node  The animation.
 * @param environment  The expression environment.
 * @param accumulator  Collects the handles.
 *
 * @returns `accumulator`.
 */
export function animValues(
  node: t.Node | null | undefined,
  environment: Env,
  accumulator: Set<string> = new Set(),
): Set<string> {
  node = resolveAnim(node, environment);

  // Reject props that the FlatList-to-ScrollView rewrite cannot preserve.
  if (!isAnimatedCall(node)) {
    throw aotError(
      'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
    );
  }

  // Recurse into composed animations, skipping delay nodes because they drive no animated value.
  const kind = node.callee.property.name;
  const callArguments = node.arguments;
  if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    accumulator.add(animStep(node, environment, 0).slotName);
  } else if (kind === 'loop') {
    animValues(callArguments[0], environment, accumulator);
  } else if (kind !== 'delay') {
    const list = kind === 'stagger' ? callArguments[1] : callArguments[0];
    for (const child of (list as t.ArrayExpression | undefined)?.elements ??
      []) {
      if (child) {
        animValues(child, environment, accumulator);
      }
    }
  }

  return accumulator;
}

/**
 * The steps of an `Animated.sequence([...])` for an on_complete chain. A delay() folds into the next step's
 * delay_ms; `trailingDelay` is what follows the last step. A nested parallel/stagger/loop throws.
 *
 * @param sequenceNode  The sequence call.
 * @param environment  The expression environment.
 *
 * @returns The steps, and the delay after the last one.
 */
function sequenceSteps(
  sequenceNode: AnimatedCall,
  environment: Env,
): {steps: AnimEntry[]; trailingDelay: number} {
  const list = sequenceNode.arguments[0];

  // Sequence composition is lowered at build time, so its children must be an array literal.
  if (list?.type !== 'ArrayExpression') {
    throw aotError('AOT: Animated.sequence(...) needs an array of animations');
  }

  // Fold delay entries into the next real animation step so the sequence can be emitted as an on-complete chain.
  const steps: AnimEntry[] = [];
  let pendingDelay = 0;
  for (const element of list.elements) {
    if (!element) continue;
    const child = resolveAnim(element, environment);

    // Reject non-animation entries in the sequence.
    if (!isAnimatedCall(child)) {
      throw aotError(
        'AOT: Animated.sequence entries must be Animated.timing/spring/decay/delay(...)',
      );
    }

    // Fold delay-only entries into the following animation step.
    const kind = child.callee.property.name;
    if (kind === 'delay') {
      pendingDelay += configMs(
        child.arguments[0],
        environment,
        0,
        "Animated.delay's time",
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

    steps.push(animStep(child, environment, pendingDelay));
    pendingDelay = 0;
  }

  return {steps, trailingDelay: pendingDelay};
}

/**
 * Lowers steps to an on_complete CHAIN: step 0 starts inline, and each step's completion callback starts the
 * next. Unlike the flat delay_ms path, this is correct when steps share a value — er_animation_value_animate
 * cancels the running anim on a value, so starting them together would cancel all but the last — and needs
 * no fixed duration. A step that is interrupted (finished == false) ends the chain, as it does in JS. The
 * chain's callbacks and loop slots are added to the output here.
 *
 * `loop` makes the last step start the first again: {iterations (negative = forever), resetTo (the value to
 * put back where it started before each repeat, or null), trailingDelay (ms before each repeat)}.
 *
 * @param steps  The chain's steps.
 * @param environment  The expression environment.
 * @param context  The statement compiler's context.
 * @param completion  The completion callback to run when the chain ends, if any.
 * @param loop  How the chain repeats, or null for once.
 * @param trailing  For a chain that runs once: the composition's key and the wait after its last step.
 *
 * @returns The chain.
 */
function lowerChain(
  steps: AnimEntry[],
  environment: Env,
  context: StatementContext,
  completion: IrCompletion | null,
  loop: LoopSpec | null = null,
  trailing: {key: string; delayMs: number} | null = null,
): IrStartChain {
  // GLOBAL: callback names must be unique across handlers
  const id = context.output.sequenceCount++;

  // Lowered from the tail, the order the callbacks are emitted in.
  const lowered: IrAnimation[] = new Array(steps.length);
  for (let index = steps.length - 1; index >= 0; index--) {
    lowered[index] = lowerAnimation(steps[index], environment);
  }

  // A repeat restarts step 0 after any trailing sequence delay.
  const chain: IrStartChain = {
    kind: 'startChain',
    id,
    steps: lowered,
    loop: loop
      ? {
          iterations: loop.iterations,
          resetHandle: loop.resetTo,
          restart: lowerAnimation(
            {...steps[0], delayMs: steps[0].delayMs + loop.trailingDelay},
            environment,
          ),
          finalWait:
            loop.iterations >= 0 && completion && loop.trailingDelay > 0
              ? {
                  delayMs: totalMs(loop.trailingDelay),
                  owner: waitOwner(context, loop.key, true),
                }
              : null,
        }
      : null,
    completion,
    completionWait:
      !loop && completion && trailing && trailing.delayMs > 0
        ? completionWait(
            context,
            trailing.key,
            trailing.delayMs,
            `chain_${id}_${steps.length}`,
          )
        : null,
  };

  context.output.animationCallbacks.push({kind: 'chain', chain});
  context.output.effectDeclarations.push({kind: 'chain', chain});

  return chain;
}

/**
 * A start with nothing to run.
 *
 * @param context  The statement compiler's context.
 * @param completion  The completion callback, if any; it never runs.
 *
 * @returns The start.
 */
const emptyStart = (
  context: StatementContext,
  completion: IrCompletion | null,
): IrStartAnimations => ({
  kind: 'startAnimations',
  animations: [],
  completion,
  completionWait: null,
  completionJoin: null,
});

/** Numbers each composition node once, so a key names the place it is written. */
const compositionIds = new WeakMap<t.Node, number>();
let nextCompositionId = 0;

/**
 * What identifies a composition as the owner of its completion waits: where it is written, within one component
 * instance. Each run still waits on its own; the owner is what a stop names, so it ends the runs of the
 * composition it stops and no other. A start and stop through the same local (`const pulse = …; pulse.start();
 * … pulse.stop()`) reach the same node.
 *
 * @param node  The composition, resolved.
 * @param environment  The expression environment.
 *
 * @returns The key.
 */
function compositionKey(
  node: t.Node | null | undefined,
  environment: Env,
): string {
  let id = node ? compositionIds.get(node) : -1;
  if (id === undefined) {
    id = nextCompositionId++;
    compositionIds.set(node!, id);
  }

  return `${environment.callbackPrefix ?? ''}|${id}`;
}

/**
 * The owner number completion waits of a composition carry, so a stop can end them; numbered on first use.
 *
 * @param context  The statement compiler's context.
 * @param key  The composition's key.
 * @param isStart  A start is asking, so the waits are real (a stop alone leaves nothing to end).
 *
 * @returns The owner.
 */
function waitOwner(
  context: StatementContext,
  key: string,
  isStart: boolean,
): number {
  let record = context.output.animationWaits.get(key);
  if (!record) {
    record = {owner: context.output.animationWaits.size, isStarted: false};
    context.output.animationWaits.set(key, record);
  }
  if (isStart) {
    record.isStarted = true;
  }

  return record.owner;
}

/**
 * True when a composition has an Animated.delay anywhere, the only thing a completion can wait on.
 *
 * @param node  The composition.
 * @param environment  The expression environment.
 *
 * @returns Whether it does.
 */
function hasDelay(node: t.Node | null | undefined, environment: Env): boolean {
  node = resolveAnim(node, environment);
  if (!isAnimatedCall(node)) return false;

  // Look through compositions; a timing, spring or decay waits on nothing beyond itself.
  const kind = node.callee.property.name;
  const callArguments = node.arguments;
  if (kind === 'delay') return true;
  if (kind === 'loop') return hasDelay(callArguments[0], environment);
  if (kind === 'sequence' || kind === 'parallel' || kind === 'stagger') {
    const list = kind === 'stagger' ? callArguments[1] : callArguments[0];
    return (
      list?.type === 'ArrayExpression' &&
      list.elements.some(child => child && hasDelay(child, environment))
    );
  }

  return false;
}

/**
 * The composition whose completion waits `anim.stop()` ends, when it has a delay to wait on.
 *
 * @param node  The composition being stopped.
 * @param environment  The expression environment.
 * @param context  The statement compiler's context.
 *
 * @returns The owner, or null.
 */
export function stopWaitOwner(
  node: t.Node | null | undefined,
  environment: Env,
  context: StatementContext,
): number | null {
  const resolved = resolveAnim(node, environment);
  return hasDelay(resolved, environment)
    ? waitOwner(context, compositionKey(resolved, environment), false)
    : null;
}

/**
 * The wait between a composition's last animation finishing and its completion running.
 *
 * @param context  The statement compiler's context.
 * @param key  The composition's key.
 * @param delayMs  The wait.
 * @param callback  The name of the on_complete callback that starts it.
 *
 * @returns The wait.
 */
const completionWait = (
  context: StatementContext,
  key: string,
  delayMs: number,
  callback: string,
): IrCompletionWait => ({
  delayMs: totalMs(delayMs),
  callback,
  owner: waitOwner(context, key, true),
});

/**
 * A start that drives no value. As in JS, its completion still runs as finished once the composition's delays
 * have passed, counted down in a completion wait of its own.
 *
 * @param context  The statement compiler's context.
 * @param completion  The completion callback, if any.
 * @param delayMs  How long the composition takes.
 * @param key The composition's key.
 *
 * @returns The start.
 */
function startNothing(
  context: StatementContext,
  completion: IrCompletion | null,
  delayMs: number,
  key: string,
): IrStartAnimations | IrCompleteAfter {
  if (!completion) {
    return emptyStart(context, completion);
  }

  const start: IrCompleteAfter = {
    kind: 'completeAfter',
    completion,
    delayMs: totalMs(delayMs),
    owner: null,
  };
  if (delayMs > 0) {
    start.owner = waitOwner(context, key, true);
  }

  return start;
}

/**
 * Lowers `Animated.loop(animation, config?).start()`, or returns null for the flat path. An endless loop of
 * one timing is the engine's own cfg.loop; anything else (a sequence, a spring or decay, a counted loop) is a
 * chain whose last step starts the first again. As in JS, a looped single animation goes back to where it
 * started before each repeat (resetBeforeIteration), and a looped sequence carries on from where it ended.
 *
 * @param loopNode  The `Animated.loop(…)` call.
 * @param environment  The expression environment.
 * @param context  The statement compiler's context.
 * @param completion  The completion callback to run when the loop ends, if any.
 * @param key  The loop's composition key.
 *
 * @returns The start, or null when the flat path (the engine's own cfg.loop) handles it.
 */
function lowerLoopStart(
  loopNode: AnimatedCall,
  environment: Env,
  context: StatementContext,
  completion: IrCompletion | null,
  key: string,
): IrAnimationStart | null {
  // Fold the loop count at build time; absent or noninteger iterations mean repeat forever.
  const inner = resolveAnim(loopNode.arguments[0], environment);
  const kind = isAnimatedCall(inner) ? inner.callee.property.name : null;
  const get = animConfigGetter(loopNode.arguments[1]);
  let iterations = -1;
  if (get('iterations')) {
    let value: unknown;
    try {
      value = evalStatic(
        get('iterations')!,
        foldScope(environment, environment.constants ?? {}),
      );
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
  const reset =
    evalStaticOr(get('resetBeforeIteration'), environment, true) !== false;
  let steps: AnimEntry[];
  let trailingDelay = 0;
  let resetTo: string | null = null;
  if (kind === 'sequence') {
    ({steps, trailingDelay} = sequenceSteps(
      inner as AnimatedCall,
      environment,
    ));
  } else if (kind === 'timing' || kind === 'spring' || kind === 'decay') {
    if (kind === 'timing' && iterations < 0 && reset) return null;
    steps = [animStep(inner as AnimatedCall, environment, 0)];
    if (reset) {
      resetTo = steps[0].slotName;
    }
  } else {
    throw aotError(
      'AOT: Animated.loop can repeat a timing, spring, decay or sequence (looping a parallel/stagger is not yet supported)',
    );
  }

  // A zero-iteration loop starts nothing but still reports successful completion.
  if (iterations === 0) {
    return startNothing(context, completion, 0, key);
  }

  // A loop of delays alone completes after its passes; an endless one never does.
  if (!steps.length) {
    return iterations < 0
      ? emptyStart(context, completion)
      : startNothing(context, completion, iterations * trailingDelay, key);
  }

  // A timing with no duration and no delay completes inside er_animation_value_animate, so a loop made only of
  // those would restart itself forever without returning.
  const instant = (animationEntry: AnimEntry) =>
    animationEntry.kind === 'timing' &&
    animationEntry.delayMs === 0 &&
    timingDuration(animationEntry.get, environment) === 0;
  if (trailingDelay === 0 && steps.every(instant)) {
    throw aotError(
      'AOT: every step of this Animated.loop finishes instantly, so it would never stop repeating',
      'give a step a duration or a delay.',
    );
  }

  return lowerChain(steps, environment, context, completion, {
    iterations,
    resetTo,
    trailingDelay,
    key,
  });
}

/**
 * Lowers a `.start(onComplete)` completion callback to a function (ERAnimationCompleteFn) set as the
 * animation's on_complete, and adds it to the output; its body runs setters/refs/etc. and re-applies state via
 * the update if needed.
 *
 * @param functionNode  The callback.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The completion.
 */
function lowerCompletion(
  functionNode: FunctionNode,
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrCompletion {
  // Generate a completion callback and expose start(({finished}) => ...) as a local int.
  const id = context.output.sequenceCount++;
  const name = `done_${id}`;
  const locals = new Map<string, Local>(environment.locals);
  const parameter = functionNode.params[0];
  if (parameter?.type === 'ObjectPattern') {
    for (const property of parameter.properties as t.ObjectProperty[]) {
      if (
        ((property.key as t.Identifier)?.name ??
          (property.key as t.StringLiteral)?.value) === 'finished'
      ) {
        locals.set((property.value as t.Identifier)?.name ?? 'finished', {
          name: 'finished',
          slotKind: 'int',
        });
      }
    }
  }

  // Lower the callback body as statements; it re-renders if it changed React state.
  const functionBody = functionNode.body;
  const list: t.Statement[] =
    functionBody.type === 'BlockStatement'
      ? functionBody.body
      : [{type: 'ExpressionStatement', expression: functionBody}];
  const completionContext: StatementContext = {
    stateChanged: false,
    output: context.output,
  };
  const body = lowerBody(
    list,
    {...environment, locals},
    state,
    completionContext,
  );
  const completion = {name, body, updatesState: completionContext.stateChanged};
  context.output.animationCallbacks.push({kind: 'completion', completion});

  return completion;
}

/**
 * Lowers `<animation>.start()` — a single Animated.timing/spring/decay or a composition
 * (sequence/parallel/stagger/delay/loop). A sequence, and a loop the engine cannot repeat on its own, become
 * an on_complete chain (lowerChain); everything else flattens to one animation per atomic entry, composition
 * expressed through per-entry delay_ms. Native-driven; sets no React state.
 *
 * @param expression  The `.start(…)` call.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param context  The statement compiler's context.
 *
 * @returns The start.
 */
export function lowerAnimateStart(
  expression: t.CallExpression,
  environment: Env,
  state: StateTable,
  context: StatementContext,
): IrAnimationStart {
  // Resolve the animation being started and handle cases that need callback chaining before falling back to flat emission.
  const completion = isFn(expression.arguments[0])
    ? lowerCompletion(expression.arguments[0], environment, state, context)
    : null;
  const receiver = resolveAnim(
    (expression.callee as t.MemberExpression).object,
    environment,
  );
  const kind = isAnimatedCall(receiver) ? receiver.callee.property.name : null;
  const key = compositionKey(receiver, environment);
  if (kind === 'sequence') {
    const {steps, trailingDelay} = sequenceSteps(
      receiver as AnimatedCall,
      environment,
    );
    return steps.length
      ? lowerChain(steps, environment, context, completion, null, {
          key,
          delayMs: trailingDelay,
        })
      : startNothing(context, completion, trailingDelay, key);
  }
  if (kind === 'loop') {
    const start = lowerLoopStart(
      receiver as AnimatedCall,
      environment,
      context,
      completion,
      key,
    );
    if (start) return start;
  }

  // The flat path has no "after all entries finish" join point, so completion callbacks are limited to one entry.
  const {entries, duration} = flattenAnim(receiver, environment, 0, false);
  if (!entries.length) {
    return startNothing(context, completion, duration ?? 0, key);
  }
  if (completion && entries.length > 1) {
    throw aotError(
      'AOT: a .start(onComplete) callback on a parallel/stagger animation is not yet supported',
      'attach the completion callback to a single animation or an Animated.sequence(...). For "after all parallel anims", restructure as a sequence.',
    );
  }

  // Each entry starts on its own, numbered after the configs already in this body.
  const start = emptyStart(context, completion);
  for (const animationEntry of entries) {
    start.animations.push(lowerAnimation(animationEntry, environment));
  }

  // A spring or decay has no fixed end, so a delay beside it that runs past its start is joined at run time.
  const [only] = entries;
  if (
    completion &&
    only.kind !== 'timing' &&
    !only.loop &&
    duration != null &&
    duration > only.delayMs
  ) {
    start.completionJoin = {
      delayMs: totalMs(duration),
      owner: waitOwner(context, key, true),
    };
  }

  // A delay beside the one timing that outlasts it holds the completion back until the composition ends.
  if (completion && only.kind === 'timing' && !only.loop && duration != null) {
    const end = only.delayMs + timingDuration(only.get, environment);
    if (duration > end) {
      start.completionWait = completionWait(
        context,
        key,
        duration - end,
        `wait_${context.output.sequenceCount++}`,
      );
      context.output.animationCallbacks.push({
        kind: 'completionWait',
        completion,
        wait: start.completionWait,
      });
    }
  }

  return start;
}
