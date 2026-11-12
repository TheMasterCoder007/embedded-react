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
 * The files an app's native modules come from, read ahead so the compile itself stays pure: the local files it
 * imports a module's interface from, and the files and packages it imports declared modules from, resolved the Node
 * way. A module is read from its .ts source, so a package that ships only JavaScript or .d.ts is kept as a file with
 * no source, for the compiler to refuse.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {existsSync, readFileSync, realpathSync, statSync} from 'node:fs';
import {dirname, isAbsolute, join, relative, resolve} from 'node:path';
import {parse} from '@babel/parser';
import type * as t from '@babel/types';
import {
  declarationsOf,
  declaredMembers,
  localImportOf,
} from './native-modules.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A file the app's native modules are declared in, or their interfaces. */
export interface NativeFile {
  /** Where it is from the app's folder, for errors. */
  path: string;
  /** Its source, or null when it is JavaScript or a .d.ts, which a native module isn't read from. */
  source: string | null;
  /** The local files its modules' interfaces are in, by the path it imports each by. */
  imports: Record<string, NativeFile>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a local .ts file may be called, after the path it is imported by. */
const SOURCE_EXTENSIONS = ['.ts', '.mts', '.tsx', '/index.ts'];

/** What a JavaScript or .d.ts file may be called, after the path it is imported by. */
const BUILT_EXTENSIONS = ['.js', '.mjs', '.cjs', '.jsx', '.d.ts', '/index.js'];

/** What a module's mock may be called, after `<name>.mock`. */
const MOCK_EXTENSIONS = ['.ts', '.tsx', '.js', '.jsx'];

/** The conditions a package's `exports` is read with, as Node imports it, and as bundlers do. */
const CONDITIONS = new Set(['import', 'module', 'node', 'default']);

/** A JavaScript file that declares a native module. */
const DECLARES_MODULE = /\bnativeModule(?:\.optional)?\s*\(\s*['"`]/;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Whether a file is .ts source, not a .d.ts.
 *
 * @param path  The file.
 *
 * @returns True when it is.
 */
const isSourceFile = (path: string): boolean =>
  /\.[cm]?tsx?$/.test(path) && !/\.d\.[cm]?ts$/.test(path);

/**
 * Whether a file is JavaScript, JSX included.
 *
 * @param path  The file.
 *
 * @returns True when it is.
 */
const isBuiltFile = (path: string): boolean => /\.[cm]?jsx?$/.test(path);

/**
 * Whether a path is a file.
 *
 * @param path  The path.
 *
 * @returns True when it is.
 */
const isFile = (path: string): boolean =>
  existsSync(path) && statSync(path).isFile();

/**
 * Parses a file, types and all.
 *
 * @param source  Its source.
 *
 * @returns Its program.
 */
const parseFile = (source: string): t.Program =>
  parse(source, {sourceType: 'module', plugins: ['jsx', 'typescript']}).program;

/**
 * The .ts file a path names: `./sensors`, `./sensors.ts`, or `./sensors.js` as NodeNext writes it.
 *
 * @param base  The path, resolved.
 *
 * @returns The file, or undefined when there is none.
 */
function sourceFileOf(base: string): string | undefined {
  const candidates = [
    base,
    base.replace(/\.(m?)js$/, '.$1ts'),
    ...SOURCE_EXTENSIONS.map(extension => base + extension),
  ];
  return candidates.find(path => isSourceFile(path) && isFile(path));
}

/**
 * The file a path names, .ts source first, then JavaScript or a .d.ts.
 *
 * @param base  The path, resolved.
 *
 * @returns The file, or undefined when there is none.
 */
const fileOf = (base: string): string | undefined =>
  sourceFileOf(base) ??
  [base, ...BUILT_EXTENSIONS.map(extension => base + extension)].find(isFile);

/**
 * The path a package's `exports` gives for a condition object, array or path.
 *
 * @param target  The export's value.
 *
 * @returns The path, or null when no condition matches.
 */
function exportTarget(target: unknown): string | null {
  if (typeof target === 'string') return target;
  if (Array.isArray(target)) {
    for (const entry of target) {
      const path = exportTarget(entry);
      if (path) return path;
    }
    return null;
  }
  if (target && typeof target === 'object') {
    for (const [condition, value] of Object.entries(target)) {
      if (!CONDITIONS.has(condition)) continue;
      const path = exportTarget(value);
      if (path) return path;
    }
  }
  return null;
}

/**
 * The path a package's `exports` gives a subpath: an exact key, or one with a `*`.
 *
 * @param exports  The package's `exports`.
 * @param subpath  The subpath, `.` for the package itself.
 *
 * @returns The path in the package, or null when it isn't exported.
 */
function exportedPath(exports: unknown, subpath: string): string | null {
  const isSubpathMap =
    !!exports &&
    typeof exports === 'object' &&
    !Array.isArray(exports) &&
    Object.keys(exports).some(key => key.startsWith('.'));
  if (!isSubpathMap) {
    return subpath === '.' ? packageTarget(exportTarget(exports)) : null;
  }
  const map = exports as Record<string, unknown>;
  if (Object.hasOwn(map, subpath)) {
    return packageTarget(exportTarget(map[subpath]));
  }
  // As Node does, the pattern with the longest part before its `*` wins, then the longest pattern.
  let best: {key: string; prefix: string; suffix: string} | null = null;
  for (const key of Object.keys(map)) {
    const star = key.indexOf('*');
    if (star < 0 || key.indexOf('*', star + 1) >= 0) continue;
    const prefix = key.slice(0, star);
    const suffix = key.slice(star + 1);
    const matches =
      subpath.length >= key.length - 1 &&
      subpath.startsWith(prefix) &&
      subpath.endsWith(suffix);
    if (
      matches &&
      (!best ||
        prefix.length > best.prefix.length ||
        (prefix.length === best.prefix.length && key.length > best.key.length))
    ) {
      best = {key, prefix, suffix};
    }
  }
  if (!best) return null;
  const match = subpath.slice(
    best.prefix.length,
    subpath.length - best.suffix.length,
  );
  return packageTarget(
    exportTarget(map[best.key])?.replaceAll('*', match) ?? null,
  );
}

/**
 * An export's path, when it stays in its package as Node requires: `./` then no empty, `.`, `..` or node_modules part.
 *
 * @param path  The path the export gives.
 *
 * @returns The path, or null when Node would refuse it.
 */
function packageTarget(path: string | null): string | null {
  if (!path?.startsWith('./')) return null;
  const parts = path.slice(2).split(/[\\/]/);
  return parts.some(
    part =>
      part === '' ||
      part === '.' ||
      part === '..' ||
      part.toLowerCase() === 'node_modules',
  )
    ? null
    : path;
}

/**
 * The file a package import names: the package is found in a node_modules folder up from the importer's, and the
 * file by its `exports`, or else its `main` or the path itself.
 *
 * @param fromDirectory  The importer's folder.
 * @param specifier  The import's path: `my-device`, `@acme/device` or `my-device/can`.
 *
 * @returns The file, or undefined when there is none.
 */
function packageFileOf(
  fromDirectory: string,
  specifier: string,
): string | undefined {
  const parts = specifier.split('/');
  const nameLength = specifier.startsWith('@') ? 2 : 1;
  const name = parts.slice(0, nameLength).join('/');
  const subpath = ['.', ...parts.slice(nameLength)].join('/');
  for (let directory = fromDirectory; ; directory = dirname(directory)) {
    const manifestPath = join(directory, 'node_modules', name, 'package.json');
    if (isFile(manifestPath)) {
      // A workspace or file: package is a link to its own folder.
      return packageEntryOf(realpathSync(dirname(manifestPath)), subpath);
    }
    if (dirname(directory) === directory) return undefined;
  }
}

/**
 * The file a package gives a subpath, by its `exports`, or else its `main` or the path itself.
 *
 * @param packageDirectory  The package's folder.
 * @param subpath  The subpath, `.` for the package itself.
 *
 * @returns The file, or undefined when there is none.
 */
function packageEntryOf(
  packageDirectory: string,
  subpath: string,
): string | undefined {
  const manifest = JSON.parse(
    readFileSync(join(packageDirectory, 'package.json'), 'utf8'),
  );
  if (manifest.exports !== undefined) {
    const path = exportedPath(manifest.exports, subpath);
    const file = path && resolve(packageDirectory, path);
    return file && isFile(file) ? file : undefined;
  }
  return fileOf(
    resolve(
      packageDirectory,
      subpath === '.' ? (manifest.main ?? 'index') : subpath,
    ),
  );
}

/**
 * The file an import names, resolved the Node way: a relative or absolute path, or a package.
 *
 * @param fromDirectory  The importer's folder.
 * @param specifier  The import's path.
 *
 * @returns The file, or undefined when there is none.
 */
const importedFileOf = (
  fromDirectory: string,
  specifier: string,
): string | undefined =>
  specifier.startsWith('.') || isAbsolute(specifier)
    ? fileOf(resolve(fromDirectory, specifier))
    : packageFileOf(fromDirectory, specifier);

/**
 * The local .ts file an interface is imported from.
 *
 * @param fromDirectory  The importer's folder.
 * @param importPath  The path it is imported by.
 *
 * @returns The file, or undefined when there is none.
 */
export const interfaceFileOf = (
  fromDirectory: string,
  importPath: string,
): string | undefined => sourceFileOf(resolve(fromDirectory, importPath));

/**
 * The local files a file's module declarations import their interfaces from, by the path each is imported by.
 *
 * @param program  The file.
 * @param directory  Its folder.
 *
 * @returns The interfaces' files, each found by its import path.
 */
function interfaceFilesOf(
  program: t.Program,
  directory: string,
): Map<string, string> {
  const found = new Map<string, string>();
  for (const declarator of declarationsOf(program)) {
    const typeArgument = (declarator.init as t.CallExpression).typeParameters
      ?.params[0];
    if (
      typeArgument?.type !== 'TSTypeReference' ||
      typeArgument.typeName.type !== 'Identifier' ||
      declaredMembers(program, typeArgument.typeName.name) !== null
    ) {
      continue;
    }
    const local = localImportOf(program, typeArgument.typeName.name);
    if (!local || found.has(local.path)) continue;
    const file = interfaceFileOf(directory, local.path);
    if (file) {
      found.set(local.path, file);
    }
  }
  return found;
}

/**
 * Reads a .ts file, with the interface files its module declarations import.
 *
 * @param path  The file.
 * @param appDirectory  The app's folder, which a file's path is shown from.
 *
 * @returns The file.
 */
function readSourceFile(path: string, appDirectory: string): NativeFile {
  const source = readFileSync(path, 'utf8');
  const imports: Record<string, NativeFile> = {};
  if (/\bnativeModule\b/.test(source)) {
    for (const [importPath, file] of interfaceFilesOf(
      parseFile(source),
      dirname(path),
    )) {
      imports[importPath] = {
        path: shownPath(appDirectory, file),
        source: readFileSync(file, 'utf8'),
        imports: {},
      };
    }
  }
  return {path: shownPath(appDirectory, path), source, imports};
}

/**
 * A file's path as an error shows it, from the app's folder.
 *
 * @param appDirectory  The app's folder.
 * @param path  The file.
 *
 * @returns The path.
 */
const shownPath = (appDirectory: string, path: string): string =>
  relative(realpathSync(appDirectory), realpathSync(path)).replace(/\\/g, '/');

/**
 * Whether a file passes on what another file declares, `export * from` or `export {Can} from`, and one of those files
 * declares native modules, or passes them on in turn.
 *
 * @param source  The file's source.
 * @param path  The file.
 * @param seen  The files already looked at, so a loop ends.
 *
 * @returns True when it does.
 */
function passesOnModules(
  source: string,
  path: string,
  seen: Set<string> = new Set([path]),
): boolean {
  if (!/\bexport\b[^;]*\bfrom\b/.test(source)) return false;
  return parseFile(source).body.some(statement => {
    if (
      (statement.type !== 'ExportAllDeclaration' &&
        statement.type !== 'ExportNamedDeclaration') ||
      !statement.source ||
      statement.exportKind === 'type'
    ) {
      return false;
    }
    const file = importedFileOf(dirname(path), statement.source.value);
    if (!file || seen.has(file)) return false;
    seen.add(file);
    const passed = readFileSync(file, 'utf8');
    if (!isSourceFile(file)) {
      return isBuiltFile(file) && DECLARES_MODULE.test(passed);
    }
    return (
      /\bnativeModule\b/.test(passed) || passesOnModules(passed, file, seen)
    );
  });
}

/**
 * Reads the files an app's native modules come from: the local files it imports a module's interface from, and the
 * files and packages it imports modules from.
 *
 * @param source  The app's source.
 * @param appDirectory  The app's folder, which its imports are resolved from.
 *
 * @returns Each file, by the path the app imports it by.
 */
export function readNativeFiles(
  source: string,
  appDirectory: string,
): Record<string, NativeFile> {
  const program = parseFile(source);
  const files: Record<string, NativeFile> = {};
  for (const [importPath, file] of interfaceFilesOf(program, appDirectory)) {
    files[importPath] = readSourceFile(file, appDirectory);
  }

  // The modules it imports: a file that may declare one, or a built one that does.
  for (const statement of program.body) {
    if (
      statement.type !== 'ImportDeclaration' ||
      statement.importKind === 'type' ||
      statement.source.value === 'embedded-react' ||
      statement.source.value in files ||
      !statement.specifiers.some(
        specifier =>
          specifier.type === 'ImportSpecifier' &&
          specifier.importKind !== 'type',
      )
    ) {
      continue;
    }
    const importPath = statement.source.value;
    const file = importedFileOf(appDirectory, importPath);
    if (!file) continue;
    if (isSourceFile(file)) {
      const source = readFileSync(file, 'utf8');
      // A file that passes modules on is read too, for the compiler to refuse.
      if (/\bnativeModule\b/.test(source) || passesOnModules(source, file)) {
        files[importPath] = readSourceFile(file, appDirectory);
      }
    } else if (
      isBuiltFile(file) &&
      DECLARES_MODULE.test(readFileSync(file, 'utf8'))
    ) {
      files[importPath] = {
        path: shownPath(appDirectory, file),
        source: null,
        imports: {},
      };
    }
  }
  return files;
}

/**
 * Reads the file a C header is generated from, with the files its modules' interfaces are in.
 *
 * @param target  A file, a package's folder, or a package's name as an app imports it.
 * @param fromDirectory  The folder a path is from, and a package is looked for from.
 *
 * @returns The file, its source, and its interface files.
 */
export function readModuleFile(
  target: string,
  fromDirectory: string,
): {path: string; source: string; files: Record<string, NativeFile>} {
  const path = resolve(fromDirectory, target);
  const file = isFile(join(path, 'package.json'))
    ? packageEntryOf(path, '.')
    : (fileOf(path) ?? packageFileOf(fromDirectory, target));
  if (!file) {
    throw new Error(
      `No file or package at ${target}. Pass the .ts file that declares the modules, or the package's folder or name.`,
    );
  }
  if (!isSourceFile(file)) {
    throw new Error(
      `${file} is JavaScript or a .d.ts, which a native module isn't read from. ` +
        'Ship the package\'s .ts source, and point its exports at it: "exports": "./index.ts".',
    );
  }
  const source = readFileSync(file, 'utf8');
  return {path: file, source, files: readNativeFiles(source, dirname(file))};
}

/**
 * Where each module's mock for the simulators may be: `<name>.mock.ts` (or .tsx, .js, .jsx) beside its interface.
 *
 * @param source  A file's source.
 * @param baseDirectory  The file's folder.
 *
 * @returns The paths to look for, in order, by module name.
 */
export function nativeMockFiles(
  source: string,
  baseDirectory: string,
): Map<string, string[]> {
  const program = parseFile(source);
  const interfaceFiles = interfaceFilesOf(program, baseDirectory);
  const mocks = new Map<string, string[]>();
  for (const declarator of declarationsOf(program)) {
    const call = declarator.init as t.CallExpression;
    const [name] = call.arguments;
    if (name?.type !== 'StringLiteral') continue;
    const typeArgument = call.typeParameters?.params[0];
    const local =
      typeArgument?.type === 'TSTypeReference' &&
      typeArgument.typeName.type === 'Identifier'
        ? localImportOf(program, typeArgument.typeName.name)
        : null;
    const interfaceFile = local ? interfaceFiles.get(local.path) : undefined;
    const directory = interfaceFile ? dirname(interfaceFile) : baseDirectory;
    mocks.set(
      name.value,
      MOCK_EXTENSIONS.map(extension =>
        resolve(directory, `${name.value}.mock${extension}`),
      ),
    );
  }
  return mocks;
}
