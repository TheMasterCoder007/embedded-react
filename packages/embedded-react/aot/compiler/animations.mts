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
import {isFn, wideInitial} from './collect.mts';
import {lowerBody} from './handlers.mts';
import {isPanCreate} from './pan-responder.mts';
import {parseColorValue} from '../style-map.mts';
import type * as t from '@babel/types';
import type {
  IrAnimation,
  IrAnimationSequence,
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

/** One step of a sequence that runs inside a parallel or stagger. */
type AnimSequenceStep =
  | {kind: 'animation'; entry: AnimEntry}
  | {kind: 'delay'; delayMs: number}
  /** A parallel or stagger, flattened from the moment the step starts. */
  | {kind: 'group'; stopsTogether: boolean; flat: FlatAnimation};

/** A sequence inside a parallel or stagger, run step by step so a step that is interrupted ends it. */
interface AnimSequence {
  steps: AnimSequenceStep[];
}

/**
 * A flattened composition: its atomic entries, the sequences that run beside them, and its own run length (null
 * when not fixed; a sequence that runs step by step adds nothing to it).
 */
interface FlatAnimation {
  entries: AnimEntry[];
  sequences: AnimSequence[];
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

/** Most descriptors one started tree may have (the runtime's ERB_MAX_ANIM_TREE): a group, its animations, its wait. */
const MAX_GROUP_ENTRIES = 32;

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
 *
 * @returns The refs, by name.
 */
export function collectRefs(
  functionBody: t.BlockStatement | t.Expression,
  scope: Scope,
  prefix = '',
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

        // A ref that starts past the int range holds 64 bits; one a timestamp is written to is widened later.
        const initialWidth = wideInitial(value);
        const widened =
          Number.isInteger(value) && initialWidth === 'number'
            ? 'number'
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
 * The latest a composition's delays end, when that may be after its animations do: what a group waits on.
 *
 * @param flat  The flattened composition.
 * @param environment  The expression environment.
 *
 * @returns The wait from the composition's start, or null when an animation is known to end last.
 */
function outlastingDelay(flat: FlatAnimation, environment: Env): number | null {
  const lastKnownEnd = Math.max(
    0,
    ...flat.entries.map(
      animationEntry =>
        animationEntry.delayMs +
        (animationEntry.kind === 'timing'
          ? timingDuration(animationEntry.get, environment)
          : 0),
    ),
  );
  return flat.duration != null && flat.duration > lastKnownEnd
    ? flat.duration
    : null;
}

/**
 * The handle of every value a sequence drives.
 *
 * @param sequence  The sequence.
 * @param accumulator  Collects the handles.
 *
 * @returns `accumulator`.
 */
function sequenceValues(
  sequence: AnimSequence,
  accumulator: Set<string> = new Set(),
): Set<string> {
  for (const step of sequence.steps) {
    if (step.kind === 'animation') {
      accumulator.add(step.entry.slotName);
    } else if (step.kind === 'group') {
      for (const animationEntry of step.flat.entries) {
        accumulator.add(animationEntry.slotName);
      }
      for (const nested of step.flat.sequences) {
        sequenceValues(nested, accumulator);
      }
    }
  }

  return accumulator;
}

/**
 * Running-animation entries a sequence takes: its own, and one for each step, delay and animation in it.
 *
 * @param sequence  The sequence.
 * @param environment  The expression environment.
 *
 * @returns The entries.
 */
function sequenceEntries(sequence: AnimSequence, environment: Env): number {
  let count = 1;
  for (const step of sequence.steps) {
    if (step.kind !== 'group') {
      count++;
      continue;
    }
    count +=
      1 +
      (outlastingDelay(step.flat, environment) == null ? 0 : 1) +
      step.flat.entries.length;
    for (const nested of step.flat.sequences) {
      count += sequenceEntries(nested, environment);
    }
  }

  return count;
}

/**
 * Lowers a sequence that runs inside a parallel or stagger.
 *
 * @param sequence  The sequence.
 * @param environment  The expression environment.
 *
 * @returns The sequence.
 */
function lowerSequence(
  sequence: AnimSequence,
  environment: Env,
): IrAnimationSequence {
  return {
    steps: sequence.steps.map(step => {
      if (step.kind === 'animation') {
        return {
          kind: 'animation',
          animation: lowerAnimation(step.entry, environment),
        };
      }
      if (step.kind === 'delay') {
        return {kind: 'delay', delayMs: totalMs(step.delayMs)};
      }
      const waitMs = outlastingDelay(step.flat, environment);
      return {
        kind: 'group',
        stopsTogether: step.stopsTogether,
        animations: step.flat.entries.map(animationEntry =>
          lowerAnimation(animationEntry, environment),
        ),
        sequences: step.flat.sequences.map(nested =>
          lowerSequence(nested, environment),
        ),
        waitMs: waitMs == null ? null : totalMs(waitMs),
      };
    }),
  };
}

/**
 * Flattens a sequence inside another composition. One with a single animation is that animation, delayed. Any
 * other runs step by step, each step started when the one before it finishes, so a step that is interrupted ends
 * the sequence, the same value can take several steps, and a step needs no fixed length. A sequence inside it is
 * part of it.
 *
 * @param sequenceNode  The sequence call.
 * @param environment  The expression environment.
 * @param baseDelay  When the sequence starts, in ms.
 * @param loop  The sequence repeats forever (inside an Animated.loop).
 *
 * @returns Its entries, or the sequence, and its run length.
 */
function flattenSequence(
  sequenceNode: AnimatedCall,
  environment: Env,
  baseDelay: number,
  loop: boolean,
): FlatAnimation {
  const steps: AnimSequenceStep[] = [];
  // The delays not yet given to a step: a step's own delay takes them, and a group has a delay step before it.
  let pendingDelay = baseDelay;
  const collect = (node: AnimatedCall): void => {
    const list = node.arguments[0];
    if (list?.type !== 'ArrayExpression') {
      throw aotError(
        'AOT: Animated.sequence(...) needs an array of animations',
      );
    }
    for (const element of list.elements) {
      if (!element) continue;
      const child = resolveAnim(element, environment);
      if (!isAnimatedCall(child)) {
        throw aotError(
          'AOT: an animation must be Animated.timing/spring/decay/sequence/parallel/stagger/delay/loop(...)',
        );
      }

      const kind = child.callee.property.name;
      if (kind === 'sequence') {
        collect(child);
      } else if (kind === 'loop') {
        throw aotError(
          'AOT: an Animated.loop inside an Animated.sequence is not supported',
          'a loop around a sequence works when the loop is the animation you start: Animated.loop(Animated.sequence([...])).start().',
        );
      } else if (kind === 'parallel' || kind === 'stagger') {
        const flat = flattenAnim(child, environment, 0, loop);
        if (!flat.entries.length && !flat.sequences.length) {
          pendingDelay += flat.duration ?? 0;
          continue;
        }
        if (pendingDelay > 0) {
          steps.push({kind: 'delay', delayMs: pendingDelay});
          pendingDelay = 0;
        }
        steps.push({
          kind: 'group',
          stopsTogether: stopsTogether(child, environment),
          flat,
        });
      } else {
        const flat = flattenAnim(child, environment, pendingDelay, loop);
        if (flat.entries.length) {
          steps.push({kind: 'animation', entry: flat.entries[0]});
          pendingDelay = 0;
        } else {
          pendingDelay += flat.duration ?? 0;
        }
      }
    }
  };
  collect(sequenceNode);

  // Delays alone start nothing.
  if (!steps.length) {
    return {entries: [], sequences: [], duration: pendingDelay - baseDelay};
  }

  // One animation needs no sequence, unless a delay follows an animation with no fixed length.
  const [only] = steps;
  if (
    steps.length === 1 &&
    only.kind === 'animation' &&
    (only.entry.kind === 'timing' || pendingDelay === 0)
  ) {
    const {entry} = only;
    return {
      entries: [entry],
      sequences: [],
      duration:
        entry.kind === 'timing'
          ? entry.delayMs -
            baseDelay +
            timingDuration(entry.get, environment) +
            pendingDelay
          : null,
    };
  }

  if (pendingDelay > 0) {
    steps.push({kind: 'delay', delayMs: pendingDelay});
  }

  return {entries: [], sequences: [{steps}], duration: null};
}

/**
 * Flattens an Animated composition (timing/spring/decay/sequence/parallel/stagger/delay/loop) into a flat
 * list of atomic entries, each with an ABSOLUTE start delay (ms) — a parallel or stagger is realized purely
 * through per-entry delay_ms. A sequence of several steps inside one is kept whole instead (flattenSequence).
 * Returns { entries, sequences, duration } where `duration` is this node's own run length in ms, used to offset
 * later siblings in a stagger and to know what a completion waits on; null = unknown (spring/decay/loop).
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
      sequences: [],
      duration,
    };
  }

  // Animated.delay contributes time to a composition but starts no engine animation itself.
  if (kind === 'delay') {
    return {
      entries: [],
      sequences: [],
      duration: configMs(
        callArguments[0],
        environment,
        0,
        "Animated.delay's time",
      ),
    };
  }

  if (kind === 'sequence') {
    return flattenSequence(node, environment, baseDelay, loop);
  }

  // A parallel or stagger flattens into atomic animations with absolute delay_ms offsets.
  if (kind === 'parallel' || kind === 'stagger') {
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

    // Every child starts from the same base, a stagger's each an interval after the one before. A value can be
    // driven by one child only: er_animation_value_animate cancels the running anim on a value, so two that run
    // at once would cancel each other.
    const entries: AnimEntry[] = [];
    const sequences: AnimSequence[] = [];
    const driven = new Set<string>();
    let groupDuration = 0; // max end-time relative to baseDelay
    let index = 0;
    for (const child of list.elements) {
      if (!child) continue;
      const start = baseDelay + index * staggerMs;
      const flatAnimation: FlatAnimation = flattenAnim(
        child,
        environment,
        start,
        loop,
      );
      entries.push(...flatAnimation.entries);
      sequences.push(...flatAnimation.sequences);
      const end = start - baseDelay + (flatAnimation.duration ?? 0);
      if (end > groupDuration) {
        groupDuration = end;
      }

      const values = new Set(
        flatAnimation.entries.map(animationEntry => animationEntry.slotName),
      );
      for (const sequence of flatAnimation.sequences) {
        sequenceValues(sequence, values);
      }
      for (const value of values) {
        if (driven.has(value)) {
          throw aotError(
            'AOT: the same animated value is driven more than once in this composition — animations that run at once on one value cancel each other. Use an Animated.sequence(...) for multi-step animation of one value.',
          );
        }
        driven.add(value);
      }

      index++;
    }

    return {entries, sequences, duration: groupDuration};
  }

  // A nested loop can flatten only when it wraps a single atomic animation; complex loops need start-time chaining.
  if (kind === 'loop') {
    const flatAnimation: FlatAnimation = flattenAnim(
      callArguments[0],
      environment,
      baseDelay,
      true,
    );
    if (flatAnimation.entries.length !== 1 || flatAnimation.sequences.length) {
      throw aotError(
        'AOT: an Animated.loop inside another composition can only wrap a single Animated.timing/spring/decay',
        'a loop around a sequence works when the loop is the animation you start: Animated.loop(Animated.sequence([...])).start().',
      );
    }
    return {entries: flatAnimation.entries, sequences: [], duration: null};
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
  sequences: [],
  completion,
  completionWait: null,
  group: null,
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
 * @param hasWaits  The start runs waits of their own, beside any group it runs.
 *
 * @returns The owner.
 */
function waitOwner(
  context: StatementContext,
  key: string,
  isStart: boolean,
  hasWaits: boolean = isStart,
): number {
  let record = context.output.animationWaits.get(key);
  if (!record) {
    record = {
      owner: context.output.animationWaits.size,
      isStarted: false,
      hasWaits: false,
    };
    context.output.animationWaits.set(key, record);
  }
  if (isStart) {
    record.isStarted = true;
  }
  if (hasWaits) {
    record.hasWaits = true;
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
 * Whether one interrupted animation of a composition stops the rest: a parallel's do unless it is given
 * `stopTogether: false`, and a stagger's run on.
 *
 * @param node  The parallel or stagger.
 * @param environment  The expression environment.
 *
 * @returns Whether they stop together.
 */
function stopsTogether(node: AnimatedCall, environment: Env): boolean {
  if (node.callee.property.name !== 'parallel') return false;
  const stopTogether = animConfigGetter(node.arguments[1])('stopTogether');
  return evalStaticOr(stopTogether, environment, true) !== false;
}

/**
 * Lowers `<animation>.start()` — a single Animated.timing/spring/decay or a composition
 * (sequence/parallel/stagger/delay/loop). A sequence, and a loop the engine cannot repeat on its own, become
 * an on_complete chain (lowerChain); everything else flattens to one animation per atomic entry, composition
 * expressed through per-entry delay_ms, run as a group when a completion waits on several or a parallel stops
 * them together. Native-driven; sets no React state.
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

  const flat = flattenAnim(receiver, environment, 0, false);
  const {entries, sequences, duration} = flat;
  if (!entries.length && !sequences.length) {
    return startNothing(context, completion, duration ?? 0, key);
  }

  // Each entry starts on its own, numbered after the configs already in this body.
  const start = emptyStart(context, completion);
  for (const animationEntry of entries) {
    start.animations.push(lowerAnimation(animationEntry, environment));
  }

  // Several animations run as a group when a completion waits on them all, one stopping stops the rest, or a
  // sequence runs among them.
  const runners = entries.length + sequences.length;
  const stopTogether =
    runners > 1 && stopsTogether(receiver as AnimatedCall, environment);
  if (sequences.length || (runners > 1 && (completion || stopTogether))) {
    const waitMs = completion ? outlastingDelay(flat, environment) : null;
    const groupEntries =
      1 +
      (waitMs == null ? 0 : 1) +
      entries.length +
      sequences.reduce(
        (count, sequence) => count + sequenceEntries(sequence, environment),
        0,
      );
    if (groupEntries > MAX_GROUP_ENTRIES) {
      const mostAnimations = MAX_GROUP_ENTRIES - 1 - (waitMs == null ? 0 : 1);
      throw aotError(
        sequences.length
          ? `AOT: this parallel/stagger takes ${groupEntries} entries to run as one group, of at most ${MAX_GROUP_ENTRIES} (one for itself, and one for each animation, sequence and delay in it)`
          : `AOT: this parallel/stagger can run at most ${mostAnimations} animations as one group`,
        'split the composition into smaller ones.',
      );
    }
    context.output.animationGroupEntries += groupEntries;
    start.sequences = sequences.map(sequence =>
      lowerSequence(sequence, environment),
    );
    start.group = {
      stopsTogether: stopTogether,
      wait:
        waitMs == null
          ? null
          : {delayMs: totalMs(waitMs), owner: waitOwner(context, key, true)},
      // A group with sequences is kept, so starting it again stops the run before.
      owner: sequences.length
        ? waitOwner(context, key, true, waitMs != null)
        : null,
    };
    return start;
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
    start.group = {
      stopsTogether: true,
      wait: {
        delayMs: totalMs(duration),
        owner: waitOwner(context, key, true),
      },
      owner: null,
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
