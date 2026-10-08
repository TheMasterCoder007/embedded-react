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
import {attrExpr, styleWrites, collectStyleAssigns} from '../style-text.mts';
import {emitChildren} from '../control-flow.mts';
import {emitRefBind} from '../nodes.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env, Scope, StateTable} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// Overlay defaults: absolute, fill the parent via four 0 insets (the robust "stretch" for an absolute
// node), center the content. The user's style overrides any of these.
const DEFAULTS = [
  ['position', 'ER_POS_ABSOLUTE'],
  ['left', '0'],
  ['top', '0'],
  ['right', '0'],
  ['bottom', '0'],
  ['align_items', 'ER_ALIGN_CENTER'],
  ['justify_content', 'ER_JUSTIFY_CENTER'],
];

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <Modal visible={show} backdropColor=… style=…>{content}</Modal> → ER_NODE_MODAL. The engine draws a
 * full-screen backdrop then the modal and its children when visible, and toggles the node's layout display
 * from `visible`. Defaults to an absolute full-screen overlay centring its content (style can override).
 * transparent / animationType / onRequestClose are accepted but currently no-ops.
 *
 * @param element  The <Modal>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitModal(
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

  // Apply modal overlay defaults only for fields the user's style has not already set.
  for (const [field, expr] of DEFAULTS) {
    if (!hasField(field)) {
      staticAssigns.push({field: field, expr});
    }
  }

  // process element attributes
  let visibleNode: t.Node | null = null;
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <Modal> are not supported');
    }

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attr);
    if (name === 'visible') {
      visibleNode = node;
    } else if (name === 'backdropColor') {
      staticAssigns.push({
        field: 'backdrop_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    } else if (
      name === 'transparent' ||
      name === 'animationType' ||
      name === 'onRequestClose' ||
      name === 'statusBarTranslucent'
    ) {
      /* accepted for RN compatibility; no-op in the AOT today */
    } else {
      throw aotError(
        `AOT: <Modal> prop "${name}" is not supported`,
        'supported: visible, backdropColor, style, children (transparent / animationType / onRequestClose are accepted but no-ops).',
      );
    }
  }

  // Require `visible` and lower it to the engine's modal_visible flag, folding constants when possible.
  if (!visibleNode) {
    throw aotError(
      'AOT: a <Modal> needs a visible prop',
      '<Modal visible={show}>…</Modal>',
    );
  }
  try {
    staticAssigns.push({
      field: 'modal_visible',
      expr: evalStatic(visibleNode, scope) ? '1' : '0',
    });
  } catch {
    dynAssigns.push({
      field: 'modal_visible',
      code: `(uint8_t)((${emitExpr(visibleNode, env).code}) ? 1 : 0)`,
    });
  }

  // Create the modal node and either apply fixed props once or register it for app_update().
  const isDynamic = dynAssigns.length > 0;
  out.build.push(`    ${nodeId} = er_node_create(ER_NODE_MODAL);`);
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
    for (const a of staticAssigns) {
      out.build.push(`    p.${a.field} = ${a.expr};`);
    }
    out.build.push(`    er_node_set_props(${nodeId}, &p);`);
  }

  emitRefBind(nodeId, element.openingElement, out, env);
  emitChildren(element.children, nodeId, scope, out, env, state); // the modal's content (shown/hidden with the modal)

  return nodeId;
}
