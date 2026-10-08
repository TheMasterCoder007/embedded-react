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
import {cstr} from '../c-syntax.mts';
import {emitExpr, emitFormat} from '../expressions.mts';
import {isFn} from '../collect.mts';
import {attrExpr, collectStyleAssigns} from '../style-text.mts';
import {compileValueHandler} from '../handlers.mts';
import {emitRefBind} from '../nodes.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {TextContent} from '../style-text.mts';
import type {Env, Scope, StateTable} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <TextInput value={text} onChangeText={(t) => setText(t)} placeholder="…" placeholderTextColor=… style=… />
 * → ER_NODE_TEXT_INPUT. The engine autofocuses on tap (hit_test) and edits its own buffer, firing
 * ER_EVENT_CHANGE_TEXT with the new text — bound to the handler's param via `data->changed_text` (a string).
 * `value` drives the text buffer (er_node_set_props → er_text_input_set_text; state → dynamic, re-synced in
 * app_update; set_text is a no-op when unchanged, so a controlled input is safe). Desktop types via the
 * keyboard; the touch-only CYD needs an on-screen keyboard to enter text (deferred follow-on).
 *
 * @param element  The <TextInput>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
export function emitTextInput(
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

  // process element attributes
  let valueNode: t.Node | null = null;
  let onChangeFn: t.Node | null = null;
  let placeholder: string | null = null;
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <TextInput> are not supported');
    }

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'style' || name === 'ref' || name === 'key') continue;

    // process props
    const node = attrExpr(attr);
    if (name === 'value' || name === 'defaultValue') {
      valueNode = node;
    } else if (name === 'onChangeText') {
      onChangeFn = node;
    } else if (name === 'placeholder') {
      placeholder = String(evalStatic(node, scope));
    } else if (name === 'placeholderTextColor') {
      staticAssigns.push({
        field: 'placeholder_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    } else if (name === 'cursorColor') {
      staticAssigns.push({
        field: 'cursor_color',
        expr: colorLiteral(String(evalStatic(node, scope))),
      });
    } else if (name === 'editable' || name === 'secureTextEntry') {
      const field = name === 'editable' ? 'editable' : 'secure_text_entry';
      try {
        staticAssigns.push({
          field,
          expr: evalStatic(node, scope) ? '1' : '0',
        });
      } catch {
        dynAssigns.push({
          field,
          code: `(uint8_t)((${emitExpr(node, env).code}) ? 1 : 0)`,
        });
      }
    } else if (
      [
        'autoFocus',
        'keyboardType',
        'maxLength',
        'multiline',
        'autoCapitalize',
        'autoCorrect',
        'returnKeyType',
        'onSubmitEditing',
        'onFocus',
        'onBlur',
      ].includes(name)
    ) {
      /* accepted but not yet lowered (no on-screen keyboard / submit wiring in the AOT path) */
    } else {
      throw aotError(
        `AOT: <TextInput> prop "${name}" is not supported`,
        'supported props: value, onChangeText, placeholder, placeholderTextColor, cursorColor, editable, secureTextEntry, style.',
      );
    }
  }

  // value → the input's text buffer (er_node_set_props → er_text_input_set_text): static literal, or a
  // state-driven value re-synced each app_update.
  let text: TextContent | null = null;
  if (valueNode) {
    const formatResult = emitFormat(valueNode, env, scope);
    text = {
      dynamic: formatResult.args.length > 0,
      format: formatResult.format,
      args: formatResult.args,
    };
  }

  // Create the text input node and either apply fixed props/text once or register it for app_update().
  const isDynamic = dynAssigns.length > 0 || (text && text.dynamic);
  out.build.push(`    ${nodeId} = er_node_create(ER_NODE_TEXT_INPUT);`);
  if (isDynamic) {
    out.build.push(`    s_${nodeId} = ${nodeId};`);
    out.handles.push(nodeId);
    out.updates.push({
      nodeId: nodeId,
      styleAssigns: staticAssigns,
      text,
      dynAssigns,
      placeholder,
    });
  } else {
    out.build.push(`    er_props_default(&p);`);
    for (const assign of staticAssigns) {
      out.build.push(`    p.${assign.field} = ${assign.expr};`);
    }
    if (placeholder != null) {
      out.build.push(
        `    snprintf(p.placeholder, sizeof(p.placeholder), "%s", ${cstr(placeholder)});`,
      );
    }
    if (text) {
      out.build.push(
        `    snprintf(p.text, sizeof(p.text), "%s", ${cstr(text.format.replace(/%%/g, '%'))});`,
      );
    }
    out.build.push(`    er_node_set_props(${nodeId}, &p);`);
  }

  // Compile onChangeText into an ER_EVENT_CHANGE_TEXT handler that receives the edited text.
  if (onChangeFn) {
    if (!isFn(onChangeFn)) {
      throw aotError(
        'AOT: onChangeText must be an inline function',
        'onChangeText={(t) => setText(t)}',
      );
    }
    const handlerName = `er_handler_${out.handlers.length}`;
    out.handlers.push({
      name: handlerName,
      body: compileValueHandler(
        onChangeFn,
        'data->changed_text',
        env,
        state,
        out,
        'string',
      ),
    });
    out.build.push(
      `    er_event_set(${nodeId}, ER_EVENT_CHANGE_TEXT, ${handlerName}, NULL);`,
    );
  }

  emitRefBind(nodeId, element.openingElement, out, env);

  return nodeId;
}
