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
 * A handler, effect or timer reads the state its render had: `setCount(count + 1)` leaves `count` as it was for the
 * rest of the body. A setter writes its slot at once, so a body that sets a state and then reads it starts by
 * keeping a copy, which its reads use. An updater's parameter and a reducer's state still read the slot.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {defersTyping, whenTyped} from './check.mts';
import type {IrStmt} from './ir/statements.mts';
import type {Out} from './out.mts';
import type {ScalarState} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a walk doesn't follow: an expression before typing repeats its reads, and the rest hold none. */
const SKIPPED_KEYS = new Set(['source', 'location', 'readsLatest']);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Marks the statements an object state's updater or reducer lowered to, so their reads of its fields give what they
 * hold now.
 *
 * @param statements  The statements.
 * @param states  The slots of the state set.
 *
 * @returns The statements, marked, and those inside them.
 */
export function readingLatest(
  statements: IrStmt[],
  states: ReadonlySet<ScalarState>,
): IrStmt[] {
  if (!states.size) return statements;
  // Each is marked in place: the typing pass has yet to fill some in, which a copy would miss.
  return statements.map(statement => {
    const marked = Object.assign(statement, {readsLatest: states});
    switch (marked.kind) {
      case 'if':
        marked.consequent = readingLatest(marked.consequent, states);
        marked.alternate =
          marked.alternate && readingLatest(marked.alternate, states);
        break;
      case 'block':
      case 'updateItems':
        marked.body = readingLatest(marked.body, states);
        break;
      case 'loop':
        marked.body = readingLatest(marked.body, states);
        marked.step = readingLatest(marked.step, states);
        break;
    }
    return marked;
  });
}

/**
 * The states a body reads after setting them.
 *
 * @param body  The body's statements.
 *
 * @returns The states, in the order they are first read so.
 */
function statesReadAfterSet(body: IrStmt[]): ScalarState[] {
  let written = new Set<ScalarState>();
  const found = new Set<ScalarState>();
  const path = new Set<object>();

  /** Every state the statements under a value set. */
  const sets = (value: unknown, into: Set<ScalarState>): void => {
    if (!value || typeof value !== 'object' || path.has(value)) return;
    if (Array.isArray(value)) {
      value.forEach(element => sets(element, into));
      return;
    }
    const node = value as Record<string, unknown>;
    if ('slotName' in node) return;
    if (node.kind === 'setState' || node.kind === 'setStateText') {
      into.add(node.state as ScalarState);
    }
    path.add(node);
    for (const [key, child] of Object.entries(node)) {
      if (!SKIPPED_KEYS.has(key)) sets(child, into);
    }
    path.delete(node);
  };

  const walk = (
    value: unknown,
    latest: ReadonlySet<ScalarState> | undefined,
  ): void => {
    if (!value || typeof value !== 'object' || path.has(value)) return;
    if (Array.isArray(value)) {
      value.forEach(element => walk(element, latest));
      return;
    }
    const node = value as Record<string, unknown>;
    // A state or a list itself, which a statement names.
    if ('slotName' in node) return;
    if (node.kind === 'state' && 'type' in node) {
      const state = node.state as ScalarState;
      if (written.has(state) && !node.latest && !latest?.has(state)) {
        found.add(state);
      }
      return;
    }
    // A field given back as it was reads it, where the body set it before.
    if (node.kind === 'keepField') {
      const state = node.state as ScalarState;
      const live =
        (node.readsLatest as ReadonlySet<ScalarState> | undefined) ?? latest;
      if (written.has(state) && !live?.has(state)) found.add(state);
      return;
    }
    const reads =
      (node.readsLatest as ReadonlySet<ScalarState> | undefined) ?? latest;
    // Only one branch runs, so neither reads what the other set.
    if (node.kind === 'if') {
      walk(node.test, reads);
      // A branch that returns sets nothing for what comes after.
      const returns = (branch: unknown): boolean =>
        Array.isArray(branch) && branch.at(-1)?.kind === 'return';
      const before = new Set(written);
      walk(node.consequent, reads);
      const consequent = returns(node.consequent) ? before : written;
      written = new Set(before);
      walk(node.alternate, reads);
      if (returns(node.alternate)) written = new Set(before);
      consequent.forEach(state => written.add(state));
      return;
    }
    // A loop's later visits read what its earlier ones set.
    if (node.kind === 'loop') sets(node, written);
    path.add(node);
    for (const [key, child] of Object.entries(node)) {
      if (!SKIPPED_KEYS.has(key)) walk(child, reads);
    }
    path.delete(node);
    // A setter's own value reads the state before it is set.
    if (node.kind === 'setState' || node.kind === 'setStateText') {
      written.add(node.state as ScalarState);
    }
  };

  walk(body, undefined);
  return [...found];
}

/**
 * Starts a body by keeping a copy of each state it sets and then reads, for those reads.
 *
 * @param body  The body's statements.
 * @param output  Everything emitted so far; the states kept are added to it.
 *
 * @returns The body, after the copies when it needs any.
 */
export function keepingStates(body: IrStmt[], output: Out): IrStmt[] {
  // What the body reads is known once it is typed, so the copies are found then: none leaves nothing to keep.
  const states: ScalarState[] = [];
  const kept: IrStmt[] = [{kind: 'keepStates', states}, ...body];
  whenTyped(() => {
    states.push(...statesReadAfterSet(body));
    states.forEach(state => output.keptStates.add(state));
    if (!states.length && kept[0]?.kind === 'keepStates') {
      kept.shift();
    }
  });
  return defersTyping() || states.length ? kept : body;
}
