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
 * Interned enums: a string state that only ever holds one of a closed set of literals (`mode`, `page`) is an
 * int slot holding an index into that set, so comparing it is an int compare and printing it is a lookup.
 *
 * A state is interned only when every use of it allows it: each write is a constant string, and each read is
 * compared with a string literal or with the state itself, printed in a text, or copied into a string local.
 * Any other read (a condition, a choice between strings, an image name) leaves it a string buffer.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {constantText} from '../diagnostics.mts';
import {nodesOf} from '../ir/nodes.mts';
import type {IrApp} from '../ir/app.mts';
import type {IrExpr, IrState} from '../ir/expressions.mts';
import type {IrText} from '../ir/text.mts';
import type {ScalarState} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// What an IR walk skips: where a value came from (which repeats its reads, untyped) and source positions.
const SKIPPED_KEYS = new Set(['source', 'loc']);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The string states that can be interned, each with its values: its initial value first, then each value a
 * write sets, in the order the app's code has them.
 *
 * @param app  The app.
 *
 * @returns The states and their values.
 */
export function internedStates(app: IrApp): Map<ScalarState, string[]> {
  const values = new Map<ScalarState, string[]>();
  for (const state of app.state) {
    if (state.kind === 'scalar' && state.cType === 'string' && !state.host) {
      values.set(state, [String(state.initial)]);
    }
  }
  const refused = new Set<ScalarState>();
  const isRead = (value: unknown): value is IrState =>
    (value as IrExpr | null)?.kind === 'state' &&
    values.has((value as IrState).state);

  const seen = new Set<object>();
  const walk = (value: unknown): void => {
    if (!value || typeof value !== 'object' || seen.has(value)) {
      return;
    }
    seen.add(value);
    if (Array.isArray(value)) {
      value.forEach(walk);
      return;
    }

    const node = value as Record<string, unknown> & {kind?: string};
    switch (node.kind) {
      case 'state':
        if (isRead(node)) {
          refused.add(node.state as ScalarState);
        }
        return;
      case 'stringEquals': {
        const {left, right} = node as unknown as {left: IrExpr; right: IrExpr};
        for (const [read, other] of [
          [left, right],
          [right, left],
        ]) {
          if (
            isRead(read) &&
            other.kind !== 'string' &&
            !(isRead(other) && other.state === read.state)
          ) {
            refused.add(read.state);
          }
        }
        [left, right].filter(side => !isRead(side)).forEach(walk);
        return;
      }
      case 'setStateText': {
        const {state, text} = node as unknown as {
          state: ScalarState;
          text: IrText;
        };
        const constant = constantText(text);
        if (values.has(state)) {
          if (constant === null) {
            refused.add(state);
          } else if (!values.get(state)!.includes(constant)) {
            values.get(state)!.push(constant);
          }
        }
        walk(text);
        return;
      }
      case 'declareLocal':
        if (!isRead(node.init)) {
          walk(node.init);
        }
        return;
    }

    // A text's value part prints the state's string, which the set holds.
    if (Array.isArray(node.parts)) {
      for (const part of node.parts as IrText['parts']) {
        if (!(part.kind === 'value' && isRead(part.value))) {
          walk(part);
        }
      }
    }
    for (const [key, child] of Object.entries(node)) {
      if (!SKIPPED_KEYS.has(key) && key !== 'parts') {
        walk(child);
      }
    }
  };

  const {out} = app;
  walk([
    nodesOf(app.root),
    out.handlers,
    out.timerFns,
    out.effectFns,
    out.mountEffects,
    out.depEffects,
    out.animCbs,
    out.panResponders,
    out.vectors,
    app.boot,
  ]);

  for (const state of refused) {
    values.delete(state);
  }
  return values;
}
