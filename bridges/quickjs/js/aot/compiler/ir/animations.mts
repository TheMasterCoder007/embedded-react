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

import type {IrExpr} from './expressions.mts';
import type {IrStmt} from './statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** `Animated.timing`: a fixed duration along an easing curve. */
export interface IrTimingCurve {
  kind: 'timing';
  durationMs: number;
  /** The engine easing, an ER_EASE_* name. */
  easing: string;
  /** The control points of an ER_EASE_BEZIER easing, or null. */
  bezier: number[] | null;
}

/** `Animated.spring`. */
export interface IrSpringCurve {
  kind: 'spring';
  stiffness: number;
  damping: number;
  mass: number;
}

/** `Animated.decay`: slows from a starting velocity. */
export interface IrDecayCurve {
  kind: 'decay';
  deceleration: number;
  velocity: number;
}

/** How an animated value moves: one ERAnimConfig's type-specific fields. */
export type IrAnimCurve = IrTimingCurve | IrSpringCurve | IrDecayCurve;

/** One animated value driven toward a target. */
export interface IrAnimation {
  /** The animated value's handle. */
  handle: string;
  curve: IrAnimCurve;
  /** The target, or null for a decay with no toValue (it runs on its velocity alone). */
  to: IrExpr | null;
  /** Milliseconds before it starts, composition offsets included. */
  delayMs: number;
  /** Repeats forever on the engine's own loop. */
  isLooping: boolean;
}

/** A `.start(onComplete)` callback, which becomes a function of its own. */
export interface IrCompletion {
  name: string;
  body: IrStmt[];
  /** A statement in the body set state, so it ends by re-applying state. */
  updatesState: boolean;
}

/**
 * The wait a composition still needs after its last animation finishes (a trailing delay, or a longer delay
 * beside it), before the completion runs as finished. An interrupted animation ends it at once, as not finished.
 */
export interface IrCompletionWait {
  delayMs: number;
  /** The on_complete callback that starts the wait. */
  callback: string;
  /** The composition the wait belongs to, which a stop names to end it. */
  owner: number;
}

/**
 * Animations started together, a composition (parallel / stagger / delay) already folded into their delays.
 * The completion runs when the last one ends; there is at most one when there is a completion.
 */
export interface IrStartAnimations {
  kind: 'startAnimations';
  animations: IrAnimation[];
  /** Numbers the first animation's config; the rest follow on. */
  firstConfig: number;
  completion: IrCompletion | null;
  completionWait: IrCompletionWait | null;
}

/** How a chain repeats. */
export interface IrChainLoop {
  /** Repeats; negative loops forever. */
  iterations: number;
  /** The value put back where it started before each repeat, or null. */
  resetHandle: string | null;
  /** The first step as each repeat starts it: its delay includes the wait after the last step. */
  restart: IrAnimation;
  /** A counted loop's last pass ends with that wait too, before the completion runs; null when there is none. */
  finalWait: {delayMs: number; owner: number} | null;
}

/**
 * Steps run one after another, each started when the one before completes. An interrupted step ends the chain.
 * This is how a sequence runs, and a loop the engine cannot repeat by itself.
 */
export interface IrStartChain {
  kind: 'startChain';
  /** Numbers the chain's callbacks, configs, and loop slots. */
  id: number;
  steps: IrAnimation[];
  loop: IrChainLoop | null;
  completion: IrCompletion | null;
  /** Only for a chain that does not loop. */
  completionWait: IrCompletionWait | null;
}

/**
 * A start that drives no value but has a completion: an empty composition, delays alone, or a loop that runs
 * zero times. It completes as finished once its delays have passed, at once when there are none.
 */
export interface IrCompleteAfter {
  kind: 'completeAfter';
  completion: IrCompletion;
  /** Milliseconds to wait; 0 completes at once. */
  delayMs: number;
  /** The composition the wait belongs to, or null when it completes at once. */
  owner: number | null;
}

/** `<animation>.start()`. */
export type IrAnimationStart =
  | IrStartAnimations
  | IrStartChain
  | IrCompleteAfter;
