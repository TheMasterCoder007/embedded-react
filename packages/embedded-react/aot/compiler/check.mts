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

import {GRAD_MAX_STOPS} from '../../src/embedded-react/svg-ops.js';
import {aotError, currentLoc, locateError} from './diagnostics.mts';
import type {AotError, SourcePosition} from './diagnostics.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What lowering records for the check pass: a value, or the size of something the engine holds a fixed number of. */
export type IrCheckSubject =
  /** A number the app stores as a float. */
  | {kind: 'number'; value: unknown}
  /** A time in milliseconds an animation waits or runs for, and what it is, for the error ("Animated.timing's "duration""). */
  | {kind: 'milliseconds'; field: string; value: unknown}
  /** The breakpoints of an `.interpolate()`. */
  | {kind: 'interpolationPoints'; count: number}
  /** The inline segments of a <Text>, and the most the engine draws. */
  | {kind: 'textSpans'; count: number; max: number}
  /** The stops of an <Svg> shape's gradient, by the attribute that holds it. */
  | {kind: 'gradientStops'; attribute: string; count: number}
  /** The stops a <Dial indicatorGradient> has. */
  | {kind: 'dialGradientStops'; count: number}
  /** What the typing pass refused: a 64-bit value mixed with a float, say. */
  | {kind: 'typing'; problem: AotError};

/** A recorded check, and where its error is reported. */
export type IrCheck = IrCheckSubject & {location: SourcePosition | undefined};

/** What lowering leaves for the typing pass: a decision that depends on a type, and where its error is reported. */
interface TypingStep {
  kind: 'typingStep';
  run: () => void;
  location: SourcePosition | undefined;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most breakpoints an interpolation holds (the engine's ER_INTERPOLATE_MAX_POINTS). */
const MAX_INTERPOLATION_POINTS = 8;

/** The longest time an animation can wait or run for: INT_MAX, since the waits the app counts down take an int. */
const MAX_MILLISECONDS = 2 ** 31 - 1;

/** The most stops a <Dial indicatorGradient> holds (the engine's ER_GRADIENT_MAX_STOPS). */
export const DIAL_MAX_GRADIENT_STOPS = 4;

/**
 * Where lowering records its checks and typing steps while a compile runs, in order, or null outside one, when
 * each runs at once.
 */
let recording: (IrCheck | TypingStep)[] | null = null;

/** How many fallback attempts are running: a check inside one runs at once, so its failure picks the fallback. */
let attempts = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * What is wrong with a number the app stores as a float, if anything: every backend refuses NaN, Infinity and
 * a value past the float range.
 *
 * @param rawValue  The value; anything `Number()` converts.
 *
 * @returns The error to report, or null for a value a float holds.
 */
export function numberProblem(rawValue: unknown): AotError | null {
  const value = Number(rawValue);
  if (!Number.isFinite(value)) {
    return aotError(
      `AOT: a numeric constant folded to ` +
        `${rawValue === undefined ? 'undefined' : String(rawValue)}, which a number slot cannot hold`,
      'the value must be a finite number.',
    );
  }

  if (!Number.isFinite(Math.fround(value))) {
    return aotError(
      `AOT: the numeric constant ${value} is past the float range`,
      'a float holds up to about 3.4e38.',
    );
  }

  return null;
}

/**
 * What is wrong with an animation time, if anything: it is rounded to whole milliseconds, and an int holds those
 * from 0 to INT_MAX.
 *
 * @param field  What the time is, for the error.
 * @param rawValue  The value; anything `Number()` converts.
 *
 * @returns The error to report, or null for a time an int holds.
 */
export function millisecondsProblem(
  field: string,
  rawValue: unknown,
): AotError | null {
  const hint = `a time is a number of milliseconds from 0 to ${MAX_MILLISECONDS}.`;
  const value = Math.round(Number(rawValue));
  if (!Number.isFinite(value)) {
    return aotError(
      `AOT: ${field} folded to ` +
        `${rawValue === undefined ? 'undefined' : String(rawValue)}, which is not a time`,
      hint,
    );
  }

  if (value < 0) {
    return aotError(
      `AOT: ${field} is ${value} ms; a time cannot be negative`,
      hint,
    );
  }

  return value > MAX_MILLISECONDS
    ? aotError(`AOT: ${field} is ${value} ms, past the int range`, hint)
    : null;
}

/**
 * What is wrong with a recorded subject, if anything.
 *
 * @param subject  The subject.
 *
 * @returns The error to report, or null when it passes.
 */
export function checkProblem(subject: IrCheckSubject): AotError | null {
  switch (subject.kind) {
    case 'number':
      return numberProblem(subject.value);
    case 'milliseconds':
      return millisecondsProblem(subject.field, subject.value);
    case 'interpolationPoints':
      return subject.count > MAX_INTERPOLATION_POINTS
        ? aotError(
            `AOT: .interpolate() supports up to ${MAX_INTERPOLATION_POINTS} breakpoints ` +
              '(ER_INTERPOLATE_MAX_POINTS)',
          )
        : null;
    case 'textSpans':
      return subject.count > subject.max
        ? aotError(
            `AOT: a <Text> has ${subject.count} inline segments but the engine renders at most ${subject.max}`,
            `combine adjacent plain-text segments, or end the sentence right after a styled ` +
              `<Text> (e.g. "A <b>B</b> C <b>D</b>" is 4). If your engine build raised ` +
              `ER_TEXT_MAX_SPANS, set ER_AOT_MAX_TEXT_SPANS to match when running the AOT.`,
          )
        : null;
    case 'gradientStops':
      return subject.count < 2 || subject.count > GRAD_MAX_STOPS
        ? aotError(
            `AOT: "${subject.attribute}.stops" needs 2..${GRAD_MAX_STOPS} entries (got ${subject.count})`,
          )
        : null;
    case 'dialGradientStops':
      if (subject.count < 2) {
        return aotError(
          'AOT: <Dial indicatorGradient> needs at least two stops (max 4)',
        );
      }

      return subject.count > DIAL_MAX_GRADIENT_STOPS
        ? aotError(
            `AOT: <Dial indicatorGradient> takes at most ${DIAL_MAX_GRADIENT_STOPS} stops (got ${subject.count})`,
            "the engine's arc holds 4 (ER_GRADIENT_MAX_STOPS); merge stops that sit close together.",
          )
        : null;
    case 'typing':
      return subject.problem;
  }
}

/**
 * Checks a subject. During a compile it is recorded for the check pass, with where its error would be
 * reported; otherwise, or inside a fallback attempt, it is checked at once.
 *
 * @param subject  The subject.
 * @param location  Where its error is reported, when that is more exact than the node being lowered.
 */
export function check(
  subject: IrCheckSubject,
  location?: SourcePosition,
): void {
  if (recording && !attempts) {
    recording.push({...subject, location: location ?? currentLoc()});
    return;
  }

  const problem = checkProblem(subject);
  if (problem) {
    locateError(problem, location);
    throw problem;
  }
}

/**
 * Whether lowering is leaving types to the typing pass: during a compile, outside a fallback attempt.
 *
 * @returns True when a typing step is recorded rather than run.
 */
export const defersTyping = (): boolean => recording !== null && !attempts;

/**
 * Runs what depends on a type. During a compile it is recorded for the typing pass, which runs it once the
 * slot widths are known; otherwise, or inside a fallback attempt, it runs at once.
 *
 * @param run  The step.
 */
export function whenTyped(run: () => void): void {
  if (recording && !attempts) {
    recording.push({kind: 'typingStep', run, location: currentLoc()});
    return;
  }

  run();
}

/**
 * Checks a number the app stores as a float (see check).
 *
 * @param value  The value.
 */
export const checkNumber = (value: unknown): void =>
  check({kind: 'number', value});

/**
 * Runs a lowering attempt whose failure picks a fallback, so the checks inside it run at once.
 *
 * @param attempt  The attempt.
 *
 * @returns What the attempt returns.
 */
export function attemptLowering<Result>(attempt: () => Result): Result {
  attempts++;
  try {
    return attempt();
  } finally {
    attempts--;
  }
}

/**
 * The check pass: reports the first recorded check that fails, at its location.
 *
 * @param checks  The checks, in the order lowering recorded them.
 */
export function runChecks(checks: IrCheck[]): void {
  for (const recorded of checks) {
    const problem = checkProblem(recorded);
    if (problem) {
      if (recorded.location) {
        problem.aotLoc = recorded.location;
      }

      throw problem;
    }
  }
}

/**
 * Lowers with its checks and typing steps recorded rather than run, then runs the typing pass: each step, in
 * the order lowering recorded it, until one fails. That one becomes a check, so the check pass reports the first
 * problem in the app. When lowering fails, both passes run over what was recorded before the failure, so the
 * first problem is still the one reported.
 *
 * @param lower  The lowering.
 * @param beforeTyping  Runs once lowering is over, before the typing pass.
 *
 * @returns What lowering returns, and its checks, in order.
 */
export function lowerWithChecks<Result>(
  lower: () => Result,
  beforeTyping: () => void = () => {},
): {
  result: Result;
  checks: IrCheck[];
} {
  const steps: (IrCheck | TypingStep)[] = [];
  const outer = recording;
  recording = steps;
  let lowered: {result: Result} | {error: unknown};
  try {
    lowered = {result: lower()};
  } catch (error) {
    lowered = {error};
  } finally {
    recording = outer;
  }

  beforeTyping();
  const checks: IrCheck[] = [];
  for (const step of steps) {
    if (step.kind !== 'typingStep') {
      checks.push(step);
      continue;
    }

    try {
      step.run();
    } catch (error) {
      locateError(error, step.location);
      checks.push({
        kind: 'typing',
        problem: error as AotError,
        location: (error as AotError).aotLoc,
      });
      break;
    }
  }

  if ('error' in lowered) {
    runChecks(checks);
    throw lowered.error;
  }

  return {result: lowered.result, checks};
}
