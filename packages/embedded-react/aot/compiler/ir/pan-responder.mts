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
import type {IrCallback} from './statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * What a responder event's handler does around the app's callback:
 *  - `grant` anchors the gesture where the claim happened (a second grant is ignored: one gesture, not two);
 *  - `release` and `terminate` run only while the gesture is granted, then clear it;
 *  - `move` runs only while the gesture is granted;
 *  - `reject` runs as is: the node asked for the gesture and was refused, so it never owned it.
 */
export type IrPanRole = 'grant' | 'release' | 'terminate' | 'move' | 'reject';

/** One responder event the engine fires on the node. */
export interface IrPanEvent {
  /** The engine event, an ER_EVENT_RESPONDER_* name. */
  event: string;
  role: IrPanRole;
  /** The app's callback, or null when it has none (grant, release and terminate still keep the gesture state). */
  callback: IrCallback | null;
}

/** A should-set predicate the engine asks synchronously inside hit-testing: one condition, read-only. */
export interface IrPanQuery {
  /** The engine query, an ER_QUERY_* name. */
  query: string;
  test: IrExpr;
}

/**
 * A PanResponder lowered onto the engine's responder system: its gesture state, its event handlers and its
 * should-set queries, generated once however many nodes spread it.
 */
export interface IrPanResponder {
  /** The JS name it is bound to. */
  name: string;
  /** Prefix of its file-scope gesture state (`<statePrefix>_granted`, …). */
  statePrefix: string;
  /** Prefix of its generated C functions. */
  fnPrefix: string;
  /** The gesture's id. */
  stateID: number;
  /** In the order they are emitted. */
  events: IrPanEvent[];
  queries: IrPanQuery[];
}
