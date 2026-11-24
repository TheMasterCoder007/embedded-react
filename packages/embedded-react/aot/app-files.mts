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
 * Reads a Flow B app from disk: its entry, the files of its own the entry imports, and what each of them needs baked
 * or read ahead of the compile, which does no I/O itself.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {existsSync, readFileSync, statSync} from 'node:fs';
import {dirname, relative, resolve} from 'node:path';
import {parse} from '@babel/parser';
import {bakeSvgArtifacts} from './compiler/collect.mts';
import {
  SOURCE_SUFFIXES,
  bringsValues,
  declaresModules,
  normalizePath,
  pluginsFor,
} from './compiler/link.mts';
import {readNativeFiles} from './compiler/native-files.mts';
import {bakeSvgTextPaths} from './svg-text-elements.mts';
import type {NativeFile} from './compiler/native-files.mts';
import type {CompileOptions} from './compiler/options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An app read from disk, ready to compile. */
export interface AppFiles {
  /** The entry's source, each <SvgText> baked. */
  source: string;
  /** The text of every file, for what reads the whole app: the font sizes it names, the glyphs it shows. */
  text: string;
  /** Every source file's path, the entry first, for a watch. */
  paths: string[];
  /** What compilePackage needs of the files. */
  options: Required<
    Pick<
      CompileOptions,
      'filename' | 'sourceFiles' | 'svgArtifacts' | 'nativeFiles'
    >
  >;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a path is one of the app's own source files.
 *
 * @param path  The path.
 *
 * @returns True for a .ts, .tsx, .js or .jsx file that isn't a .d.ts.
 */
const isAppSource = (path: string): boolean =>
  /\.(tsx?|jsx?)$/.test(path) &&
  !/\.d\.ts$/.test(path) &&
  existsSync(path) &&
  statSync(path).isFile();

/**
 * The relative paths a file imports values from, or passes on.
 *
 * @param source  The file's source.
 * @param path  The file.
 *
 * @returns The paths as written; none for a file that doesn't parse, which the compile reports.
 */
function relativeImports(source: string, path: string): string[] {
  let program;
  try {
    program = parse(source, {
      sourceType: 'module',
      plugins: pluginsFor(path),
    }).program;
  } catch {
    return [];
  }
  return program.body.flatMap(statement =>
    (statement.type === 'ImportDeclaration' ||
      statement.type === 'ExportNamedDeclaration' ||
      statement.type === 'ExportAllDeclaration') &&
    statement.source?.value.startsWith('.') &&
    bringsValues(statement)
      ? [statement.source.value]
      : [],
  );
}

/**
 * Reads an app: its entry and, from there, every file of its own it imports by a relative path.
 *
 * @param appPath  The entry, App.tsx or App.jsx.
 *
 * @returns The app's sources, and compilePackage's options for them.
 */
export async function readApp(appPath: string): Promise<AppFiles> {
  const appDirectory = dirname(appPath);
  const source = bakeSvgTextPaths(readFileSync(appPath, 'utf8'), appDirectory);
  const sourceFiles: Record<string, string> = {};
  const nativeFiles: Record<string, NativeFile> = {};
  const svgArtifacts = await bakeSvgArtifacts(source, appDirectory);
  const paths = [appPath];

  // A path from the entry's folder, as the entry would import it.
  const fromEntry = (path: string): string => {
    const shown = normalizePath(
      relative(appDirectory, path).replace(/\\/g, '/'),
    );
    return shown.startsWith('..') ? shown : `./${shown}`;
  };

  const pending = [{path: appPath, source}];
  for (let file = pending.shift(); file; file = pending.shift()) {
    const folder = dirname(file.path);
    const natives =
      file.path === appPath
        ? readNativeFiles(file.source, folder)
        : readNativeFiles(file.source, folder, file.path);
    for (const [importPath, native] of Object.entries(natives)) {
      nativeFiles[
        importPath.startsWith('.')
          ? fromEntry(resolve(folder, importPath))
          : importPath
      ] ??= native;
    }

    for (const importPath of relativeImports(file.source, file.path)) {
      const base = resolve(folder, importPath);
      const found = SOURCE_SUFFIXES.map(suffix => base + suffix).find(
        isAppSource,
      );
      if (!found || found === appPath) continue;
      const native = nativeFiles[fromEntry(base)];
      if (native && declaresModules(native)) continue;
      const key = fromEntry(found).replace(/^\.\//, '');
      if (Object.hasOwn(sourceFiles, key)) continue;

      const text = readFileSync(found, 'utf8');
      const imported = /\bSvgText\b/.test(text)
        ? bakeSvgTextPaths(text, dirname(found))
        : text;
      sourceFiles[key] = imported;
      paths.push(found);
      // A file that declares native modules is never merged, however it is reached: the compiler only needs its
      // source to say so.
      if (/\bnativeModule\b/.test(imported)) continue;
      if (/\.svg['"]/i.test(imported)) {
        Object.assign(
          svgArtifacts,
          await bakeSvgArtifacts(imported, dirname(found)),
        );
      }
      pending.push({path: found, source: imported});
    }
  }

  return {
    source,
    text: [source, ...Object.values(sourceFiles)].join('\n'),
    paths,
    options: {filename: appPath, sourceFiles, svgArtifacts, nativeFiles},
  };
}
