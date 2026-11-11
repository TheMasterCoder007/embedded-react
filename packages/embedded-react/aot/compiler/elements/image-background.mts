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

import {attrExpr} from '../style-text.mts';
import {emitNode} from '../nodes.mts';
import {
  attributeOf,
  elementOf,
  literalObject,
  refuseSpreads,
  renamed,
} from './rewrite.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {IrElement} from '../ir/nodes.mts';
import type {EmitOptions, Env, Scope, StateTable} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** StyleSheet.absoluteFill: the image fills the container's padding box. */
const ABSOLUTE_FILL = {
  position: 'absolute',
  left: 0,
  right: 0,
  top: 0,
  bottom: 0,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * <ImageBackground style imageStyle imageRef …imageProps>children</ImageBackground> → the tree
 * ImageBackground.js renders: a <View> holding an <Image> that fills it, then the children. `style` and `ref`
 * go to the View, every other prop to the Image.
 *
 * @param element  The <ImageBackground>, its props already checked.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the element is placed.
 *
 * @returns What the <View> lowers to.
 */
export function emitImageBackground(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options?: EmitOptions,
): IrElement {
  refuseSpreads(element, 'ImageBackground');
  const viewAttributes: t.JSXAttribute[] = [];
  const imageAttributes: t.JSXAttribute[] = [];
  const imageStyles: t.Expression[] = [literalObject(ABSOLUTE_FILL)];
  for (const attribute of element.openingElement
    .attributes as t.JSXAttribute[]) {
    const name = attribute.name.name as string;
    if (name === 'key') continue;

    if (name === 'style' || name === 'ref') {
      viewAttributes.push(attribute);
    } else if (name === 'imageStyle') {
      imageStyles.push(attrExpr(attribute) as t.Expression);
    } else if (name === 'imageRef') {
      imageAttributes.push(renamed(attribute, 'ref'));
    } else {
      imageAttributes.push(attribute);
    }
  }

  imageAttributes.push(
    attributeOf('style', {type: 'ArrayExpression', elements: imageStyles}),
  );
  const image = elementOf('Image', imageAttributes, [], element);
  return emitNode(
    elementOf('View', viewAttributes, [image, ...element.children], element),
    scope,
    output,
    environment,
    state,
    options,
  );
}
