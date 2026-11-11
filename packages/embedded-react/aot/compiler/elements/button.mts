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

import {evalStatic, evalStaticOrThrow} from '../static-eval.mts';
import {attrExpr} from '../style-text.mts';
import {emitNode} from '../nodes.mts';
import {
  attributeOf,
  elementOf,
  expressionChild,
  literalObject,
  refuseSpreads,
} from './rewrite.mts';
import {at} from './props.mts';
import type * as t from '@babel/types';
import type {AotError} from '../diagnostics.mts';
import type {Out} from '../out.mts';
import type {IrElement} from '../ir/nodes.mts';
import type {EmitOptions, Env, Scope, StateTable} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

// Button.js's styles. Flow B has no textAlign, so the Pressable centres the label instead.
const BUTTON_STYLE = {
  backgroundColor: '#2196F3',
  borderRadius: 2,
  alignItems: 'center',
};
const BUTTON_DISABLED_STYLE = {backgroundColor: '#dfdfdf'};
const TEXT_STYLE = {color: '#ffffff', padding: 8, fontWeight: '500'};
const TEXT_DISABLED_STYLE = {color: '#a1a1a1'};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <Button title onPress color disabled /> → the tree Button.js renders: a styled <Pressable> around one <Text>.
 *
 * @param element  The <Button>, its props already checked.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the element is placed.
 *
 * @returns What the <Pressable> lowers to.
 */
export function emitButton(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  refuseSpreads(element, 'Button');
  const attributes = new Map<string, t.JSXAttribute>();
  for (const attribute of element.openingElement.attributes) {
    attributes.set(
      (attribute as t.JSXAttribute).name.name as string,
      attribute as t.JSXAttribute,
    );
  }

  // `disabled` picks the styles and drops the press handler, so it must fold.
  const disabledAttribute = attributes.get('disabled');
  let disabled = false;
  if (disabledAttribute) {
    try {
      disabled = !!evalStaticOrThrow(
        attrExpr(disabledAttribute),
        scope,
        'AOT: <Button disabled> must be a compile-time constant',
        'it picks the grey styles and drops onPress at build time. To gate on state, leave it ' +
          'enabled and return early in the handler, or build the <Pressable> + <Text> yourself.',
      );
    } catch (error) {
      throw at(error as AotError, disabledAttribute);
    }
  }

  // A color that folds to nothing keeps the default blue, as `if (color)` does in Flow A.
  const buttonStyles: t.Expression[] = [literalObject(BUTTON_STYLE)];
  const textStyles: t.Expression[] = [literalObject(TEXT_STYLE)];
  const colorAttribute = attributes.get('color');
  if (colorAttribute && !disabled && !foldsFalsy(colorAttribute, scope)) {
    buttonStyles.push({
      type: 'ObjectExpression',
      properties: [
        {
          type: 'ObjectProperty',
          key: {type: 'Identifier', name: 'backgroundColor'},
          value: attrExpr(colorAttribute) as t.Expression,
          computed: false,
          shorthand: false,
        },
      ],
    });
  }
  if (disabled) {
    buttonStyles.push(literalObject(BUTTON_DISABLED_STYLE));
    textStyles.push(literalObject(TEXT_DISABLED_STYLE));
  }

  // The label, coerced as Flow A coerces it: a missing title draws nothing.
  const titleAttribute = attributes.get('title');
  const label = titleAttribute
    ? [
        expressionChild(
          titleText(attrExpr(titleAttribute) as t.Expression, scope),
        ),
      ]
    : [];
  const text = elementOf(
    'Text',
    [attributeOf('style', {type: 'ArrayExpression', elements: textStyles})],
    label,
    element,
  );

  const pressableAttributes = [
    attributeOf('style', {type: 'ArrayExpression', elements: buttonStyles}),
  ];
  const onPress = attributes.get('onPress');
  if (onPress && !disabled) {
    pressableAttributes.push(onPress);
  }

  return emitNode(
    elementOf('Pressable', pressableAttributes, [text], element),
    scope,
    output,
    environment,
    state,
    options,
  );
}

/**
 * A title as Button.js draws it, `title == null ? '' : String(title)`: worked out now when it folds, else
 * `String(title)`, so a boolean draws as "true" or "false" rather than nothing.
 *
 * @param title  The title.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The label's text.
 */
function titleText(title: t.Expression, scope: Scope): t.Expression {
  try {
    const value = evalStatic(title, scope);
    return {type: 'StringLiteral', value: value == null ? '' : String(value)};
  } catch {
    return {
      type: 'CallExpression',
      callee: {type: 'Identifier', name: 'String'},
      arguments: [title],
    };
  }
}

/**
 * Whether an attribute folds to a falsy value. One that does not fold is a value worked out at run time.
 *
 * @param attribute  The attribute.
 * @param scope  Compile-time constants in reach.
 *
 * @returns Whether it folds falsy.
 */
function foldsFalsy(attribute: t.JSXAttribute, scope: Scope): boolean {
  try {
    return !evalStatic(attrExpr(attribute), scope);
  } catch {
    return false;
  }
}
