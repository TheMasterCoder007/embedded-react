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

import {colorLiteral} from '../../style-map.mts';
import {aotError} from '../diagnostics.mts';
import {evalStatic} from '../static-eval.mts';
import {emitExpr} from '../expressions.mts';
import {
  attrExpr,
  emitColorExpr,
  styleWrites,
  collectStyleAssigns,
} from '../style-text.mts';
import {emitRefBind} from '../nodes.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope} from '../types.mts';

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
 * @param out  The current output.
 * @param env  The current environment.
 *
 * @returns The name of the node variable (Node ID).
 */
export function emitActivityIndicator(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
): string {
  // get node id
  const nodeId = `n${out.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    element.openingElement,
    scope,
    env,
  );
  const hasField = (field: string) =>
    styleWrites(staticAssigns, dynAssigns, field);

  // process element attributes
  let size = SIZE_LARGE;
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute') {
      throw aotError(
        'AOT: spread props on <ActivityIndicator> are not supported',
      );
    }

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attr);
    if (name === 'color') {
      try {
        staticAssigns.push({
          field: 'indicator_color',
          expr: colorLiteral(String(evalStatic(node, scope))),
        });
      } catch {
        dynAssigns.push({
          field: 'indicator_color',
          code: emitColorExpr(node, env),
        });
      }
    } else if (name === 'size') {
      const sizeValue = evalStatic(node, scope);
      size =
        sizeValue === 'small'
          ? SIZE_SMALL
          : sizeValue === 'large'
            ? SIZE_LARGE
            : Number(sizeValue) || SIZE_LARGE;
    } else if (name === 'animating') {
      try {
        staticAssigns.push({
          field: 'animating',
          expr: evalStatic(node, scope) ? '1' : '0',
        });
      } catch {
        dynAssigns.push({
          field: 'animating',
          code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)`,
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
    staticAssigns.push({field: 'width', expr: String(size)});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', expr: String(size)});
  }

  // Create the activity indicator node and either apply fixed props once or register it for app_update().
  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${nodeId} = er_node_create(ER_NODE_ACTIVITY_INDICATOR);`);
  if (isDynamic) {
    out.build.push(`    s_${nodeId} = ${nodeId};`);
    out.handles.push(nodeId);
    out.updates.push({
      nodeId,
      styleAssigns: staticAssigns,
      text: null,
      dynAssigns,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns) {
      out.build.push(`    p.${a.field} = ${a.expr};`);
    }
    out.build.push(`    er_node_set_props(${nodeId}, &p);`);
  }

  emitRefBind(nodeId, element.openingElement, out, env);

  return nodeId;
}
