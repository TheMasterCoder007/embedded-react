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

// Shared row handling for the list wrappers (<FlatList>, <SectionList>). Both turn a render callback's
// result into a keyed child of one <ScrollView>, and the awkward cases are the same for both: a row
// that renders to nothing, and a row that isn't an element at all.
import {cloneElement} from 'react';

/** True for a render result that puts nothing on screen — the `{cond && <Row/>}` of a row callback. */
export const rendersNothing = node =>
  node === null || node === undefined || node === false;

/** True for something cloneElement can key — a React element, as opposed to a string/number row. */
export const isElement = node =>
  node !== null && typeof node === 'object' && node.$$typeof !== undefined;

/**
 * Appends one rendered row to `out`, keyed, or drops it if it rendered nothing.
 *
 * Callers own the key because they know what makes one unique (a SectionList's rows are siblings
 * across sections, a FlatList's are not). A row that is not an element goes in untouched: there is no
 * key to attach to a raw string, and a plain `.map` wouldn't have attached one either.
 *
 * The key arrives as a FUNCTION, not a value, and that is the point: it is called only for a node that
 * can actually carry one. `keyExtractor` is the app's code — running it for a row that rendered
 * nothing, or for a raw string that cannot be cloned, is both wasted work on the commit path and a
 * side effect the app never asked for. Passing the key by value invites exactly that, and did.
 *
 * @param {Array} out The child list being built.
 * @param {*} node Whatever the render callback returned.
 * @param {(element: object) => string} keyFor Builds the React key, given the keyable element.
 */
export function pushKeyedChild(out, node, keyFor) {
  if (rendersNothing(node)) return;
  if (!isElement(node)) {
    out.push(node);
    return;
  }
  const key = keyFor(node);
  out.push(node.key === key ? node : cloneElement(node, {key}));
}
