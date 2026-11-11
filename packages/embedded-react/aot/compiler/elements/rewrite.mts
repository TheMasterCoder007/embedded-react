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

// Builders for the JSX the wrapper components (Button, ImageBackground, SectionList) are rewritten to.

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from '../diagnostics.mts';
import {at} from './props.mts';
import type * as t from '@babel/types';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses a spread on a wrapper, whose props are split by name.
 *
 * @param element  The wrapper.
 * @param tag  Its tag, for the error.
 */
export function refuseSpreads(element: t.JSXElement, tag: string): void {
  const spread = element.openingElement.attributes.find(
    attribute => attribute.type === 'JSXSpreadAttribute',
  );
  if (spread) {
    throw at(
      aotError(
        `AOT: a spread {...} on <${tag}> is not supported`,
        'list each prop explicitly.',
      ),
      spread,
    );
  }
}

/**
 * A JSX element, located at the wrapper it stands for so its errors point there.
 *
 * @param tag  The tag.
 * @param attributes  Its attributes.
 * @param children  Its children.
 * @param from  The wrapper.
 *
 * @returns The element.
 */
export function elementOf(
  tag: string,
  attributes: t.JSXAttribute[],
  children: t.JSXElement['children'],
  from: t.JSXElement,
): t.JSXElement {
  const name: t.JSXIdentifier = {type: 'JSXIdentifier', name: tag};
  return {
    type: 'JSXElement',
    openingElement: {
      type: 'JSXOpeningElement',
      name,
      attributes,
      selfClosing: children.length === 0,
    },
    closingElement:
      children.length === 0 ? null : {type: 'JSXClosingElement', name},
    children,
    loc: from.loc,
  };
}

/**
 * A `name={value}` attribute.
 *
 * @param name  The name.
 * @param value  The value.
 *
 * @returns The attribute.
 */
export function attributeOf(name: string, value: t.Expression): t.JSXAttribute {
  return {
    type: 'JSXAttribute',
    name: {type: 'JSXIdentifier', name},
    value: {type: 'JSXExpressionContainer', expression: value},
  };
}

/**
 * An attribute under another name, keeping its value and location.
 *
 * @param attribute  The attribute.
 * @param name  The new name.
 *
 * @returns The renamed attribute.
 */
export function renamed(
  attribute: t.JSXAttribute,
  name: string,
): t.JSXAttribute {
  return {...attribute, name: {type: 'JSXIdentifier', name}};
}

/**
 * A `{value}` child.
 *
 * @param value  The expression.
 *
 * @returns The child.
 */
export function expressionChild(value: t.Expression): t.JSXExpressionContainer {
  return {type: 'JSXExpressionContainer', expression: value};
}

/**
 * An object literal of strings and numbers.
 *
 * @param fields  The fields.
 *
 * @returns The literal.
 */
export function literalObject(
  fields: Record<string, string | number>,
): t.ObjectExpression {
  return {
    type: 'ObjectExpression',
    properties: Object.entries(fields).map(([key, value]) => ({
      type: 'ObjectProperty',
      key: {type: 'Identifier', name: key},
      value:
        typeof value === 'number'
          ? {type: 'NumericLiteral', value}
          : {type: 'StringLiteral', value},
      computed: false,
      shorthand: false,
    })),
  };
}
