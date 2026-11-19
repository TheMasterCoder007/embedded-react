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

/*
 * The fonts an app draws its text in, and the sizes each is drawn at. The engine has no rasterizer, so the build
 * bakes each imported font at exactly these sizes.
 */

import type {IrNode} from './ir/nodes.mts';
import type {Out} from './out.mts';
import type {FontUse} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The size the engine draws a <TextInput> at when its style names none. */
const INPUT_FONT_SIZE = 16;

/** The range the engine clamps a font size into before it looks a font up. */
const MIN_FONT_SIZE = 8;
const MAX_FONT_SIZE = 96;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Records the sizes a <Text> or a <TextInput> is drawn at, under its font family.
 *
 * @param node  The text's node.
 * @param output  Everything emitted so far.
 */
export function recordFontUse(node: IrNode, output: Out): void {
  const sizes: number[] = [];
  const add = (size: unknown): void => {
    if (typeof size === 'number' && size > 0) {
      sizes.push(Math.min(MAX_FONT_SIZE, Math.max(MIN_FONT_SIZE, size)));
    }
  };

  // A size state drives can't be listed here. A <Text> that names no size asks the engine for the smallest there
  // is: an imported font is baked at it, and the built-in font, which has its own sizes, adds none.
  const open = node.stateProps.some(prop => prop.field === 'font_size');
  if (!open) {
    const unsized =
      node.type === 'ER_NODE_TEXT_INPUT'
        ? INPUT_FONT_SIZE
        : node.fontFamily
          ? MIN_FONT_SIZE
          : 0;
    add(
      node.constantProps.find(prop => prop.field === 'font_size')?.value ||
        unsized,
    );
  }
  // A span with no size of its own takes its text's.
  node.spans?.forEach(span => add(span.font_size));

  // A family state picks could be any imported one.
  const use =
    node.fontFamily?.kind === 'value'
      ? output.anyFontSizes
      : fontSizesOf(output, node.fontFamily?.name ?? '');
  sizes.forEach(size => use.sizes.add(size));
  use.open ||= open;
}

/**
 * The fonts an app's text is drawn in, each with its sizes: the built-in font, the imported fonts it reaches, and
 * any family it names without importing.
 *
 * @param output  Everything the app emitted.
 * @param fontFamilies  The imported fonts: family name → source-relative path.
 *
 * @returns The fonts.
 */
export function fontUses(
  output: Out,
  fontFamilies: Map<string, string>,
): FontUse[] {
  const {anyFontSizes} = output;
  if (anyFontSizes.sizes.size > 0 || anyFontSizes.open) {
    for (const family of fontFamilies.keys()) {
      const use = fontSizesOf(output, family);
      anyFontSizes.sizes.forEach(size => use.sizes.add(size));
      use.open ||= anyFontSizes.open;
    }
  }

  return [...output.fontSizes].map(([family, use]) => ({
    family,
    importPath: fontFamilies.get(family) ?? null,
    sizes: [...use.sizes].sort((left, right) => left - right),
    openSizes: use.open,
  }));
}

/**
 * The sizes recorded for a family, made on first use.
 *
 * @param output  Everything emitted so far.
 * @param family  The family's name; '' is the built-in font.
 *
 * @returns The family's sizes.
 */
function fontSizesOf(
  output: Out,
  family: string,
): {sizes: Set<number>; open: boolean} {
  let use = output.fontSizes.get(family);
  if (!use) {
    use = {sizes: new Set(), open: false};
    output.fontSizes.set(family, use);
  }

  return use;
}
