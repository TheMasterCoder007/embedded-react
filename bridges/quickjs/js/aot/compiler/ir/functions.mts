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

import type {CType} from '../types.mts';
import type {
  IrCompletion,
  IrCompletionWait,
  IrStartChain,
} from './animations.mts';
import type {IrExpr} from './expressions.mts';
import type {IrPanEvent, IrPanResponder} from './pan-responder.mts';
import type {IrCallback, IrStmt} from './statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A <TouchableOpacity> fade: one timing animation of the node's opacity value. */
export interface IrPressFade {
  /** The animated value's handle. */
  value: string;
  opacity: number;
  durationMs: number;
}

/** An event handler: a function with the engine's event callback signature. */
export type IrHandler =
  | {kind: 'callback'; name: string; callback: IrCallback}
  /** A press fade, then the app's own handler for the event, if it has one. */
  | {kind: 'pressFade'; name: string; fade: IrPressFade; then: string | null}
  | {kind: 'panEvent'; pan: IrPanResponder; event: IrPanEvent};

/** A setInterval / setTimeout callback. Its slot is reserved before the body lowers, which may add timers. */
export interface IrTimerFunction {
  name: string;
  callback: IrCallback | null;
}

/** An animation's on_complete callbacks, in the order the app declares them. */
export type IrAnimationCallback =
  | {kind: 'completion'; completion: IrCompletion}
  /** Starts the completion's wait once the last animation has finished. */
  | {kind: 'completionWait'; completion: IrCompletion; wait: IrCompletionWait}
  /** A chain's step callbacks, and the repeat of a loop. */
  | {kind: 'chain'; chain: IrStartChain};

/**
 * A dep-driven useEffect, `er_effect_<id>()`: it runs at mount, then again from app_update() whenever a dep
 * changed against the copy kept from the last run.
 */
export interface IrDepEffect {
  id: number;
  deps: IrExpr[];
  body: IrStmt[];
  /** It ends in a `return () => …`, so each run first runs the last run's cleanup, if that run reached it. */
  hasCleanup: boolean;
  /** The cleanup, `er_effect_<id>_cleanup()`, once lowered. */
  cleanup: IrStmt[] | null;
}

/** A function an effect runs in, in the order the effects lower. */
export type IrEffectFunction =
  | {kind: 'effect'; effect: IrDepEffect}
  | {kind: 'cleanup'; effect: IrDepEffect}
  /** A mount-once effect with an early `return`, which needs a function of its own. */
  | {kind: 'mount'; name: string; body: IrStmt[]};

/** What er_app_build() runs, once, after the first app_update(). */
export type IrMountEffect =
  | {kind: 'inline'; callback: IrCallback}
  | {kind: 'call'; name: string; updatesState: boolean}
  /** Runs a dep-driven effect and keeps a copy of each dep. */
  | {kind: 'depEffect'; effect: IrDepEffect};

/** A file-scope variable that effects and animation chains keep between calls. */
export type IrStaticDecl =
  /** An effect local its cleanup reads after the run. */
  | {kind: 'local'; name: string; cType: CType}
  /** Whether an effect's last run reached its cleanup. */
  | {kind: 'armed'; effect: IrDepEffect}
  /** The copy of each dep an effect keeps from its last run. */
  | {kind: 'deps'; effect: IrDepEffect}
  /** A looping chain's pass counter and start value. */
  | {kind: 'chain'; chain: IrStartChain};
