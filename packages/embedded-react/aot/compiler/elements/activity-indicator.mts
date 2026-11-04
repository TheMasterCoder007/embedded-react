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

import {parseColorValue} from '../../style-map.mts';
import {aotError, currentLoc} from '../diagnostics.mts';
import {evalStatic} from '../static-eval.mts';
import {lowerExpr} from '../expressions.mts';
import {
  attrExpr,
  lowerColorValue,
  styleWrites,
  collectStyleAssigns,
} from '../style-text.mts';
import {checkInterpolations, lowerRefBinds} from '../nodes.mts';
import {at, elementSpec} from './props.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope} from '../types.mts';
import type {IrNode} from '../ir/nodes.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const SIZE_SMALL = 20;
const SIZE_LARGE = 36;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One of the typed element emitters that emitNodeImpl (nodes.mts) dispatches to.
 *
 * The engine spins it on its own (a looping rotate; render is a ring of 8 fading dots). No intrinsic size, so
 * a default box is set from `size` (small=20, large=36) unless style sets width/height.
 *
 * ```jsx
 * <ActivityIndicator color={…} size="small"|"large"|N animating={…} style={…} />
 *
 * <ActivityIndicator> -> ER_NODE_ACTIVITY_INDICATOR
 * ```
 *
 * @param element  The JSX element.
 * @param scope  The current scope.
 * @param output  The current output.
 * @param environment  The current environment.
 *
 * @returns The name of the node variable (Node ID).
 */
export function emitActivityIndicator(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
): IrNode {
  // get node id
  const nodeId = `n${output.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynamicAssigns, binds} = collectStyleAssigns(
    element.openingElement,
    scope,
    environment,
    elementSpec('ActivityIndicator')!,
  );
  const hasField = (field: string) =>
    styleWrites(staticAssigns, dynamicAssigns, field);

  // `style.color` tints the spinner, as in Flow A; a `color` prop written after it wins.
  for (const assign of [...staticAssigns, ...dynamicAssigns]) {
    if (assign.field === 'color') {
      assign.field = 'indicator_color';
    }
  }

  // process element attributes
  let size = SIZE_LARGE;
  for (const attribute of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attribute.type !== 'JSXAttribute') {
      throw aotError(
        'AOT: spread props on <ActivityIndicator> are not supported',
      );
    }

    // skip non-props
    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attribute);
    if (name === 'color') {
      try {
        staticAssigns.push({
          field: 'indicator_color',
          value: parseColorValue(String(evalStatic(node, scope))),
        });
      } catch {
        dynamicAssigns.push({
          field: 'indicator_color',
          value: lowerColorValue(node, environment),
        });
      }
    } else if (name === 'size') {
      const sizeValue = evalStatic(node, scope);
      size =
        sizeValue === 'small'
          ? SIZE_SMALL
          : sizeValue === 'large'
            ? SIZE_LARGE
            : typeof sizeValue === 'number' &&
                Number.isFinite(sizeValue) &&
                sizeValue > 0
              ? Math.round(sizeValue)
              : NaN;
      if (Number.isNaN(size)) {
        throw at(
          aotError(
            `AOT: <ActivityIndicator size> must be 'small', 'large' or a number of pixels (got ${JSON.stringify(sizeValue) ?? String(sizeValue)})`,
          ),
          attribute,
        );
      }
    } else if (name === 'animating') {
      try {
        staticAssigns.push({
          field: 'animating',
          value: evalStatic(node, scope) ? 1 : 0,
        });
      } catch {
        dynamicAssigns.push({
          field: 'animating',
          value: {
            kind: 'flag',
            test: lowerExpr(node, environment),
            truthiness: 'value',
            negate: false,
            whenTrue: 1,
          },
        });
      }
    } else {
      throw aotError(
        `AOT: <ActivityIndicator> prop "${name}" is not supported`,
        'supported props: color, size, animating, style.',
      );
    }
  }

  // default size: large unless style sets width/height
  if (!hasField('width')) {
    staticAssigns.push({field: 'width', value: size});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', value: size});
  }

  const node: IrNode = {
    id: nodeId,
    location: currentLoc(),
    type: 'ER_NODE_ACTIVITY_INDICATOR',
    constantProps: staticAssigns,
    stateProps: dynamicAssigns,
    placeholder: null,
    text: null,
    image: null,
    spans: null,
    children: [],
    wiring: [
      ...binds.map((bind, index) => ({kind: 'bind' as const, bind, index})),
      ...lowerRefBinds(element.openingElement, environment).map(slot => ({
        kind: 'ref' as const,
        slot,
      })),
    ],
  };

  checkInterpolations(node);
  return node;
}
