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
 * Flow A's esbuild loader for an app's own files. It gives each nativeModule() call its methods' signatures, which
 * the bundle's erased types can't, and in the simulators its mock, then runs the bundler's own transform when it has
 * one. esbuild runs one onLoad per file, so a bundler that transforms the app's files calls this from its own, and
 * loadDependencyFile for every other file, since a package may declare native modules too.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {existsSync, readFileSync} from 'node:fs';
import {dirname, relative} from 'node:path';
import type {OnLoadResult} from 'esbuild';
import type {AotError} from './compiler/diagnostics.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * esbuild's loader for one of the app's own files.
 *
 * @param path  The file.
 * @param transform  The bundler's own transform, if it has one.
 * @param options  `mocks` for the simulators: each native module runs its mock (`sensors.mock.ts` beside its
 *                 interface) instead of the firmware's.
 *
 * @returns What esbuild loads, or undefined to let esbuild read the file itself.
 */
export async function loadAppFile(
  path: string,
  transform?: (source: string) => string,
  options: {mocks?: boolean} = {},
): Promise<OnLoadResult | undefined> {
  const source = readFileSync(path, 'utf8');
  let contents = source;
  // A mock added later rebuilds the app.
  const watchFiles: string[] = [];
  if (/\bnativeModule\b/.test(source)) {
    const {addNativeSignatures} = await import('./compiler/native-modules.mts');
    const {nativeMockFiles, readNativeFiles} =
      await import('./compiler/native-files.mts');
    try {
      let mocks: Map<string, string | null> | undefined;
      if (options.mocks) {
        mocks = new Map();
        for (const [name, candidates] of nativeMockFiles(
          source,
          dirname(path),
        )) {
          watchFiles.push(...candidates);
          const found = candidates.find(candidate => existsSync(candidate));
          mocks.set(
            name,
            found
              ? './' + relative(dirname(path), found).replace(/\\/g, '/')
              : null,
          );
        }
      }
      contents = addNativeSignatures(
        source,
        readNativeFiles(source, dirname(path)),
        mocks,
      );
    } catch (error) {
      const aotError = error as AotError & {code?: string};
      // A syntax error is esbuild's to report.
      if (aotError.code !== 'BABEL_PARSER_SYNTAX_ERROR') {
        if (!aotError.message?.startsWith('AOT:')) throw error;
        return {
          errors: [
            {
              text: aotError.message.replace(/^AOT: /, ''),
              location: aotError.aotLoc
                ? {
                    file: path,
                    line: aotError.aotLoc.line,
                    column: aotError.aotLoc.column,
                  }
                : null,
              notes: aotError.aotHint ? [{text: aotError.aotHint}] : [],
            },
          ],
        };
      }
    }
  }
  if (transform) {
    contents = transform(contents);
  } else if (contents === source) {
    return undefined;
  }
  return {
    contents,
    loader: /\.[cm]?tsx$/.test(path)
      ? 'tsx'
      : /\.[cm]?ts$/.test(path)
        ? 'ts'
        : 'jsx',
    ...(watchFiles.length ? {watchFiles} : {}),
  };
}

/**
 * esbuild's loader for a file that isn't the app's own: a package's .ts file that declares native modules gets their
 * signatures, and in the simulators their mocks, as the app's own files do.
 *
 * @param path  The file.
 * @param options  `mocks` for the simulators (see loadAppFile).
 *
 * @returns What esbuild loads, or undefined to let esbuild read the file itself.
 */
export async function loadDependencyFile(
  path: string,
  options: {mocks?: boolean} = {},
): Promise<OnLoadResult | undefined> {
  if (!/\.[cm]?tsx?$/.test(path) || /\.d\.[cm]?ts$/.test(path)) {
    return undefined;
  }
  return loadAppFile(path, undefined, options);
}
