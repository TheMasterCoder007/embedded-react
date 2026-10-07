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

import {aotError} from '../diagnostics.mts';
import {isFn} from '../collect.mts';
import {attrExpr} from '../style-text.mts';
import {emitNode} from '../nodes.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {EmitOptions, Env, Scope, StateTable} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <FlatList data={items} renderItem={({ item, index }) => <Row …/>} keyExtractor=… style=… /> → the SAME as
 * <ScrollView style=…>{items.map((item, index) => <Row …/>)}</ScrollView>. The engine's FlatList IS a
 * ScrollView (no virtualization), and the AOT already unrolls a .map (static or state-list), so this is a thin
 * API-compat rewrite: synthesize that ScrollView+map AST and emit it. keyExtractor is ignored (no reconciler).
 *
 * @param element  The <FlatList>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param opts  How the element is placed.
 *
 * @returns The variable of the <ScrollView> node it lowers to.
 */
export function emitFlatList(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts?: EmitOptions,
): string {
  // Collect the supported <FlatList> props, rejecting anything the AOT rewrite cannot statically lower.
  let dataNode: t.Node | null = null;
  let renderItem: t.Node | null = null;
  let styleAttr: t.JSXAttribute | null = null;
  for (const attr of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attr.type !== 'JSXAttribute')
      throw aotError('AOT: spread props on <FlatList> are not supported');

    // skip non-props
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'keyExtractor' || name === 'ref' || name === 'key') continue;

    // process props
    if (name === 'data') {
      dataNode = attrExpr(attr);
    } else if (name === 'renderItem') {
      renderItem = attrExpr(attr);
    } else if (name === 'style') {
      styleAttr = attr;
    } else
      throw aotError(
        `AOT: <FlatList> prop "${name}" is not supported`,
        'supported: data, renderItem, keyExtractor, style. For headers/footers/horizontal/onEndReached etc., use <ScrollView> + .map directly.',
      );
  }

  // Require the FlatList inputs needed for the rewrite: a data source and a renderItem callback that destructures its row info.
  if (!dataNode)
    throw aotError(
      'AOT: <FlatList> needs a data prop',
      '<FlatList data={items} renderItem={({ item }) => <Row item={item} />} />',
    );
  if (!renderItem || !isFn(renderItem))
    throw aotError(
      'AOT: <FlatList> needs a renderItem function',
      'renderItem={({ item, index }) => <Row item={item} />}',
    );
  const param = renderItem.params[0];
  if (!param || param.type !== 'ObjectPattern')
    throw aotError(
      'AOT: FlatList renderItem must destructure ({ item, index })',
      'renderItem={({ item }) => <Row item={item} />}',
    );

  // Extract the local item/index names from renderItem's destructured parameter, rejecting any unsupported row fields or patterns.
  let itemName: string | null = null;
  let indexName: string | null = null;
  for (const prop of param.properties) {
    if (prop.type !== 'ObjectProperty' || prop.value.type !== 'Identifier')
      throw aotError(
        'AOT: FlatList renderItem may destructure only item / index (to plain names)',
      );
    if ((prop.key as t.Identifier).name === 'item') itemName = prop.value.name;
    else if ((prop.key as t.Identifier).name === 'index')
      indexName = prop.value.name;
    else
      throw aotError(
        `AOT: FlatList renderItem cannot destructure "${(prop.key as t.Identifier).name}" (only item / index)`,
      );
  }

  // Ensure renderItem binds the required item value; index is optional for the generated map callback.
  if (!itemName) {
    throw aotError(
      'AOT: FlatList renderItem must destructure item',
      'renderItem={({ item }) => …}',
    );
  }

  // Rewrite renderItem `({ item, index }) => BODY` → a positional `.map` callback `(item, index) => BODY`.
  const cbParams: t.Identifier[] = [{type: 'Identifier', name: itemName}];
  if (indexName) cbParams.push({type: 'Identifier', name: indexName});
  const cb: t.ArrowFunctionExpression = {
    type: 'ArrowFunctionExpression',
    params: cbParams,
    body: renderItem.body,
    async: false,
    expression: renderItem.body.type !== 'BlockStatement',
  };
  const mapCall: t.CallExpression = {
    type: 'CallExpression',
    callee: {
      type: 'MemberExpression',
      object: dataNode as t.Expression,
      property: {type: 'Identifier', name: 'map'},
      computed: false,
    },
    arguments: [cb],
  };
  const scrollView: t.JSXElement = {
    type: 'JSXElement',
    openingElement: {
      type: 'JSXOpeningElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
      attributes: styleAttr ? [styleAttr] : [],
      selfClosing: false,
    },
    closingElement: {
      type: 'JSXClosingElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
    },
    children: [{type: 'JSXExpressionContainer', expression: mapCall}],
  };
  return emitNode(scrollView, scope, out, env, state, opts);
}
