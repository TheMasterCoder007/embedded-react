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

import {describe, it, expect} from 'vitest';
import {readdirSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, join, relative, resolve} from 'node:path';
import ts from 'typescript';

// A lint rule for the Flow A bundle: nothing under src/ may iterate a string (for...of, spread,
// array destructuring, Array.from, yield*). Firmware up to 0.21.0 never finishes such a loop on a
// 32-bit board, and no host test shows it. Walk the string by index instead.
//
// The rule goes by the checker's types, so it can't see a string held in an untyped (`any`) value,
// which is most parameters of the .js sources.

const SOURCE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const COMPILER_OPTIONS: ts.CompilerOptions = {
  target: ts.ScriptTarget.ES2020,
  module: ts.ModuleKind.ESNext,
  moduleResolution: ts.ModuleResolutionKind.Bundler,
  jsx: ts.JsxEmit.ReactJSX,
  allowJs: true,
  strict: true,
  noEmit: true,
  skipLibCheck: true,
};

function sourceFiles(directory: string): string[] {
  const files: string[] = [];
  for (const entry of readdirSync(directory, {withFileTypes: true})) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== '__tests__') files.push(...sourceFiles(path));
    } else if (
      /\.(js|jsx|ts|tsx|mts)$/.test(entry.name) &&
      !entry.name.endsWith('.d.ts')
    ) {
      files.push(path);
    }
  }
  return files;
}

function mayBeString(type: ts.Type): boolean {
  if (type.isUnionOrIntersection()) return type.types.some(mayBeString);
  return (type.flags & ts.TypeFlags.StringLike) !== 0;
}

/** The expression a node iterates, if it iterates one. */
function iteratedExpression(node: ts.Node): ts.Expression | undefined {
  if (ts.isForOfStatement(node)) return node.expression;
  if (ts.isSpreadElement(node)) return node.expression;
  if (ts.isYieldExpression(node) && node.asteriskToken) return node.expression;
  if (ts.isVariableDeclaration(node) && ts.isArrayBindingPattern(node.name))
    return node.initializer;
  if (
    ts.isBinaryExpression(node) &&
    node.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
    ts.isArrayLiteralExpression(node.left)
  ) {
    return node.right;
  }
  if (ts.isCallExpression(node) && node.expression.getText() === 'Array.from')
    return node.arguments[0];
  return undefined;
}

/** Every place the files iterate a string, as `file:line`. */
function stringIterations(
  program: ts.Program,
  files: string[],
  root: string,
): string[] {
  const checker = program.getTypeChecker();
  const found: string[] = [];
  for (const file of files) {
    const source = program.getSourceFile(file);
    if (!source) throw new Error(`not in the program: ${file}`);
    const visit = (node: ts.Node): void => {
      const expression = iteratedExpression(node);
      if (expression && mayBeString(checker.getTypeAtLocation(expression))) {
        const line =
          source.getLineAndCharacterOfPosition(expression.getStart()).line + 1;
        found.push(`${relative(root, file)}:${line}`);
      }
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  return found;
}

/** Runs the rule over one in-memory file. */
function checkSnippet(code: string): string[] {
  const name = resolve(SOURCE_ROOT, 'snippet.ts');
  const host = ts.createCompilerHost(COMPILER_OPTIONS);
  const readFile = host.getSourceFile.bind(host);
  host.getSourceFile = (fileName, languageVersion, ...rest) =>
    fileName === name
      ? ts.createSourceFile(fileName, code, languageVersion, true)
      : readFile(fileName, languageVersion, ...rest);
  return stringIterations(
    ts.createProgram([name], COMPILER_OPTIONS, host),
    [name],
    SOURCE_ROOT,
  );
}

describe('string iteration in the Flow A bundle', () => {
  it('finds each way to iterate a string', () => {
    const found = checkSnippet(
      [
        'declare const text: string;',
        'declare const maybe: string | string[];',
        'declare let first: string;',
        'for (const character of text) first = character;',
        'const spread = [...text];',
        'Math.max(...(text as any as number[]), ...("12" as string));',
        'const [head] = text;',
        '[first] = text;',
        'const copied = Array.from(text);',
        'for (const item of maybe) first = item;',
        'function* each() { yield* text; }',
      ].join('\n'),
    );
    expect(found).toEqual([
      'snippet.ts:4',
      'snippet.ts:5',
      'snippet.ts:6',
      'snippet.ts:7',
      'snippet.ts:8',
      'snippet.ts:9',
      'snippet.ts:10',
      'snippet.ts:11',
    ]);
  });

  it('allows arrays and index walks', () => {
    const found = checkSnippet(
      [
        'declare const text: string;',
        'declare const words: string[];',
        'let total = 0;',
        'for (const word of words) total += word.length;',
        'for (let index = 0; index < text.length; index++) total += text.charCodeAt(index);',
        'const copied = [...words, ...text.split(",")];',
        'const [head] = words;',
        'const listed = Array.from(words);',
      ].join('\n'),
    );
    expect(found).toEqual([]);
  });

  it('finds none under src/', () => {
    const files = sourceFiles(SOURCE_ROOT);
    const program = ts.createProgram(files, COMPILER_OPTIONS);
    expect(stringIterations(program, files, SOURCE_ROOT)).toEqual([]);
  }, 60_000);
});
