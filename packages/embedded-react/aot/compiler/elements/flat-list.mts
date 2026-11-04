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
import type {IrElement} from '../ir/nodes.mts';
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
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the element is placed.
 *
 * @returns The variable of the <ScrollView> node it lowers to.
 */
export function emitFlatList(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  // Collect the supported <FlatList> props, rejecting anything the AOT rewrite cannot statically lower.
  let dataNode: t.Node | null = null;
  let renderItem: t.Node | null = null;
  let styleAttribute: t.JSXAttribute | null = null;
  for (const attribute of element.openingElement.attributes) {
    // Ensure all props are explicit named attributes.
    // Spread props cannot be analyzed statically
    if (attribute.type !== 'JSXAttribute') {
      throw aotError('AOT: spread props on <FlatList> are not supported');
    }

    // skip non-props
    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'keyExtractor' || name === 'key') continue;

    // process props
    if (name === 'data') {
      dataNode = attrExpr(attribute);
    } else if (name === 'renderItem') {
      renderItem = attrExpr(attribute);
    } else if (name === 'style') {
      styleAttribute = attribute;
    } else {
      throw aotError(
        `AOT: <FlatList> prop "${name}" is not supported`,
        'supported: data, renderItem, keyExtractor, style. For headers/footers/horizontal/onEndReached etc., use <ScrollView> + .map directly.',
      );
    }
  }

  // Require the FlatList inputs needed for the rewrite: a data source and a renderItem callback that destructures its row info.
  if (!dataNode) {
    throw aotError(
      'AOT: <FlatList> needs a data prop',
      '<FlatList data={items} renderItem={({ item }) => <Row item={item} />} />',
    );
  }
  if (!renderItem || !isFn(renderItem)) {
    throw aotError(
      'AOT: <FlatList> needs a renderItem function',
      'renderItem={({ item, index }) => <Row item={item} />}',
    );
  }
  const parameter = renderItem.params[0];
  if (!parameter || parameter.type !== 'ObjectPattern') {
    throw aotError(
      'AOT: FlatList renderItem must destructure ({ item, index })',
      'renderItem={({ item }) => <Row item={item} />}',
    );
  }

  // Extract the local item/index names from renderItem's destructured parameter, rejecting any unsupported row fields or patterns.
  let itemName: string | null = null;
  let indexName: string | null = null;
  for (const prop of parameter.properties) {
    if (prop.type !== 'ObjectProperty' || prop.value.type !== 'Identifier') {
      throw aotError(
        'AOT: FlatList renderItem may destructure only item / index (to plain names)',
      );
    }
    if ((prop.key as t.Identifier).name === 'item') {
      itemName = prop.value.name;
    } else if ((prop.key as t.Identifier).name === 'index') {
      indexName = prop.value.name;
    } else {
      throw aotError(
        `AOT: FlatList renderItem cannot destructure "${(prop.key as t.Identifier).name}" (only item / index)`,
      );
    }
  }

  // Ensure renderItem binds the required item value; index is optional for the map callback.
  if (!itemName) {
    throw aotError(
      'AOT: FlatList renderItem must destructure item',
      'renderItem={({ item }) => …}',
    );
  }

  // Rewrite renderItem `({ item, index }) => BODY` → a positional `.map` callback `(item, index) => BODY`.
  const callbackParameters: t.Identifier[] = [
    {type: 'Identifier', name: itemName},
  ];
  if (indexName) {
    callbackParameters.push({type: 'Identifier', name: indexName});
  }
  const callback: t.ArrowFunctionExpression = {
    type: 'ArrowFunctionExpression',
    params: callbackParameters,
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
    arguments: [callback],
  };
  const scrollView: t.JSXElement = {
    type: 'JSXElement',
    openingElement: {
      type: 'JSXOpeningElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
      attributes: styleAttribute ? [styleAttribute] : [],
      selfClosing: false,
    },
    closingElement: {
      type: 'JSXClosingElement',
      name: {type: 'JSXIdentifier', name: 'ScrollView'},
    },
    children: [{type: 'JSXExpressionContainer', expression: mapCall}],
  };
  return emitNode(scrollView, scope, output, environment, state, options);
}
