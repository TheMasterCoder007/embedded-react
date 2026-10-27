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

import {printExpr} from './expressions.mts';
import {printCallback} from './statements.mts';
import type {CFunction} from './statements.mts';
import type {
  IrPanEvent,
  IrPanQuery,
  IrPanResponder,
} from '../ir/pan-responder.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A should-set predicate as a C function that returns its value. */
export interface ResponderQuery {
  name: string;
  expr: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Query constant → the suffix of the C function emitted for it. */
const QUERY_SUFFIX: Record<string, string> = {
  ER_QUERY_START_SHOULD_SET: 'start_should_set',
  ER_QUERY_START_SHOULD_SET_CAPTURE: 'start_should_set_capture',
  ER_QUERY_MOVE_SHOULD_SET: 'move_should_set',
  ER_QUERY_MOVE_SHOULD_SET_CAPTURE: 'move_should_set_capture',
  ER_QUERY_TERMINATION_REQUEST: 'termination_request',
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The C function that handles one of a responder's events.
 *
 * @param pan  The responder.
 * @param event  The event.
 *
 * @returns The function's name.
 */
export const panEventName = (pan: IrPanResponder, event: IrPanEvent): string =>
  `${pan.fnPrefix}_${event.role}`;

/**
 * The C function that answers one of a responder's should-set queries.
 *
 * @param pan  The responder.
 * @param query  The query.
 *
 * @returns The function's name.
 */
export const panQueryName = (pan: IrPanResponder, query: IrPanQuery): string =>
  `${pan.fnPrefix}_${QUERY_SUFFIX[query.query]}`;

/**
 * A responder's file-scope gesture state: whether the gesture is granted, the anchor, and RN's x0/y0.
 *
 * @param pan  The responder.
 *
 * @returns The C declarations.
 */
export function printPanDecls(pan: IrPanResponder): string[] {
  const prefix = pan.statePrefix;
  return [
    `/* PanResponder "${pan.name}" (gesture ${pan.stateID}) — RN's gestureState. The grant anchors the`,
    `   travel so g.dx opens at 0 however much slop the claim cost; data->dx runs from touch-down. */`,
    `static int ${prefix}_granted = 0;`,
    `static int ${prefix}_base_dx = 0;`,
    `static int ${prefix}_base_dy = 0;`,
    `static int ${prefix}_x0 = 0;`,
    `static int ${prefix}_y0 = 0;`,
  ];
}

/**
 * One responder event's C handler: the gesture bookkeeping its role needs around the app's callback.
 *
 * @param pan  The responder.
 * @param event  The event.
 *
 * @returns The C function.
 */
export function printPanEvent(
  pan: IrPanResponder,
  event: IrPanEvent,
): CFunction {
  const prefix = pan.statePrefix;
  const callback = event.callback ? printCallback(event.callback) : [];
  const grantedGuard = [
    `    if (!${prefix}_granted)`,
    '    {',
    '        return;',
    '    }',
  ];

  let body: string[];
  switch (event.role) {
    case 'grant':
      body = [
        `    if (${prefix}_granted)`,
        '    {',
        '        return; /* a second finger re-granted this node: one gesture, not two */',
        '    }',
        `    ${prefix}_granted = 1;`,
        `    ${prefix}_base_dx = data->dx;`,
        `    ${prefix}_base_dy = data->dy;`,
        `    ${prefix}_x0 = data->x;`,
        `    ${prefix}_y0 = data->y;`,
        ...callback,
      ];
      break;
    case 'release':
    case 'terminate':
      // The callback runs while g.dx's anchor is still valid; the gesture is cleared after it.
      body = [
        ...grantedGuard,
        ...callback,
        `    ${prefix}_granted = 0;`,
        `    ${prefix}_base_dx = 0;`,
        `    ${prefix}_base_dy = 0;`,
      ];
      break;
    case 'move':
      body = [...grantedGuard, ...callback];
      break;
    case 'reject':
      body = callback;
      break;
  }

  return {name: panEventName(pan, event), body};
}

/**
 * A should-set query as the C function the engine calls: its name and its condition.
 *
 * @param pan  The responder.
 * @param query  The query.
 *
 * @returns The query function.
 */
export const printPanQuery = (
  pan: IrPanResponder,
  query: IrPanQuery,
): ResponderQuery => ({
  name: panQueryName(pan, query),
  expr: `(${printExpr(query.test)}) != 0`,
});
