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
 * Bakes each <SvgText> in an app's source to glyph outlines, before either flow compiles it. The element is
 * rewritten in place to one that carries a path `d`, so the device needs no font for it. Its text and what places
 * it must be constants; its paint is left as written.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {join, resolve} from 'node:path';
import {parse} from '@babel/parser';
import type * as t from '@babel/types';
import {parseTransform, transformOps} from '../assets/bake-svg.mts';
import {
  DEFAULT_FONT_SIZE,
  anchorOffset,
  baselineShift,
  opsToPathData,
  parseFontFamilies,
  parseFontWeight,
  parseLetterSpacing,
  parseTextAnchor,
  resolveFont,
  runAdvance,
  runOps,
} from '../assets/svg-text.mts';
import type {FontSearch} from '../assets/svg-text.mts';
import {aotError} from './compiler/diagnostics.mts';
import type {AotError} from './compiler/diagnostics.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

const TAG = 'SvgText';

/** The attributes that shape the outlines; every other one stays on the element. */
const TEXT_ATTRIBUTES = new Set([
  'x',
  'y',
  'fontFamily',
  'fontSize',
  'fontWeight',
  'letterSpacing',
  'textAnchor',
  'dominantBaseline',
  'transform',
]);

const DYNAMIC_HINT =
  'an <SvgText> is drawn at build time. Text that changes at run time goes in a <Text> placed over the <Svg>.';

type Constant = string | number;

/** A font import: `import Inter from './fonts/Inter.ttf'`. */
interface FontImport {
  family: string;
  path: string;
  statement: t.ImportDeclaration;
  /** How many <SvgText fontFamily> uses were baked away. */
  baked: number;
}

/** A replacement of source[start, end) by `text`. */
interface Splice {
  start: number;
  end: number;
  text: string;
}

function located(error: AotError, node: t.Node): AotError {
  if (node.loc) error.aotLoc = node.loc.start;
  return error;
}

/** Calls @p visit on every node under @p node. */
function walk(node: unknown, visit: (node: t.Node) => void): void {
  if (Array.isArray(node)) {
    for (const item of node) walk(item, visit);
    return;
  }
  if (!node || typeof (node as t.Node).type !== 'string') return;
  visit(node as t.Node);
  for (const [key, value] of Object.entries(node)) {
    if (key !== 'loc' && value && typeof value === 'object') walk(value, visit);
  }
}

/**
 * Bakes every <SvgText> in a source file to outlines.
 *
 * @param source  The file's text.
 * @param directory  The file's folder: font imports resolve against it, and a family with no import is looked for
 *                   in it and in its `fonts` folder.
 * @param tag  The tag the baked element takes.
 * @param keepFontImports  Leaves every font import in place.
 *
 * @returns The source with each <SvgText> replaced, on the same lines. A font imported only for them loses its
 *          import, so Flow A does not also bake it as a bitmap font.
 */
export function bakeSvgTextElements(
  source: string,
  directory: string,
  tag: string = TAG,
  keepFontImports = false,
): string {
  if (!source.includes(TAG)) return source;

  const program = parse(source, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;

  // Module-level constants and font imports an attribute may name.
  const constants = new Map<string, Constant>();
  const fontImports = new Map<string, FontImport>();
  for (const statement of program.body) {
    if (
      statement.type === 'ImportDeclaration' &&
      /\.(ttf|otf)$/i.test(statement.source.value)
    ) {
      const importPath = statement.source.value;
      for (const specifier of statement.specifiers) {
        if (specifier.type === 'ImportDefaultSpecifier') {
          fontImports.set(specifier.local.name, {
            family: importPath
              .split(/[\\/]/)
              .pop()!
              .replace(/\.[^.]+$/, ''),
            path: resolve(directory, importPath),
            statement,
            baked: 0,
          });
        }
      }
    } else if (
      statement.type === 'VariableDeclaration' &&
      statement.kind === 'const'
    ) {
      for (const declarator of statement.declarations) {
        const value = declarator.init && literalOf(declarator.init);
        if (declarator.id.type === 'Identifier' && value != null)
          constants.set(declarator.id.name, value);
      }
    }
  }

  const search: FontSearch = {
    imports: new Map(
      [...fontImports.values()].map(font => [font.family, font.path]),
    ),
    directories: [directory, join(directory, 'fonts')],
  };

  /** A literal's value, or null for anything else. */
  function literalOf(expression: t.Node): Constant | null {
    if (
      expression.type === 'StringLiteral' ||
      expression.type === 'NumericLiteral'
    )
      return expression.value;
    if (
      expression.type === 'UnaryExpression' &&
      expression.operator === '-' &&
      expression.argument.type === 'NumericLiteral'
    )
      return -expression.argument.value;
    if (
      expression.type === 'TemplateLiteral' &&
      expression.expressions.length === 0
    )
      return expression.quasis[0].value.cooked ?? '';
    return null;
  }

  /** A constant expression's value: a literal, a module-level constant, or an imported font's family. */
  function constantOf(expression: t.Node, what: string): Constant {
    const literal = literalOf(expression);
    if (literal != null) return literal;
    if (expression.type === 'Identifier') {
      const font = fontImports.get(expression.name);
      if (font) {
        font.baked++;
        return font.family;
      }
      const constant = constants.get(expression.name);
      if (constant != null) return constant;
    }
    throw located(
      aotError(`AOT: <SvgText> ${what} must be a constant`, DYNAMIC_HINT),
      expression,
    );
  }

  function bake(element: t.JSXElement): Splice {
    const text: Record<string, Constant> = {};
    const kept: string[] = [];
    for (const attribute of element.openingElement.attributes) {
      if (attribute.type !== 'JSXAttribute') {
        throw located(
          aotError('AOT: spread attributes on an <SvgText> not supported'),
          attribute,
        );
      }
      const name = String(attribute.name.name);
      if (!TEXT_ATTRIBUTES.has(name)) {
        kept.push(source.slice(attribute.start!, attribute.end!));
        continue;
      }
      const value = attribute.value;
      if (value?.type === 'StringLiteral') text[name] = value.value;
      else if (value?.type === 'JSXExpressionContainer')
        text[name] = constantOf(value.expression, `"${name}"`);
      else
        throw located(
          aotError(`AOT: <SvgText> "${name}" needs a value`),
          attribute,
        );
    }

    let content = '';
    for (const child of element.children) {
      if (child.type === 'JSXText') content += child.value;
      else if (child.type === 'JSXExpressionContainer') {
        if (child.expression.type !== 'JSXEmptyExpression')
          content += constantOf(child.expression, 'text');
      } else {
        throw located(
          aotError('AOT: an <SvgText> holds text only', DYNAMIC_HINT),
          child,
        );
      }
    }
    content = content.replace(/\s+/g, ' ').trim();

    let d: string;
    try {
      const fontSize = Number(text.fontSize ?? DEFAULT_FONT_SIZE);
      const font = resolveFont(
        parseFontFamilies(text.fontFamily),
        parseFontWeight(text.fontWeight),
        search,
      );
      const run = {
        text: content,
        font,
        fontSize,
        letterSpacing: parseLetterSpacing(text.letterSpacing, fontSize),
      };
      const x =
        Number(text.x ?? 0) +
        anchorOffset(parseTextAnchor(text.textAnchor), runAdvance(run));
      const y =
        Number(text.y ?? 0) +
        baselineShift(font, fontSize, text.dominantBaseline);
      const ops = runOps(run, x, y);
      d = opsToPathData(
        text.transform == null
          ? ops
          : transformOps(ops, parseTransform(String(text.transform))),
      );
    } catch (error) {
      throw located(
        aotError(`AOT: <SvgText>: ${(error as Error).message}`),
        element,
      );
    }

    // The element keeps its line count, so later errors still point at the right line.
    const original = source.slice(element.start!, element.end!);
    const lines = '\n'.repeat(original.split('\n').length - 1);
    const attributes = [`d="${d}"`, ...kept].join(' ');
    return {
      start: element.start!,
      end: element.end!,
      text: `<${tag} ${attributes}${lines}/>`,
    };
  }

  const splices: Splice[] = [];
  const uses = new Map<string, number>();
  walk(program, node => {
    if (
      node.type === 'JSXElement' &&
      node.openingElement.name.type === 'JSXIdentifier' &&
      node.openingElement.name.name === TAG
    )
      splices.push(bake(node));
    if (node.type === 'Identifier' && fontImports.has(node.name))
      uses.set(node.name, (uses.get(node.name) ?? 0) + 1);
  });

  // A font every use of which was baked away is no longer imported. Its import specifier is one of the uses.
  for (const [local, font] of fontImports) {
    if (keepFontImports) break;
    if (font.baked > 0 && uses.get(local) === font.baked + 1) {
      const {start, end} = font.statement;
      const lines = source.slice(start!, end!).split('\n').length - 1;
      splices.push({start: start!, end: end!, text: '\n'.repeat(lines)});
    }
  }

  let baked = source;
  for (const splice of splices.sort(
    (first, second) => second.start - first.start,
  ))
    baked =
      baked.slice(0, splice.start) + splice.text + baked.slice(splice.end);
  return baked;
}

/**
 * Bakes every <SvgText> in a Flow B app's source to a <Path>, which the compiler draws as any other. Font imports
 * stay: the compiler bakes no bitmap for a font no <Text> uses, and an .svg's text may still need them.
 *
 * @param source  The app's text.
 * @param directory  The app's folder.
 *
 * @returns The source with each <SvgText> replaced.
 */
export const bakeSvgTextPaths = (source: string, directory: string): string =>
  bakeSvgTextElements(source, directory, 'Path', true);
