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
import {BOOL} from '../ir/types.mts';
import {isFn} from '../collect.mts';
import {attrExpr, styleWrites, collectStyleAssigns} from '../style-text.mts';
import {lowerValueHandler} from '../handlers.mts';
import {checkInterpolations, lowerRefBinds} from '../nodes.mts';
import {at, elementSpec} from './props.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope, StateTable} from '../types.mts';
import type {IrNode} from '../ir/nodes.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** A <Switch>'s size when its style sets none, as iOS draws it. */
const DEFAULT_WIDTH = 51;
const DEFAULT_HEIGHT = 31;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <Switch value={on} onValueChange={(v) => setOn(v)} trackColor={{false,true}} thumbColor=… style=… />
 * → ER_NODE_SWITCH. The engine flips its own value on press (+ animates the thumb), then fires ER_EVENT_PRESS,
 * so onValueChange maps to PRESS, and its `v` param is the TOGGLED value (!value). `value` drives switch_value
 * (state → dynamic). Default RN 51×31 box (the renderer scales the track/thumb to it); style can override.
 *
 * @param element  The <Switch>.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitSwitch(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrNode {
  // get node id
  const nodeId = `n${output.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynamicAssigns, binds} = collectStyleAssigns(
    element.openingElement,
    scope,
    environment,
    elementSpec('Switch')!,
  );
  const hasField = (field: string) =>
    styleWrites(staticAssigns, dynamicAssigns, field);
  if (!hasField('width')) {
    staticAssigns.push({field: 'width', value: DEFAULT_WIDTH});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', value: DEFAULT_HEIGHT});
  }

  // process element attributes
  let valueNode: t.Node | null = null;
  let onChangeFunction: t.Node | null = null;
  for (const attribute of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attribute.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <Switch> are not supported');
    }

    // skip non-props
    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attribute);
    if (name === 'value') {
      valueNode = node;
    } else if (name === 'onValueChange') {
      onChangeFunction = node;
    } else if (name === 'thumbColor') {
      staticAssigns.push({
        field: 'thumb_color',
        value: parseColorValue(String(evalStatic(node, scope))),
      });
    } else if (name === 'trackColor') {
      const trackColor = evalStatic(node, scope) as {
        false?: unknown;
        true?: unknown;
      } | null;
      if (
        trackColor == null ||
        typeof trackColor !== 'object' ||
        Object.keys(trackColor).some(key => key !== 'false' && key !== 'true')
      ) {
        throw at(
          aotError(
            'AOT: <Switch trackColor> must be an object of two colors',
            "trackColor={{false: '#767577', true: '#81b0ff'}}",
          ),
          attribute,
        );
      }

      if (trackColor.false != null) {
        staticAssigns.push({
          field: 'track_color_false',
          value: parseColorValue(String(trackColor.false)),
        });
      }
      if (trackColor.true != null) {
        staticAssigns.push({
          field: 'track_color_true',
          value: parseColorValue(String(trackColor.true)),
        });
      }
    } else if (name === 'disabled') {
      // The engine flips a Switch on every press, so only a Switch that is never disabled lowers.
      let disabled: unknown = true;
      try {
        disabled = evalStatic(node, scope);
      } catch {
        /* state-driven: refused below */
      }

      if (disabled !== false) {
        throw at(
          aotError(
            'AOT: a disabled <Switch> is not supported',
            'the engine flips a Switch on every press and has no disabled state. Leave it enabled and ' +
              'ignore the change in onValueChange while it should not move.',
          ),
          attribute,
        );
      }
    } else {
      throw aotError(
        `AOT: <Switch> prop "${name}" is not supported`,
        'supported props: value, onValueChange, trackColor, thumbColor, style.',
      );
    }
  }

  // value → switch_value (static or, when state-driven, recomputed in the update).
  if (valueNode) {
    try {
      staticAssigns.push({
        field: 'switch_value',
        value: evalStatic(valueNode, scope) ? 1 : 0,
      });
    } catch {
      dynamicAssigns.push({
        field: 'switch_value',
        value: {
          kind: 'flag',
          test: lowerExpr(valueNode, environment),
          truthiness: 'condition',
          negate: false,
          whenTrue: 1,
        },
      });
    }
  }

  const node: IrNode = {
    id: nodeId,
    location: currentLoc(),
    type: 'ER_NODE_SWITCH',
    constantProps: staticAssigns,
    stateProps: dynamicAssigns,
    placeholder: null,
    text: null,
    image: null,
    spans: null,
    children: [],
    wiring: binds.map((bind, index) => ({kind: 'bind', bind, index})),
  };

  // Compile onValueChange into a press handler that receives the toggled switch value.
  if (onChangeFunction) {
    // Require a controlled switch so onValueChange can receive the next value.
    if (!isFn(onChangeFunction)) {
      throw aotError(
        'AOT: onValueChange must be an inline function',
        'onValueChange={(v) => setX(v)}',
      );
    }
    if (!valueNode) {
      throw aotError(
        'AOT: a <Switch> with onValueChange needs a value prop',
        'controlled switch: <Switch value={on} onValueChange={(v) => setOn(v)} />',
      );
    }

    // Compile onValueChange into an ER_EVENT_PRESS handler, passing the next switch value.
    const handlerName = `handler_${output.handlers.length}`;
    const value = lowerExpr(valueNode, environment);
    output.handlers.push({
      kind: 'callback',
      name: handlerName,
      callback: lowerValueHandler(
        onChangeFunction,
        environment,
        state,
        output,
        'int',
        true,
        {kind: 'unary', operator: '!', operand: value, type: BOOL}, // the engine toggles on press → param is !value
      ),
    });
    node.wiring.push({
      kind: 'event',
      event: 'ER_EVENT_PRESS',
      handler: handlerName,
    });
  }

  for (const slot of lowerRefBinds(element.openingElement, environment)) {
    node.wiring.push({kind: 'ref', slot});
  }

  checkInterpolations(node);
  return node;
}
