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

import {aotError, currentLoc} from './diagnostics.mts';
import type {AotError, SourcePosition} from './diagnostics.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A value lowering kept, to be checked by the check pass, and where its error is reported. */
export interface IrCheck {
  /** A number the app stores as a float. */
  kind: 'number';
  value: unknown;
  loc: SourcePosition | undefined;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Where lowering records its checks while a compile runs, or null outside one, when a check runs at once. */
let recording: IrCheck[] | null = null;

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
        `${rawValue === undefined ? 'undefined' : String(rawValue)}, which has no C form`,
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
 * Checks a number the app stores as a float. During a compile it is recorded for the check pass, with where
 * its error would be reported; otherwise, or inside a fallback attempt, it is checked at once.
 *
 * @param value  The value.
 */
export function checkNumber(value: unknown): void {
  if (recording && !attempts) {
    recording.push({kind: 'number', value, loc: currentLoc()});
    return;
  }

  const problem = numberProblem(value);
  if (problem) {
    throw problem;
  }
}

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
  for (const check of checks) {
    const problem = numberProblem(check.value);
    if (problem) {
      if (check.loc) {
        problem.aotLoc = check.loc;
      }

      throw problem;
    }
  }
}

/**
 * Lowers with its checks recorded rather than run. When lowering fails, the checks recorded before the
 * failure run first, so the first problem in the app is the one reported, as when every check ran at once.
 *
 * @param lower  The lowering.
 *
 * @returns What lowering returns, and its checks, in order.
 */
export function lowerWithChecks<Result>(lower: () => Result): {
  result: Result;
  checks: IrCheck[];
} {
  const checks: IrCheck[] = [];
  const outer = recording;
  recording = checks;
  try {
    return {result: lower(), checks};
  } catch (error) {
    runChecks(checks);
    throw error;
  } finally {
    recording = outer;
  }
}
