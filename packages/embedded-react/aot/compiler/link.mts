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
 * An app split across files: the files the entry imports are merged into its program, as if they were written in it.
 *
 * Each file is parsed at lines and offsets of its own, after the files before it, so every node of the merged program
 * still says where it came from (see placeOfLine). A top-level name two files use is renamed in the later one, so each
 * file keeps its own scope, and an import is the name the other file declares.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {parse} from '@babel/parser';
import {traverse, types} from '@babel/core';
import {aotError} from './diagnostics.mts';
import type {NodePath} from '@babel/core';
import type {ParserPlugin} from '@babel/parser';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {NativeFile} from './native-files.mts';
import type {CompileOptions} from './options.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One of the app's files, and where its lines and characters sit among the compile's. */
export interface LinkedFile {
  /** Its path from the entry's folder: the entry's own name, `components/Card.tsx`. */
  path: string;
  source: string;
  /** The lines before its first. */
  firstLine: number;
  lineCount: number;
  /** The characters before its first. */
  offset: number;
}

/** The app's files, the entry first. */
export interface SourceLayout {
  files: LinkedFile[];
}

/** A line in one of the app's files. */
export interface FileLine {
  /** The file's index in the layout. */
  file: number;
  line: number;
}

/** A Babel scope. */
type Scope = NodePath['scope'];

/** What a file exports under one name: a name of its own, or another file's export. */
type FileExport =
  | {kind: 'local'; local: string}
  | {kind: 'passed'; from: LinkedModule; name: string};

/** An imported file being linked. */
interface LinkedModule {
  file: LinkedFile;
  ast: t.File;
  /** The files it imports, in order. */
  dependencies: LinkedModule[];
  exports: Map<string, FileExport>;
  /** The files it passes everything on from: `export * from`. */
  passedOn: LinkedModule[];
  /** The types it declares, which an import may name without `type`. */
  typeNames: Set<string>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** What a relative import may leave off its path, in the order they are tried. */
export const SOURCE_SUFFIXES = [
  '',
  '.tsx',
  '.ts',
  '.jsx',
  '.js',
  '/index.tsx',
  '/index.ts',
  '/index.jsx',
  '/index.js',
];

/** The layout of the compile under way, for an error thrown out of it. */
let currentLayout: SourceLayout | null = null;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The layout of the last app parsed.
 *
 * @returns The layout, or null before any parse.
 */
export const linkedLayout = (): SourceLayout | null => currentLayout;

/**
 * Whether a file native modules are read from declares any, so it stays an import: the compiler reads its modules
 * from it. One that holds only their interfaces is merged like any other file.
 *
 * @param file  The file.
 *
 * @returns True for a file that declares modules, and for JavaScript, which isn't read.
 */
export const declaresModules = (file: NativeFile): boolean =>
  file.source === null || /\bnativeModule\b/.test(file.source);

/** Forgets the last app parsed, before another compile. */
export function forgetLayout(): void {
  currentLayout = null;
}

/**
 * The file a line of the compile is in, and its line there.
 *
 * @param layout  The app's files.
 * @param line  A line as the compile numbers it.
 *
 * @returns The file and its line; the entry's for a line past every file.
 */
export function placeOfLine(layout: SourceLayout, line: number): FileLine {
  const file = layout.files.findIndex(
    candidate =>
      line > candidate.firstLine &&
      line <= candidate.firstLine + candidate.lineCount,
  );
  return file < 0
    ? {file: 0, line}
    : {file, line: line - layout.files[file].firstLine};
}

/**
 * Points a located error at its own file: its line there, and the file when it isn't the entry.
 *
 * @param error  The thrown value.
 * @param layout  The app's files.
 */
export function relocateError(
  error: unknown,
  layout: SourceLayout | null,
): void {
  const located = error as AotError;
  if (!layout || !located?.aotLoc || located.aotFile) return;
  const place = placeOfLine(layout, located.aotLoc.line);
  if (place.file === 0) return;
  const {path, source} = layout.files[place.file];
  located.aotLoc = {line: place.line, column: located.aotLoc.column};
  located.aotFile = {path, source};
}

/**
 * The Babel parser plugins for a file, by its extension.
 *
 * @param path  The file.
 *
 * @returns JSX for a .tsx, .jsx or .js file, and TypeScript for a .ts or .tsx one.
 */
export const pluginsFor = (path: string): ParserPlugin[] => [
  ...(/\.[cm]?ts$/.test(path) ? [] : (['jsx'] as ParserPlugin[])),
  ...(/\.[cm]?tsx?$/.test(path) ? (['typescript'] as ParserPlugin[]) : []),
];

/**
 * A path with its `.` and `..` parts worked out.
 *
 * @param path  A path with forward slashes.
 *
 * @returns The path, which starts with `..` when it leaves the entry's folder.
 */
export function normalizePath(path: string): string {
  const parts: string[] = [];
  for (const part of path.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..' && parts.length && parts.at(-1) !== '..') {
      parts.pop();
    } else {
      parts.push(part);
    }
  }
  return parts.join('/');
}

/**
 * A file's folder, from the entry's.
 *
 * @param path  The file's path from the entry's folder.
 *
 * @returns The folder, empty for the entry's own.
 */
const folderOf = (path: string): string =>
  path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '';

/**
 * An error pinned to a node.
 *
 * @param node  The node.
 * @param message  What is wrong.
 * @param hint  How to rewrite it.
 *
 * @returns The error, for the caller to throw.
 */
function errorAt(node: t.Node, message: string, hint?: string): AotError {
  const error = aotError(message, hint);
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }
  return error;
}

/**
 * A name as an import or export specifier spells it.
 *
 * @param name  The specifier's name.
 *
 * @returns The name.
 */
const specifierName = (name: t.Identifier | t.StringLiteral): string =>
  name.type === 'Identifier' ? name.name : name.value;

/**
 * Whether an import or export statement brings in values, not only types.
 *
 * @param statement  The statement.
 *
 * @returns False for `import type`, `export type`, and one whose every name is a type.
 */
export function bringsValues(
  statement:
    t.ImportDeclaration | t.ExportNamedDeclaration | t.ExportAllDeclaration,
): boolean {
  if (statement.type === 'ImportDeclaration') {
    return (
      statement.importKind !== 'type' &&
      (statement.specifiers.length === 0 ||
        statement.specifiers.some(
          specifier =>
            specifier.type !== 'ImportSpecifier' ||
            specifier.importKind !== 'type',
        ))
    );
  }
  return statement.exportKind !== 'type';
}

/**
 * A file's program scope, worked out afresh.
 *
 * @param ast  The file.
 *
 * @returns Its top-level scope.
 */
function programScope(ast: t.File): Scope {
  traverse.cache.clear();
  let scope: Scope | null = null;
  traverse(ast, {
    Program(path: NodePath<t.Program>) {
      scope = path.scope;
      path.stop();
    },
  });
  return scope!;
}

/**
 * Every name a file writes, bound or not.
 *
 * @param ast  The file.
 * @param names  Takes the names.
 */
function collectNames(ast: t.File, names: Set<string>): void {
  const visit = (node: any): void => {
    if (!node || typeof node !== 'object') return;
    if (Array.isArray(node)) {
      node.forEach(visit);
      return;
    }
    if (typeof node.type !== 'string') return;
    if (node.type === 'Identifier' || node.type === 'JSXIdentifier') {
      names.add(node.name);
    }
    for (const key of Object.keys(node)) {
      if (key === 'loc' || key.endsWith('Comments') || key === 'extra') {
        continue;
      }
      visit(node[key]);
    }
  };
  visit(ast.program);
}

/** Links the files one app imports. */
class Linker {
  private readonly sources: Record<string, string>;
  private readonly nativePaths: Set<string>;
  private readonly modules = new Map<string, LinkedModule>();
  /** The imported files, each after the files it imports. */
  private readonly ordered: LinkedModule[] = [];
  /** The files being read, outermost first, to find a loop. */
  private readonly reading: string[] = [];
  /** Every name any file writes, so a new name is new everywhere. */
  private readonly names = new Set<string>();
  /** Each top-level name taken, and what took it. */
  private readonly taken = new Map<string, string>();
  private readonly entry: t.File;
  readonly layout: SourceLayout;

  constructor(
    entry: t.File,
    entryPath: string,
    entrySource: string,
    options: Pick<CompileOptions, 'sourceFiles' | 'nativeFiles'>,
  ) {
    this.entry = entry;
    this.sources = options.sourceFiles ?? {};
    this.nativePaths = new Set(
      Object.entries(options.nativeFiles ?? {})
        .filter(([, file]) => declaresModules(file))
        .map(([importPath]) => importPath),
    );
    this.layout = {
      files: [
        {
          path: entryPath,
          source: entrySource,
          firstLine: 0,
          lineCount: entrySource.split('\n').length,
          offset: 0,
        },
      ],
    };
  }

  /**
   * The file a relative import names, and whether it declares native modules: by `nativeFiles`, or by its own source,
   * so one reached through a file that passes it on is found too.
   *
   * @param fromPath  The importing file.
   * @param importPath  The path it imports.
   *
   * @returns The file's path from the entry's folder, null when it isn't one of the app's source files, and whether
   *          it declares native modules.
   */
  private locate(
    fromPath: string,
    importPath: string,
  ): {path: string | null; native: boolean} {
    if (!importPath.startsWith('.')) return {path: null, native: false};
    const base = normalizePath(`${folderOf(fromPath)}/${importPath}`);
    if (
      this.nativePaths.has(importPath) ||
      this.nativePaths.has(base.startsWith('..') ? base : `./${base}`)
    ) {
      return {path: null, native: true};
    }
    const entryPath = this.layout.files[0].path;
    for (const suffix of SOURCE_SUFFIXES) {
      const candidate = base + suffix;
      if (candidate === entryPath) return {path: candidate, native: false};
      if (Object.hasOwn(this.sources, candidate)) {
        return {
          path: candidate,
          native: /\bnativeModule\b/.test(this.sources[candidate]),
        };
      }
    }
    return {path: null, native: false};
  }

  /**
   * The file a relative import names, when it is one the app is linked with.
   *
   * @param fromPath  The importing file.
   * @param importPath  The path it imports.
   *
   * @returns The file's path from the entry's folder, or null for one that isn't one of the app's source files, or
   *          that declares native modules: it stays an import, which the compiler reads its modules from.
   */
  private resolve(fromPath: string, importPath: string): string | null {
    const found = this.locate(fromPath, importPath);
    return found.native ? null : found.path;
  }

  /**
   * The error for a file that passes names on from one the app isn't linked with.
   *
   * @param path  The file that passes them on.
   * @param source  The path it passes them on from.
   * @param what  What it passes on: a name, or `everything`.
   * @param at  The statement or the name.
   *
   * @returns The error, for the caller to throw.
   */
  private passedOnError(
    path: string,
    source: string,
    what: string,
    at: t.Node,
  ): AotError {
    return this.locate(path, source).native
      ? errorAt(
          at,
          `AOT: ${path} passes ${what} on from ${source}, which declares native modules, and a native module isn't followed further`,
          'import it from the file that declares it, where it is used.',
        )
      : errorAt(
          at,
          `AOT: ${path} passes ${what} on from ${source}, and only the app's own files are followed`,
          'import it from there where it is used.',
        );
  }

  /**
   * Parses an imported file after the files before it, and reads the files it imports.
   *
   * @param path  The file's path from the entry's folder.
   * @param at  The import that names it.
   *
   * @returns The file.
   */
  private read(path: string, at: t.Node): LinkedModule {
    const known = this.modules.get(path);
    if (known) {
      if (this.reading.includes(path)) {
        const loop = [...this.reading.slice(this.reading.indexOf(path)), path];
        throw errorAt(
          at,
          `AOT: the app's files import each other in a loop: ${loop.join(' → ')}`,
          'move what the files share into a file of its own, which each of them imports.',
        );
      }
      return known;
    }

    const source = this.sources[path];
    const last = this.layout.files.at(-1)!;
    const file: LinkedFile = {
      path,
      source,
      firstLine: last.firstLine + last.lineCount,
      lineCount: source.split('\n').length,
      offset: last.offset + last.source.length,
    };
    this.layout.files.push(file);

    const plugins = pluginsFor(path);
    let ast: t.File;
    try {
      ast = parse(source, {
        sourceType: 'module',
        plugins,
        startLine: file.firstLine + 1,
        startColumn: 0,
        startIndex: file.offset,
      });
    } catch (error) {
      // Parsed again from line 1, so the syntax error counts the file's own lines.
      try {
        parse(source, {sourceType: 'module', plugins});
      } catch (plain) {
        (plain as Error).message = `${path}: ${(plain as Error).message}`;
        throw plain;
      }
      throw error;
    }

    const linked: LinkedModule = {
      file,
      ast,
      dependencies: [],
      exports: new Map(),
      passedOn: [],
      typeNames: new Set(),
    };
    this.modules.set(path, linked);
    this.reading.push(path);
    linked.dependencies = this.readImports(ast, path);
    this.reading.pop();
    this.ordered.push(linked);
    return linked;
  }

  /**
   * Reads the files a file imports or passes on.
   *
   * @param ast  The importing file.
   * @param path  Its path from the entry's folder.
   *
   * @returns The files, in the order it names them.
   */
  private readImports(ast: t.File, path: string): LinkedModule[] {
    const found: LinkedModule[] = [];
    for (const statement of ast.program.body) {
      const linked = this.followed(statement, path);
      if (linked && !found.includes(linked)) {
        found.push(linked);
      }
    }
    return found;
  }

  /**
   * The file a statement imports or passes on, when it is one the app is linked with.
   *
   * @param statement  A top-level statement.
   * @param path  Its file's path from the entry's folder.
   *
   * @returns The file, read if it wasn't yet, or null.
   */
  private followed(statement: t.Statement, path: string): LinkedModule | null {
    if (
      (statement.type !== 'ImportDeclaration' &&
        statement.type !== 'ExportNamedDeclaration' &&
        statement.type !== 'ExportAllDeclaration') ||
      !statement.source ||
      !bringsValues(statement)
    ) {
      return null;
    }
    const target = this.resolve(path, statement.source.value);
    if (target === null) return null;
    if (target === this.layout.files[0].path) {
      throw errorAt(
        statement.source,
        `AOT: ${path} imports the app's entry, ${target}, which imports it`,
        'move what the files share into a file of its own, which each of them imports.',
      );
    }
    return this.read(target, statement.source);
  }

  /**
   * A name no file writes, for a top-level name another file took.
   *
   * @param name  The name wanted.
   * @param path  The file that wants it.
   *
   * @returns The new name, which is taken from now on.
   */
  private fresh(name: string, path: string): string {
    const stem = path
      .replace(/\.[^./]+$/, '')
      .replace(/\/index$/, '')
      .split('/')
      .pop()!
      .replace(/[^A-Za-z0-9_]/g, '_');
    let candidate = `${name}_${stem}`;
    for (let count = 2; this.names.has(candidate); count++) {
      candidate = `${name}_${stem}${count}`;
    }
    this.names.add(candidate);
    return candidate;
  }

  /**
   * Names a default export that has no name, so an import has one to be: the file's own name where it is free.
   *
   * @param linked  The file.
   */
  private nameDefaultExport(linked: LinkedModule): void {
    const body = linked.ast.program.body;
    const at = body.findIndex(
      statement => statement.type === 'ExportDefaultDeclaration',
    );
    if (at < 0) return;
    const statement = body[at] as t.ExportDefaultDeclaration;
    const exported = statement.declaration;
    if (
      exported.type === 'Identifier' ||
      ((exported.type === 'FunctionDeclaration' ||
        exported.type === 'ClassDeclaration') &&
        exported.id)
    ) {
      return;
    }

    const own = new Set<string>();
    collectNames(linked.ast, own);
    const stem = linked.file.path
      .replace(/\.[^./]+$/, '')
      .replace(/\/index$/, '')
      .split('/')
      .pop()!;
    const name =
      /^[A-Za-z_][A-Za-z0-9_]*$/.test(stem) && !own.has(stem)
        ? stem
        : this.fresh('default', linked.file.path);
    this.names.add(name);
    const identifier = types.identifier(name);
    if (
      exported.type === 'FunctionDeclaration' ||
      exported.type === 'ClassDeclaration'
    ) {
      exported.id = identifier;
      return;
    }
    const declaration = types.variableDeclaration('const', [
      types.variableDeclarator(identifier, exported as t.Expression),
    ]);
    declaration.loc = statement.loc;
    declaration.start = statement.start;
    declaration.end = statement.end;
    const passed = types.exportDefaultDeclaration(types.identifier(name));
    passed.loc = statement.loc;
    body.splice(at, 1, declaration, passed);
  }

  /**
   * Whether an import statement is of a file the app is linked with.
   *
   * @param statement  A statement with a source.
   * @param path  Its file's path from the entry's folder.
   *
   * @returns The file, or null.
   */
  private moduleOf(
    statement: {source?: t.StringLiteral | null},
    path: string,
  ): LinkedModule | null {
    const target = statement.source
      ? this.resolve(path, statement.source.value)
      : null;
    return target === null ? null : (this.modules.get(target) ?? null);
  }

  /**
   * Gives each top-level name a file declares, or imports from outside the app, to that file: a name another file
   * took first is renamed through the file.
   *
   * @param ast  The file.
   * @param path  Its path from the entry's folder.
   */
  private claimNames(ast: t.File, path: string): void {
    const scope = programScope(ast);
    for (const [name, binding] of Object.entries(scope.bindings)) {
      const parent = binding.path.parent;
      let owner = `${path}:${name}`;
      if (parent.type === 'ImportDeclaration') {
        // An import from another of the app's files is that file's name, settled by bindImports.
        if (this.moduleOf(parent as t.ImportDeclaration, path)) continue;
        const node = binding.path.node;
        const imported =
          node.type === 'ImportSpecifier'
            ? specifierName(node.imported)
            : node.type === 'ImportDefaultSpecifier'
              ? 'default'
              : '*';
        owner = `import:${this.sourceOf(parent as t.ImportDeclaration, path)}:${imported}`;
      }
      const takenBy = this.taken.get(name);
      if (takenBy === undefined) {
        this.taken.set(name, owner);
      } else if (takenBy !== owner) {
        const renamed = this.fresh(name, path);
        scope.rename(name, renamed);
        this.taken.set(renamed, owner);
      }
    }
  }

  /**
   * An import's path as the entry would write it, so a file in another folder names the same file the same way.
   *
   * @param statement  The import.
   * @param path  Its file's path from the entry's folder.
   *
   * @returns The path.
   */
  private sourceOf(statement: t.ImportDeclaration, path: string): string {
    const importPath = statement.source.value;
    const folder = folderOf(path);
    if (!importPath.startsWith('.') || folder === '') return importPath;
    const fromEntry = normalizePath(`${folder}/${importPath}`);
    return fromEntry.startsWith('..') ? fromEntry : `./${fromEntry}`;
  }

  /**
   * Reads what a file exports, once its names are settled.
   *
   * @param linked  The file.
   */
  private readExports(linked: LinkedModule): void {
    const {path} = linked.file;
    for (const statement of linked.ast.program.body) {
      if (
        statement.type === 'TSInterfaceDeclaration' ||
        statement.type === 'TSTypeAliasDeclaration' ||
        statement.type === 'TSEnumDeclaration'
      ) {
        linked.typeNames.add(statement.id.name);
      } else if (statement.type === 'ExportAllDeclaration') {
        const from = this.moduleOf(statement, path);
        if (!from) {
          if (statement.exportKind === 'type') continue;
          throw this.passedOnError(
            path,
            statement.source.value,
            'everything',
            statement.source,
          );
        }
        linked.passedOn.push(from);
      } else if (statement.type === 'ExportDefaultDeclaration') {
        const exported = statement.declaration;
        linked.exports.set('default', {
          kind: 'local',
          local:
            exported.type === 'Identifier'
              ? exported.name
              : (exported as t.FunctionDeclaration).id!.name,
        });
      } else if (statement.type === 'ExportNamedDeclaration') {
        this.readNamedExport(linked, statement);
      }
    }
  }

  /**
   * Reads one `export` statement's names.
   *
   * @param linked  The file.
   * @param statement  The statement.
   */
  private readNamedExport(
    linked: LinkedModule,
    statement: t.ExportNamedDeclaration,
  ): void {
    const {path} = linked.file;
    const declaration = statement.declaration;
    if (declaration) {
      if (
        declaration.type === 'TSInterfaceDeclaration' ||
        declaration.type === 'TSTypeAliasDeclaration' ||
        declaration.type === 'TSEnumDeclaration'
      ) {
        linked.typeNames.add(declaration.id.name);
        return;
      }
      for (const name of Object.keys(
        types.getBindingIdentifiers(declaration),
      )) {
        linked.exports.set(name, {kind: 'local', local: name});
      }
      return;
    }

    const from = this.moduleOf(statement, path);
    for (const specifier of statement.specifiers) {
      if (specifier.type !== 'ExportSpecifier') {
        throw errorAt(
          specifier,
          `AOT: ${path} passes on a whole file under one name, which isn't supported`,
          'export the names themselves: export {Card} from "./Card".',
        );
      }
      const exported = specifierName(specifier.exported);
      if (statement.exportKind === 'type' || specifier.exportKind === 'type') {
        linked.typeNames.add(exported);
      } else if (from) {
        linked.exports.set(exported, {
          kind: 'passed',
          from,
          name: specifier.local.name,
        });
      } else if (statement.source) {
        throw this.passedOnError(
          path,
          statement.source.value,
          exported,
          specifier,
        );
      } else {
        linked.exports.set(exported, {
          kind: 'local',
          local: specifier.local.name,
        });
      }
    }
  }

  /**
   * The name a file's export has in the merged program.
   *
   * @param linked  The file.
   * @param name  The exported name.
   * @param seen  The files already asked, so files that pass each other on end.
   *
   * @returns The name, 'type' for a type, or null when the file exports no such name.
   */
  private exportedName(
    linked: LinkedModule,
    name: string,
    seen = new Set<LinkedModule>(),
  ): string | 'type' | null {
    if (seen.has(linked)) return null;
    seen.add(linked);
    const exported = linked.exports.get(name);
    if (exported?.kind === 'local') return exported.local;
    if (exported?.kind === 'passed') {
      return this.exportedName(exported.from, exported.name, seen);
    }
    let isType = linked.typeNames.has(name);
    if (name !== 'default') {
      for (const from of linked.passedOn) {
        const passed = this.exportedName(from, name, seen);
        if (passed === 'type') {
          isType = true;
        } else if (passed !== null) {
          return passed;
        }
      }
    }
    return isType ? 'type' : null;
  }

  /**
   * Makes each name a file imports from another of the app's files the name that file declares, and drops the import.
   *
   * @param ast  The importing file.
   * @param path  Its path from the entry's folder.
   */
  private bindImports(ast: t.File, path: string): void {
    const renames: {local: string; target: string}[] = [];
    const linkedImports = new Set<t.Statement>();
    for (const statement of ast.program.body) {
      if (statement.type !== 'ImportDeclaration') continue;
      const from = this.moduleOf(statement, path);
      if (!from) continue;
      linkedImports.add(statement);
      // A type import of a linked file names a type the merged program declares.
      if (!bringsValues(statement)) continue;

      for (const specifier of statement.specifiers) {
        if (specifier.type === 'ImportNamespaceSpecifier') {
          throw errorAt(
            specifier,
            `AOT: \`import * as ${specifier.local.name}\` of one of the app's files isn't supported`,
            `import the names it uses: import {…} from '${statement.source.value}'.`,
          );
        }
        if (
          specifier.type === 'ImportSpecifier' &&
          specifier.importKind === 'type'
        ) {
          continue;
        }
        const imported =
          specifier.type === 'ImportSpecifier'
            ? specifierName(specifier.imported)
            : 'default';
        const target = this.exportedName(from, imported);
        if (target === null) {
          throw errorAt(
            specifier,
            imported === 'default'
              ? `AOT: ${from.file.path} has no default export`
              : `AOT: ${from.file.path} doesn't export ${imported}`,
            imported === 'default'
              ? `export one from it, or import what it does export by name: import {…} from '${statement.source.value}'.`
              : `export it there: export const ${imported} = …, or export function ${imported}(…) {…}.`,
          );
        }
        if (target !== 'type' && target !== specifier.local.name) {
          renames.push({local: specifier.local.name, target});
        }
      }
    }

    // Renamed while the imports still bind the names, and through a name of its own first, so two imports that swap
    // names don't meet.
    if (renames.length) {
      const scope = programScope(ast);
      const moved = renames.map(({local, target}) => {
        const interim = this.fresh(local, path);
        scope.rename(local, interim);
        return {interim, target};
      });
      for (const {interim, target} of moved) {
        this.freeInnerName(ast, target, path);
        programScope(ast).rename(interim, target);
      }
    }
    ast.program.body = ast.program.body.filter(
      statement => !linkedImports.has(statement),
    );
  }

  /**
   * Renames a name a function in a file binds for itself, so the same name from another file can be read there.
   *
   * @param ast  The file.
   * @param name  The name.
   * @param path  The file's path from the entry's folder.
   */
  private freeInnerName(ast: t.File, name: string, path: string): void {
    traverse.cache.clear();
    const rename = (scopePath: NodePath): void => {
      if (
        scopePath.scope.path === scopePath &&
        scopePath.scope.hasOwnBinding(name)
      ) {
        scopePath.scope.rename(name, this.fresh(name, path));
      }
    };
    traverse(ast, {
      Program(programPath: NodePath<t.Program>) {
        programPath.traverse({Scopable: rename});
      },
    });
  }

  /**
   * Takes the `export` off an imported file's statements, which the merged program has no use for.
   *
   * @param linked  The file.
   */
  private dropExports(linked: LinkedModule): void {
    linked.ast.program.body = linked.ast.program.body.flatMap(
      (statement): t.Statement[] => {
        if (statement.type === 'ExportAllDeclaration') return [];
        if (statement.type === 'ExportNamedDeclaration') {
          return statement.declaration ? [statement.declaration] : [];
        }
        if (statement.type === 'ExportDefaultDeclaration') {
          const exported = statement.declaration;
          return exported.type === 'FunctionDeclaration' ||
            exported.type === 'ClassDeclaration'
            ? [exported]
            : [];
        }
        return [statement];
      },
    );
  }

  /**
   * Writes a file's remaining imports as the entry would, and drops what an earlier file already imports.
   *
   * @param ast  The file.
   * @param path  Its path from the entry's folder.
   * @param imported  Each import kept so far.
   */
  private settleImports(
    ast: t.File,
    path: string,
    imported: Set<string>,
  ): void {
    ast.program.body = ast.program.body.filter(statement => {
      if (statement.type !== 'ImportDeclaration') return true;
      const source = this.sourceOf(statement, path);
      if (source !== statement.source.value) {
        statement.source.value = source;
        delete statement.source.extra;
      }
      const kind = statement.importKind ?? 'value';
      if (statement.specifiers.length === 0) {
        const key = `${kind}:${source}`;
        if (imported.has(key)) return false;
        imported.add(key);
        return true;
      }
      statement.specifiers = statement.specifiers.filter(specifier => {
        const name =
          specifier.type === 'ImportSpecifier'
            ? `${specifier.importKind ?? 'value'}:${specifierName(specifier.imported)}`
            : specifier.type === 'ImportDefaultSpecifier'
              ? 'default'
              : '*';
        const key = `${kind}:${source}:${name}:${specifier.local.name}`;
        if (imported.has(key)) return false;
        imported.add(key);
        return true;
      });
      return statement.specifiers.length > 0;
    });
  }

  /**
   * Merges the files the entry imports into it: each file's statements, before those of the files that import it.
   */
  link(): void {
    const entryPath = this.layout.files[0].path;
    this.reading.push(entryPath);
    this.readImports(this.entry, entryPath);
    this.reading.pop();
    if (!this.ordered.length) return;

    collectNames(this.entry, this.names);
    for (const linked of this.ordered) {
      collectNames(linked.ast, this.names);
    }
    for (const linked of this.ordered) {
      this.nameDefaultExport(linked);
    }

    // The entry's names are its own; the other files take theirs in the order they are merged.
    this.claimNames(this.entry, entryPath);
    for (const linked of this.ordered) {
      this.claimNames(linked.ast, linked.file.path);
    }
    for (const linked of this.ordered) {
      this.readExports(linked);
    }
    for (const linked of this.ordered) {
      this.bindImports(linked.ast, linked.file.path);
    }
    this.bindImports(this.entry, entryPath);

    // The entry may pass a file's exports on, which nothing imports.
    this.entry.program.body = this.entry.program.body.filter(
      statement =>
        !(
          (statement.type === 'ExportAllDeclaration' ||
            statement.type === 'ExportNamedDeclaration') &&
          this.moduleOf(statement, entryPath)
        ),
    );

    const imported = new Set<string>();
    this.settleImports(this.entry, entryPath, imported);
    for (const linked of this.ordered) {
      this.dropExports(linked);
      this.settleImports(linked.ast, linked.file.path, imported);
    }
    this.entry.program.body = [
      ...this.ordered.flatMap(linked => linked.ast.program.body),
      ...this.entry.program.body,
    ];
    traverse.cache.clear();
  }
}

/**
 * Merges the app's other files into its parsed entry, in place, and records where each file's lines are.
 *
 * A file in `nativeFiles` is left as an import: it declares native modules, which the compiler reads from it.
 *
 * @param entry  The parsed entry, types and all.
 * @param source  The entry's source.
 * @param entryPath  The entry's name, as the debug table names it.
 * @param options  compilePackage's options: `sourceFiles` holds the files the app imports.
 *
 * @returns The app's files, the entry first.
 */
export function linkApp(
  entry: t.File,
  source: string,
  entryPath: string,
  options: Pick<CompileOptions, 'sourceFiles' | 'nativeFiles'>,
): SourceLayout {
  const linker = new Linker(entry, entryPath, source, options);
  currentLayout = linker.layout;
  if (options.sourceFiles && Object.keys(options.sourceFiles).length) {
    linker.link();
  }
  return linker.layout;
}
