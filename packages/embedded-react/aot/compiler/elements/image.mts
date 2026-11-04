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
import {at} from './props.mts';
import {evalStatic, evalStaticOr} from '../static-eval.mts';
import {lowerExpr} from '../expressions.mts';
import {attrExpr, argbValue} from '../style-text.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {EngineName, Env} from '../types.mts';
import type {IrExpr} from '../ir/expressions.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An <Image>'s resolved props. At most one of `imageName` / `imageNameDynamic` is set. */
interface ImageAttrs {
  /** The baked asset's name, when the source folds. */
  imageName: string | null;
  /** The asset's name when the source is state-driven. */
  imageNameDynamic: IrExpr | null;
  /** The ER_RESIZE_* constant. */
  resizeMode: EngineName | null;
  /** The tint's ARGB literal. */
  tintColor: number | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const RESIZE_MODES: Record<string, EngineName> = {
  cover: 'ER_RESIZE_COVER',
  contain: 'ER_RESIZE_CONTAIN',
  stretch: 'ER_RESIZE_STRETCH',
  repeat: 'ER_RESIZE_REPEAT',
  center: 'ER_RESIZE_CENTER',
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Resolves an <Image source>/imageName expression to its baked asset NAME (a string) if it folds at compile
 * time, else null (a runtime/dynamic source — the caller emits a dynamic image_name). Image imports live in
 * the const scope as their asset-name string, so this folds `wxSun`, `item.icon` (unrolled map), and static
 * ternaries; `{ uri }` is the explicit remote-shape escape.
 *
 * @param expression  The source expression, if there is one.
 * @param environment  The expression environment.
 *
 * @returns The asset's name, or null when the source does not fold.
 */
export function imageNameFromSource(
  expression: t.Node | null | undefined,
  environment: Env,
): string | null {
  // No source expression was provided.
  if (!expression) return null;

  // Try to resolve the source as a compile-time constant.
  try {
    const value = evalStatic(expression, environment.constants ?? {});
    if (typeof value === 'string') return value;
  } catch {
    /* not a compile-time constant — fall through to {uri}, else dynamic */
  }

  // Handle React Native-style object sources: source={{ uri: 'asset_name' }}.
  if (expression.type === 'ObjectExpression') {
    const uri = expression.properties.find(
      (property): property is t.ObjectProperty =>
        property.type === 'ObjectProperty' &&
        ((property.key as t.Identifier).name === 'uri' ||
          (property.key as t.StringLiteral).value === 'uri'),
    );
    if (uri?.value?.type === 'StringLiteral') return uri.value.value;
  }

  return null;
}

/**
 * Resolves an <Image>'s source/imageName/resizeMode/tintColor for the node props (static name, a dynamic
 * name expr, resize mode, tint). Records baking intent in `output`: a static name that matches an import is
 * marked used (output.images); a dynamic source flips output.bakeAllImages (its asset can't be enumerated). So
 * only REACHED images are baked — an import used solely in a folded-away branch costs no flash.
 *
 * @param element  The <Image>.
 * @param environment  The expression environment.
 * @param output  Everything emitted so far.
 *
 * @returns The resolved props.
 */
export function resolveImageAttrs(
  element: t.JSXElement,
  environment: Env,
  output: Out,
): ImageAttrs {
  // Find the image source prop, preferring imageName over source.
  const attributes = element.openingElement.attributes;
  const find = (attributeName: string) =>
    attributes.find(
      (attribute): attribute is t.JSXAttribute =>
        attribute.type === 'JSXAttribute' &&
        attribute.name.name === attributeName,
    );
  const imageAttribute = find('imageName');
  const sourceAttribute = find('source');
  const sourceExpression = imageAttribute
    ? attrExpr(imageAttribute)
    : sourceAttribute
      ? attrExpr(sourceAttribute)
      : null;
  let imageName: string | null = null; // static asset name (a literal), OR …
  let imageNameDynamic: IrExpr | null = null; // … a runtime string (a list-item field / state) set in the update.

  // Resolve static sources immediately and mark imported assets for baking.
  if (sourceExpression) {
    imageName = imageNameFromSource(sourceExpression, environment);
    if (imageName != null) {
      const path = environment.imageNames?.get(imageName); // a baked import (vs. a bare {uri} name the app supplies)
      if (path) {
        output.images.set(imageName, path);
      }
    } else {
      // Fall back to a dynamic runtime image name when the source cannot be folded.
      // Emits it as a runtime string. The engine resolves it against the image registry by name each frame,
      // so the candidate assets must be baked. They can't be enumerated, so bake them ALL (output.bakeAllImages).
      // Only triggered when a dynamic source is actually REACHED.
      output.bakeAllImages = true;
      const nameExpression = lowerExpr(sourceExpression, environment);
      if (nameExpression.type.kind !== 'string') {
        const error = aotError(
          'AOT: an <Image source> must resolve to an asset NAME (a string)',
          "use an imported image (`import logo from './logo.png'` → source={logo}), " +
            "a string asset name, source={{ uri: 'name' }}, or a string-valued " +
            'state / list-item field for a dynamic source.',
        );

        if (sourceExpression.loc) {
          error.aotLoc = sourceExpression.loc.start;
        }
        {
          throw error;
        }
      }

      imageNameDynamic = nameExpression;
    }
  }

  // Resolve the optional resize mode and validate it against the supported engine modes.
  let resizeMode: EngineName | null = null;
  const resizeModeAttribute = find('resizeMode');
  if (resizeModeAttribute) {
    const resizeModeValue = evalStaticOr(
      attrExpr(resizeModeAttribute),
      environment,
      null,
    );
    resizeMode = RESIZE_MODES[resizeModeValue as string];
    if (!resizeMode) {
      const error = aotError(
        `AOT: unsupported <Image resizeMode> "${resizeModeValue}"`,
        `resizeMode must be one of: ${Object.keys(RESIZE_MODES).join(' / ')}.`,
      );
      if (resizeModeAttribute.loc) {
        error.aotLoc = resizeModeAttribute.loc.start;
      }
      throw error;
    }
  }

  // Resolve the optional tint color into the ARGB literal expected by the engine.
  let tintColor: number | null = null;
  const tintColorAttribute = find('tintColor');
  if (tintColorAttribute) {
    const tintValue = evalStaticOr(
      attrExpr(tintColorAttribute),
      environment,
      null,
    );
    if (typeof tintValue !== 'string' && typeof tintValue !== 'number') {
      throw at(
        aotError(
          'AOT: <Image tintColor> must be a constant color',
          'the tint is baked into the node when it is built; a state-driven one is not lowered yet.',
        ),
        tintColorAttribute,
      );
    }

    tintColor = argbValue(tintValue);
  }

  return {imageName, imageNameDynamic, resizeMode, tintColor};
}
