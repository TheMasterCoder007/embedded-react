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
import {asCond, emitExpr} from '../expressions.mts';
import {isFn} from '../collect.mts';
import {attrExpr, styleWrites, collectStyleAssigns} from '../style-text.mts';
import {compileValueHandler} from '../handlers.mts';
import {emitRefBind} from '../nodes.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope, StateTable} from '../types.mts';

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
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitSwitch(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): string {
  // get node id
  const nodeId = `n${out.allocateNodeId()}`;

  // collect style assignments
  const {staticAssigns, dynAssigns} = collectStyleAssigns(
    element.openingElement,
    scope,
    env,
  );
  const hasField = (f: string) => styleWrites(staticAssigns, dynAssigns, f);
  if (!hasField('width')) {
    staticAssigns.push({field: 'width', expr: '51'});
  }
  if (!hasField('height')) {
    staticAssigns.push({field: 'height', expr: '31'});
  }

  // process element attributes
  let valueNode: t.Node | null = null;
  let onChangeFn: t.Node | null = null;
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <Switch> are not supported');
    }

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attr);
    if (name === 'value') {
      valueNode = node;
    } else if (name === 'onValueChange') {
      onChangeFn = node;
    } else if (name === 'thumbColor') {
      staticAssigns.push({
        field: 'thumb_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    } else if (name === 'trackColor') {
      const trackColor = evalStatic(node, scope) as
        | {false?: unknown; true?: unknown}
        | null
        | undefined;
      if (trackColor?.false != null)
        staticAssigns.push({
          field: 'track_color_false',
          expr: colorLiteral(String(trackColor.false)),
        });
      if (trackColor?.true != null)
        staticAssigns.push({
          field: 'track_color_true',
          expr: colorLiteral(String(trackColor.true)),
        });
    } else if (name === 'disabled') {
      /* accepted; the AOT has no disabled-visual yet, so it is a no-op */
    } else {
      throw aotError(
        `AOT: <Switch> prop "${name}" is not supported`,
        'supported props: value, onValueChange, trackColor, thumbColor, style.',
      );
    }
  }

  // value → switch_value (static or, when state-driven, recomputed in app_update).
  if (valueNode) {
    try {
      staticAssigns.push({
        field: 'switch_value',
        expr: evalStatic(valueNode, scope) ? '1' : '0',
      });
    } catch {
      dynAssigns.push({
        field: 'switch_value',
        code: `(uint8_t)((${asCond(emitExpr(valueNode, env))}) ? 1 : 0)`,
      });
    }
  }

  // Create the switch node and either apply fixed props once or register it for app_update().
  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${nodeId} = er_node_create(ER_NODE_SWITCH);`);
  if (isDynamic) {
    out.build.push(`    s_${nodeId} = ${nodeId};`);
    out.handles.push(nodeId);
    out.updates.push({
      nodeId: nodeId,
      styleAssigns: staticAssigns,
      text: null,
      dynAssigns,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const a of staticAssigns)
      out.build.push(`    p.${a.field} = ${a.expr};`);
    out.build.push(`    er_node_set_props(${nodeId}, &p);`);
  }

  // Compile onValueChange into a press handler that receives the toggled switch value.
  if (onChangeFn) {
    // Require a controlled switch so onValueChange can receive the next value.
    if (!isFn(onChangeFn))
      throw aotError(
        'AOT: onValueChange must be an inline function',
        'onValueChange={(v) => setX(v)}',
      );
    if (!valueNode)
      throw aotError(
        'AOT: a <Switch> with onValueChange needs a value prop',
        'controlled switch: <Switch value={on} onValueChange={(v) => setOn(v)} />',
      );

    // Compile onValueChange into an ER_EVENT_PRESS handler, passing the next switch value.
    const handlerName = `er_handler_${out.handlers.length}`;
    const toggled = `(!(${asCond(emitExpr(valueNode, env))}))`; // the engine toggles on press → param is !value
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        toggled,
        env,
        state,
        out,
        'int',
        null,
        true,
      ),
    });
    out.build.push(
      `    er_event_set(${nodeId}, ER_EVENT_PRESS, ${handlerName}, NULL);`,
    );
  }

  emitRefBind(nodeId, element.openingElement, out, env);

  return nodeId;
}
