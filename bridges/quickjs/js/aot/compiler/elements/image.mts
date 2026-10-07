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
import {evalStatic, evalStaticOr} from '../static-eval.mts';
import {emitExpr} from '../expressions.mts';
import {attrExpr, argbLiteral} from '../style-text.mts';
import type * as t from '@babel/types';
import type {Out} from '../out.mts';
import type {Env} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An <Image>'s resolved props. At most one of `imageName` / `imageNameDyn` is set. */
interface ImageAttrs {
  /** The baked asset's name, when the source folds. */
  imageName: string | null;
  /** C for the asset's name when the source is state-driven. */
  imageNameDyn: string | null;
  /** The ER_RESIZE_* constant. */
  resizeMode: string | null;
  /** The tint's ARGB literal. */
  tintColor: string | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const RESIZE_MODES: Record<string, string> = {
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
 * @param expr  The source expression, if there is one.
 * @param env  The expression environment.
 *
 * @returns The asset's name, or null when the source does not fold.
 */
export function imageNameFromSource(
  expr: t.Node | null | undefined,
  env: Env,
): string | null {
  // No source expression was provided.
  if (!expr) return null;

  // Try to resolve the source as a compile-time constant.
  try {
    const value = evalStatic(expr, env.consts ?? {});
    if (typeof value === 'string') return value;
  } catch {
    /* not a compile-time constant — fall through to {uri}, else dynamic */
  }

  // Handle React Native-style object sources: source={{ uri: 'asset_name' }}.
  if (expr.type === 'ObjectExpression') {
    const uri = expr.properties.find(
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
 * name expr, resize mode, tint). Records baking intent in `out`: a static name that matches an import is
 * marked used (out.images); a dynamic source flips out.bakeAllImages (its asset can't be enumerated). So
 * only REACHED images are baked — an import used solely in a folded-away branch costs no flash.
 *
 * @param element  The <Image>.
 * @param env  The expression environment.
 * @param out  Everything emitted so far.
 *
 * @returns The resolved props.
 */
export function resolveImageAttrs(
  element: t.JSXElement,
  env: Env,
  out: Out,
): ImageAttrs {
  // Find the image source prop, preferring imageName over source.
  const attrs = element.openingElement.attributes;
  const find = (attrName: string) =>
    attrs.find(
      (attr): attr is t.JSXAttribute =>
        attr.type === 'JSXAttribute' && attr.name.name === attrName,
    );
  const imAttr = find('imageName');
  const srcAttr = find('source');
  const srcExpr = imAttr
    ? attrExpr(imAttr)
    : srcAttr
      ? attrExpr(srcAttr)
      : null;
  let imageName: string | null = null; // static asset name (a literal), OR …
  let imageNameDyn: string | null = null; // … a runtime C string expr (a list-item field / state) set in app_update.

  // Resolve static sources immediately and mark imported assets for baking.
  if (srcExpr) {
    imageName = imageNameFromSource(srcExpr, env);
    if (imageName != null) {
      const path = env.imageNames?.get(imageName); // a baked import (vs. a bare {uri} name the app supplies)
      if (path) out.images.set(imageName, path);
    } else {
      // Fall back to a dynamic runtime image name when the source cannot be folded.
      // Emits it as a runtime string. The engine resolves it against the image registry by name each frame,
      // so the candidate assets must be baked. They can't be enumerated, so bake them ALL (out.bakeAllImages).
      // Only triggered when a dynamic source is actually REACHED.
      out.bakeAllImages = true;
      const cExpr = emitExpr(srcExpr, env);
      if (cExpr.cType !== 'string') {
        const err = aotError(
          'AOT: an <Image source> must resolve to an asset NAME (a string)',
          "use an imported image (`import logo from './logo.png'` → source={logo}), " +
            "a string asset name, source={{ uri: 'name' }}, or a string-valued " +
            'state / list-item field for a dynamic source.',
        );

        if (srcExpr.loc) err.aotLoc = srcExpr.loc.start;
        {
          throw err;
        }
      }

      imageNameDyn = cExpr.code;
    }
  }

  // Resolve the optional resize mode and validate it against the supported engine modes.
  let resizeMode: string | null = null;
  const rmAttr = find('resizeMode');
  if (rmAttr) {
    const rm = evalStaticOr(attrExpr(rmAttr), env, null);
    resizeMode = RESIZE_MODES[rm as string];
    if (!resizeMode) {
      const e = aotError(
        `AOT: unsupported <Image resizeMode> "${rm}"`,
        `resizeMode must be one of: ${Object.keys(RESIZE_MODES).join(' / ')}.`,
      );
      if (rmAttr.loc) e.aotLoc = rmAttr.loc.start;
      throw e;
    }
  }

  // Resolve the optional tint color into the ARGB literal expected by the engine.
  let tintColor: string | null = null;
  const tcAttr = find('tintColor');
  if (tcAttr) {
    const tc = evalStaticOr(attrExpr(tcAttr), env, null);
    if (typeof tc === 'string' || typeof tc === 'number')
      tintColor = argbLiteral(tc);
  }

  return {imageName, imageNameDyn, resizeMode, tintColor};
}
