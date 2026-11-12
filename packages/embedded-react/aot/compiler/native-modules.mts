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
 * Native modules: `const Sensors = nativeModule<Sensors>('sensors', 1)` at module level, typed by an interface in
 * the app's file or in a local .ts file the CLI reads. Each method called becomes an import named
 * `module.method`, and each call a CALLN. The interface's `events: {reading(celsius: number): void}` are what the
 * firmware posts; each useNativeEvent becomes a handler the runtime runs with the event's values. In Flow A the
 * bundler passes nativeModule() each method's and event's signature instead, and in the simulators the module's mock
 * too.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {existsSync, readFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {parse} from '@babel/parser';
import {aotError} from './diagnostics.mts';
import {lowerExpr} from './expressions.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {
  IrNativeCall,
  NativeImport,
  NativeType,
} from './ir/expressions.mts';
import type {NativeEvent} from './ir/functions.mts';
import type {Env} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A method of a native module's interface, or why it can't be called yet. */
type NativeMember =
  | {kind: 'method'; params: NativeType[]; result: NativeType | 'void'}
  | {kind: 'refused'; reason: string; hint: string};

/** A module the app declares with nativeModule(). */
export interface NativeModule {
  /** The module's name, which each import's name starts with. */
  name: string;
  version: number;
  /** The interface's name, for errors. */
  interfaceName: string;
  members: Map<string, NativeMember>;
  /** The interface's `events`, each a method that returns void. */
  events: Map<string, NativeMember>;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Native methods a package may import: the runtime's ERB_MAX_IMPORTS. */
export const MAX_IMPORTS = 32;

const MAX_VERSION = 0xffff;

/** Parameters a native method may take: what Flow A's bridge passes (NATIVE_MAX_PARAMETERS in er_runtime.c). */
const MAX_PARAMETERS = 16;

const SIGNATURE_CODES: Record<NativeType | 'void', string> = {
  int: 'i',
  float: 'f',
  bool: 'b',
  void: 'v',
};

const DECLARATION_HINT =
  "declare it at module level: const Sensors = nativeModule<Sensors>('sensors', 1)";

const EVENTS_HINT =
  'declare the events as an object of methods that return void: events: {reading(celsius: number): void};';

/** Native event handlers a package may have: the runtime's ERB_MAX_NATIVE_EVENTS. */
export const MAX_NATIVE_EVENTS = 32;

/** What an interface file may be called, after the path the app imports it by. */
const INTERFACE_EXTENSIONS = ['.ts', '.mts', '.tsx', '/index.ts'];

/** What a module's mock may be called, after `<name>.mock`. */
const MOCK_EXTENSIONS = ['.ts', '.tsx', '.js', '.jsx'];

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An error pinned to a node of the app's source.
 *
 * @param node  The node.
 * @param message  What is wrong.
 * @param hint  How to fix it.
 *
 * @returns The error, for the caller to throw.
 */
function errorAt(node: t.Node, message: string, hint: string): AotError {
  const error = aotError(message, hint);
  if (node.loc) {
    error.aotLoc = node.loc.start;
  }
  return error;
}

/**
 * The nativeModule() calls declared at module level, by the const each one is bound to.
 *
 * @param program  The app's program, types and all.
 *
 * @returns Each declarator.
 */
function declarationsOf(program: t.Program): t.VariableDeclarator[] {
  const declarators: t.VariableDeclarator[] = [];
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration'
        ? statement.declaration
        : statement;
    if (declaration?.type !== 'VariableDeclaration') continue;
    for (const declarator of declaration.declarations) {
      if (!isNativeModuleCall(declarator.init)) continue;
      if (declaration.kind !== 'const' || declarator.id.type !== 'Identifier') {
        throw errorAt(
          declarator,
          'AOT: a native module must be bound to a const',
          DECLARATION_HINT + '.',
        );
      }
      declarators.push(declarator);
    }
  }
  return declarators;
}

/**
 * Whether a node is a nativeModule() call.
 *
 * @param node  The node.
 *
 * @returns True when it is.
 */
const isNativeModuleCall = (node: t.Node | null | undefined): boolean =>
  node?.type === 'CallExpression' &&
  node.callee.type === 'Identifier' &&
  node.callee.name === 'nativeModule';

/**
 * Refuses a nativeModule() call anywhere but a module-level declaration.
 *
 * @param node  The node to search, at any depth.
 * @param declared  The calls the module level declares.
 */
function refuseInnerDeclarations(
  node: unknown,
  declared: ReadonlySet<t.Node>,
): void {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) {
    node.forEach(child => refuseInnerDeclarations(child, declared));
    return;
  }
  const record = node as Record<string, unknown> & t.Node;
  if (typeof record.type !== 'string') return;
  if (isNativeModuleCall(record) && !declared.has(record)) {
    throw errorAt(
      record,
      'AOT: nativeModule() is called outside module level',
      DECLARATION_HINT + '.',
    );
  }
  for (const key of Object.keys(record)) {
    if (key !== 'loc' && !key.endsWith('Comments')) {
      refuseInnerDeclarations(record[key], declared);
    }
  }
}

/**
 * The interface a nativeModule() call names in its type argument.
 *
 * @param call  The call.
 *
 * @returns The interface's name.
 */
function interfaceNameOf(call: t.CallExpression): string {
  const typeArguments = call.typeParameters?.params ?? [];
  const [typeArgument] = typeArguments;
  if (
    typeArguments.length !== 1 ||
    typeArgument.type !== 'TSTypeReference' ||
    typeArgument.typeName.type !== 'Identifier' ||
    typeArgument.typeParameters
  ) {
    throw errorAt(
      call,
      'AOT: nativeModule() needs the interface that declares the module, as its type argument',
      DECLARATION_HINT +
        ', in a .tsx app, with `interface Sensors` in the same file or a local .ts file.',
    );
  }
  return typeArgument.typeName.name;
}

/**
 * The module's name and version, from the call's arguments.
 *
 * @param call  The nativeModule() call.
 *
 * @returns The name and version.
 */
function nameAndVersionOf(call: t.CallExpression): {
  name: string;
  version: number;
} {
  const [name, version] = call.arguments;
  if (
    call.arguments.length !== 2 ||
    name.type !== 'StringLiteral' ||
    version.type !== 'NumericLiteral'
  ) {
    throw errorAt(
      call,
      'AOT: nativeModule() takes the module name and version as literals',
      DECLARATION_HINT + '.',
    );
  }
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name.value)) {
    throw errorAt(
      name,
      `AOT: "${name.value}" isn't a native module name`,
      'name it with letters, digits and underscores, as the firmware registers it: "sensors".',
    );
  }
  if (
    !Number.isInteger(version.value) ||
    version.value < 0 ||
    version.value > MAX_VERSION
  ) {
    throw errorAt(
      version,
      `AOT: a native module's version is a whole number from 0 to ${MAX_VERSION}`,
      'give the version the firmware registers the module with.',
    );
  }
  return {name: name.value, version: version.value};
}

/**
 * The members of an interface, or of a type alias of an object type, declared in a file.
 *
 * @param program  The file.
 * @param name  The interface's name.
 *
 * @returns Its members, null when the file doesn't declare it, or a reason it can't be read.
 */
function declaredMembers(
  program: t.Program,
  name: string,
): t.TSTypeElement[] | string | null {
  for (const statement of program.body) {
    const declaration =
      statement.type === 'ExportNamedDeclaration' ||
      statement.type === 'ExportDefaultDeclaration'
        ? statement.declaration
        : statement;
    if (
      declaration?.type === 'TSInterfaceDeclaration' &&
      declaration.id.name === name
    ) {
      if (declaration.extends?.length || declaration.typeParameters) {
        return `the interface ${name} extends another or takes type parameters`;
      }
      return declaration.body.body;
    }
    if (
      declaration?.type === 'TSTypeAliasDeclaration' &&
      declaration.id.name === name
    ) {
      return declaration.typeAnnotation.type === 'TSTypeLiteral' &&
        !declaration.typeParameters
        ? declaration.typeAnnotation.members
        : `the type ${name} isn't an object type`;
    }
  }
  return null;
}

/**
 * Where the app imports a name from, when it imports it from a local file.
 *
 * @param program  The app's program.
 * @param name  The name the app reads.
 *
 * @returns The import path as the app writes it, and the name the file exports; or null.
 */
function localImportOf(
  program: t.Program,
  name: string,
): {path: string; exported: string} | null {
  for (const statement of program.body) {
    if (
      statement.type !== 'ImportDeclaration' ||
      !statement.source.value.startsWith('.')
    ) {
      continue;
    }
    for (const specifier of statement.specifiers) {
      if (
        specifier.type === 'ImportSpecifier' &&
        specifier.local.name === name
      ) {
        const imported = specifier.imported;
        return {
          path: statement.source.value,
          exported:
            imported.type === 'Identifier' ? imported.name : imported.value,
        };
      }
    }
  }
  return null;
}

/**
 * Parses an interface file.
 *
 * @param source  Its source.
 *
 * @returns Its program.
 */
const parseInterfaceFile = (source: string): t.Program =>
  parse(source, {sourceType: 'module', plugins: ['typescript', 'jsx']}).program;

/**
 * The type a parameter or result is passed as, or why it can't be.
 *
 * @param type  The TypeScript type.
 * @param isResult  It is the result, which may be void.
 *
 * @returns The native type, or a reason.
 */
function nativeTypeOf(
  type: t.TSType | undefined,
  isResult: boolean,
): NativeType | 'void' | {reason: string} {
  switch (type?.type) {
    case 'TSNumberKeyword':
      return 'float';
    case 'TSBooleanKeyword':
      return 'bool';
    case 'TSVoidKeyword':
      return isResult ? 'void' : {reason: 'takes void'};
    case 'TSTypeReference':
      if (
        type.typeName.type === 'Identifier' &&
        type.typeName.name === 'Int' &&
        !type.typeParameters
      ) {
        return 'int';
      }
      break;
    case 'TSStringKeyword':
      return {
        reason: `${isResult ? 'returns' : 'takes'} a string, which native modules don't pass yet`,
      };
    case undefined:
      return {
        reason: `has no ${isResult ? 'result' : 'parameter'} type`,
      };
  }
  return {
    reason: `${isResult ? 'returns' : 'takes'} a type native modules don't pass yet`,
  };
}

/**
 * One member of a module's interface: a method whose parameters and result are Int, number or boolean.
 *
 * @param member  The interface member.
 *
 * @returns Its name and what it is, or null for a member without a plain name.
 */
function memberOf(
  member: t.TSTypeElement,
): {name: string; member: NativeMember} | null {
  if (
    (member.type !== 'TSMethodSignature' &&
      member.type !== 'TSPropertySignature') ||
    member.computed ||
    member.key.type !== 'Identifier'
  ) {
    return null;
  }
  const {name} = member.key;
  const refuse = (reason: string, hint: string) => ({
    name,
    member: {kind: 'refused' as const, reason, hint},
  });
  const typeHint =
    'a native method takes and returns Int (from embedded-react), number and boolean, and may return void.';

  // A method, or a property whose type is a function.
  const functionType =
    member.type === 'TSMethodSignature'
      ? member
      : member.typeAnnotation?.typeAnnotation.type === 'TSFunctionType'
        ? member.typeAnnotation.typeAnnotation
        : null;
  if (!functionType) {
    return refuse(
      "isn't a method",
      'declare it as a method: read(channel: Int): number;',
    );
  }
  if (member.optional) {
    return refuse('is optional', 'declare every method the firmware provides.');
  }
  if (functionType.typeParameters) {
    return refuse('takes type parameters', typeHint);
  }

  if (functionType.parameters.length > MAX_PARAMETERS) {
    return refuse(
      `takes more than ${MAX_PARAMETERS} parameters`,
      'split it into methods that take fewer.',
    );
  }
  const params: NativeType[] = [];
  for (const parameter of functionType.parameters) {
    if (parameter.type !== 'Identifier' || parameter.optional) {
      return refuse(
        'has an optional, rest or destructured parameter',
        'give each parameter a name and a type: read(channel: Int): number;',
      );
    }
    const type = nativeTypeOf(
      parameter.typeAnnotation?.type === 'TSTypeAnnotation'
        ? parameter.typeAnnotation.typeAnnotation
        : undefined,
      false,
    );
    if (typeof type === 'object') return refuse(type.reason, typeHint);
    params.push(type as NativeType);
  }
  const result = nativeTypeOf(
    functionType.typeAnnotation?.typeAnnotation,
    true,
  );
  if (typeof result === 'object') return refuse(result.reason, typeHint);
  return {name, member: {kind: 'method', params, result}};
}

/**
 * Whether an interface member is its `events`.
 *
 * @param member  The member.
 *
 * @returns True when it is named `events`.
 */
const isEventsMember = (member: t.TSTypeElement): boolean =>
  (member.type === 'TSPropertySignature' ||
    member.type === 'TSMethodSignature') &&
  !member.computed &&
  member.key.type === 'Identifier' &&
  member.key.name === 'events';

/**
 * A module's events, from its interface's `events: {…}`. Each is a method that returns void.
 *
 * @param member  The `events` member.
 * @param call  The nativeModule() call, for errors.
 * @param interfaceName  The interface's name, for errors.
 *
 * @returns Each event, or why it can't be used.
 */
function eventsOf(
  member: t.TSTypeElement,
  call: t.CallExpression,
  interfaceName: string,
): Map<string, NativeMember> {
  const type =
    member.type === 'TSPropertySignature'
      ? member.typeAnnotation?.typeAnnotation
      : undefined;
  if (
    type?.type !== 'TSTypeLiteral' ||
    (member as t.TSPropertySignature).optional
  ) {
    throw errorAt(
      call,
      `AOT: ${interfaceName}'s events aren't an object type of events`,
      EVENTS_HINT,
    );
  }
  const events = new Map<string, NativeMember>();
  for (const element of type.members) {
    const event = memberOf(element);
    if (!event) continue;
    if (event.name === '__proto__') {
      throw errorAt(
        call,
        `AOT: ${interfaceName} has an event named __proto__, which a native module can't have`,
        "rename it; JavaScript keeps __proto__ for an object's prototype.",
      );
    }
    const refused = (reason: string, hint: string): NativeMember => ({
      kind: 'refused',
      reason,
      hint,
    });
    events.set(
      event.name,
      events.has(event.name)
        ? refused(
            'is overloaded, but a native event has one signature',
            'declare each event once, and give another signature a name of its own.',
          )
        : event.member.kind === 'method' && event.member.result !== 'void'
          ? refused(
              'returns a value, but an event returns nothing',
              EVENTS_HINT,
            )
          : event.member,
    );
  }
  return events;
}

/**
 * The native modules an app declares, by the const each is bound to. Run on the app before its types are stripped.
 *
 * @param program  The app's program, types and all.
 * @param interfaces  The local files the app imports a module's interface from, by import path (see
 *                    readNativeInterfaces).
 *
 * @returns The modules.
 */
export function collectNativeModules(
  program: t.Program,
  interfaces: Record<string, string> = {},
): Map<string, NativeModule> {
  const modules = new Map<string, NativeModule>();
  const names = new Set<string>();
  const declarators = declarationsOf(program);
  refuseInnerDeclarations(
    program.body,
    new Set(declarators.map(declarator => declarator.init!)),
  );
  for (const declarator of declarators) {
    const call = declarator.init as t.CallExpression;
    const interfaceName = interfaceNameOf(call);
    const {name, version} = nameAndVersionOf(call);
    if (names.has(name)) {
      throw errorAt(
        call,
        `AOT: the native module "${name}" is declared twice`,
        'declare each module once, and call its methods through that const.',
      );
    }
    names.add(name);

    // The interface, in this file or in the local file the app imports it from.
    let members = declaredMembers(program, interfaceName);
    if (members === null) {
      const local = localImportOf(program, interfaceName);
      const source = local ? interfaces[local.path] : undefined;
      if (!local || source === undefined) {
        throw errorAt(
          call,
          `AOT: can't find the interface ${interfaceName}`,
          'declare it in this file, or in a local .ts file the app imports it from: ' +
            "import type {Sensors} from './sensors';",
        );
      }
      members = declaredMembers(parseInterfaceFile(source), local.exported);
      if (members === null) {
        throw errorAt(
          call,
          `AOT: ${local.path} doesn't declare the interface ${local.exported}`,
          'declare the interface in that file itself; a native module interface is not followed through re-exports.',
        );
      }
    }
    if (typeof members === 'string') {
      throw errorAt(
        call,
        `AOT: ${members}, which a native module can't be declared by`,
        'declare the module as an interface of its methods: interface Sensors { read(channel: Int): number; }',
      );
    }

    const module: NativeModule = {
      name,
      version,
      interfaceName,
      members: new Map(),
      events: new Map(),
    };
    for (const element of members) {
      if (isEventsMember(element)) {
        module.events = eventsOf(element, call, interfaceName);
        continue;
      }
      const member = memberOf(element);
      if (!member) continue;
      // Flow A keeps the methods in objects, where __proto__ is the prototype.
      if (member.name === '__proto__') {
        throw errorAt(
          call,
          `AOT: ${interfaceName} has a method named __proto__, which a native module can't have`,
          "rename it; JavaScript keeps __proto__ for an object's prototype.",
        );
      }
      // An import has one signature, so an overloaded method can't be called.
      module.members.set(
        member.name,
        module.members.has(member.name)
          ? {
              kind: 'refused',
              reason: 'is overloaded, but a native method has one signature',
              hint: 'declare each method once, and give another signature a name of its own.',
            }
          : member.member,
      );
    }
    modules.set((declarator.id as t.Identifier).name, module);
  }
  return modules;
}

/**
 * A method's signature: its result, ':', then its parameters.
 *
 * @param member  The method.
 *
 * @returns The signature, such as "i:if".
 */
const signatureOf = (member: Extract<NativeMember, {kind: 'method'}>): string =>
  SIGNATURE_CODES[member.result] +
  ':' +
  member.params.map(param => SIGNATURE_CODES[param]).join('');

/**
 * Flow A's side: since the bundle has no types, each nativeModule() call is given its methods' and events' signatures
 * as a third argument, `{methods, events}`, read from its interface as the compiler reads it. One that can't be used
 * gets the reason instead.
 * With mocks, for the simulators, each call is also given its module's mock, or null when it has none.
 *
 * @param source  An app file's source.
 * @param interfaces  The local files it imports interfaces from (see readNativeInterfaces).
 * @param mocks  Each module's mock as the file imports it, or null when it has none, by module name.
 *
 * @returns The source with the signatures added.
 */
export function addNativeSignatures(
  source: string,
  interfaces: Record<string, string>,
  mocks?: Map<string, string | null>,
): string {
  const program = parse(source, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const modules = collectNativeModules(program, interfaces);
  const mockImports: string[] = [];
  const mockNames = new Set<string>();
  let signed = source;
  for (const declarator of declarationsOf(program).reverse()) {
    const module = modules.get((declarator.id as t.Identifier).name)!;
    const signatures = (members: Map<string, NativeMember>) =>
      Object.fromEntries(
        [...members].map(([name, member]) => [
          name,
          member.kind === 'method'
            ? signatureOf(member)
            : {reason: member.reason},
        ]),
      );
    let added = `, ${JSON.stringify({
      methods: signatures(module.members),
      events: signatures(module.events),
    })}`;
    if (mocks) {
      const mockPath = mocks.get(module.name);
      if (mockPath) {
        // A name the file doesn't use, nor another mock.
        let mockName = `__erNativeMock_${module.name}`;
        while (source.includes(mockName) || mockNames.has(mockName)) {
          mockName += '_';
        }
        mockNames.add(mockName);
        mockImports.push(
          `import ${mockName} from ${JSON.stringify(mockPath)};`,
        );
        added += `, ${mockName}`;
      } else {
        added += ', null';
      }
    }
    const versionEnd = (declarator.init as t.CallExpression).arguments[1].end!;
    signed = signed.slice(0, versionEnd) + added + signed.slice(versionEnd);
  }
  // At the end, so the app's lines keep their numbers.
  return mockImports.length
    ? `${signed}\n${mockImports.reverse().join('\n')}\n`
    : signed;
}

/**
 * The local file an app imports an interface from, by the path it writes: `./sensors`, `./sensors.ts`, or
 * `./sensors.js` as NodeNext writes it.
 *
 * @param baseDirectory  The app's directory.
 * @param importPath  The path the app imports it by.
 *
 * @returns The file, or undefined when there is none.
 */
function interfaceFileOf(
  baseDirectory: string,
  importPath: string,
): string | undefined {
  const base = resolve(baseDirectory, importPath);
  const candidates = [
    base,
    base.replace(/\.(m?)js$/, '.$1ts'),
    ...INTERFACE_EXTENSIONS.map(extension => base + extension),
  ];
  return candidates.find(path => /\.[cm]?tsx?$/.test(path) && existsSync(path));
}

/**
 * Reads the local files an app imports its native module interfaces from, so the compile itself stays pure.
 *
 * @param source  The app's source.
 * @param baseDirectory  The app's directory, which the import paths are relative to.
 *
 * @returns Each file's source, by the import path the app writes.
 */
export function readNativeInterfaces(
  source: string,
  baseDirectory: string,
): Record<string, string> {
  const program = parse(source, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const interfaces: Record<string, string> = {};
  for (const declarator of declarationsOf(program)) {
    const call = declarator.init as t.CallExpression;
    const typeArgument = call.typeParameters?.params[0];
    if (
      typeArgument?.type !== 'TSTypeReference' ||
      typeArgument.typeName.type !== 'Identifier' ||
      declaredMembers(program, typeArgument.typeName.name) !== null
    ) {
      continue;
    }
    const local = localImportOf(program, typeArgument.typeName.name);
    if (!local || local.path in interfaces) continue;
    const found = interfaceFileOf(baseDirectory, local.path);
    if (found) {
      interfaces[local.path] = readFileSync(found, 'utf8');
    }
  }
  return interfaces;
}

/**
 * Where each module's mock for the simulators may be: `<name>.mock.ts` (or .tsx, .js, .jsx) beside its interface.
 *
 * @param source  An app file's source.
 * @param baseDirectory  The app file's directory.
 *
 * @returns The paths to look for, in order, by module name.
 */
export function nativeMockFiles(
  source: string,
  baseDirectory: string,
): Map<string, string[]> {
  const program = parse(source, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const mocks = new Map<string, string[]>();
  for (const declarator of declarationsOf(program)) {
    const call = declarator.init as t.CallExpression;
    const [name] = call.arguments;
    if (name?.type !== 'StringLiteral') continue;
    const typeArgument = call.typeParameters?.params[0];
    const interfaceName =
      typeArgument?.type === 'TSTypeReference' &&
      typeArgument.typeName.type === 'Identifier'
        ? typeArgument.typeName.name
        : null;
    const local =
      interfaceName && declaredMembers(program, interfaceName) === null
        ? localImportOf(program, interfaceName)
        : null;
    const interfaceFile = local
      ? interfaceFileOf(baseDirectory, local.path)
      : undefined;
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

/**
 * The native module a call's callee reads a method of, when it is one: `Sensors.read(…)`.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 *
 * @returns The module and the method's name, or null.
 */
export function nativeCalleeOf(
  node: t.CallExpression,
  environment: Env,
): {module: NativeModule; method: string} | null {
  const {callee} = node;
  if (
    callee.type !== 'MemberExpression' ||
    callee.computed ||
    callee.object.type !== 'Identifier' ||
    callee.property.type !== 'Identifier'
  ) {
    return null;
  }
  const binding = callee.object.name;
  const module = environment.nativeModules?.get(binding);
  if (
    !module ||
    environment.locals.has(binding) ||
    environment.state.has(binding) ||
    binding in environment.constants
  ) {
    return null;
  }
  return {module, method: callee.property.name};
}

/**
 * Whether an identifier reads a native module itself, rather than calling one of its methods.
 *
 * @param name  The identifier.
 * @param environment  The expression environment.
 *
 * @returns True when it does.
 */
export const readsNativeModule = (name: string, environment: Env): boolean =>
  !!environment.nativeModules?.has(name) &&
  !environment.locals.has(name) &&
  !environment.state.has(name) &&
  !(name in environment.constants);

/**
 * Lowers a call to a native method. Only a handler, an effect or a timer calls one: Flow A would call it on every
 * render, and Flow B only works out what changed, so the two would show different values.
 *
 * @param node  The call.
 * @param callee  Its module and method (see nativeCalleeOf).
 * @param environment  The expression environment.
 *
 * @returns The call.
 */
export function lowerNativeCall(
  node: t.CallExpression,
  callee: {module: NativeModule; method: string},
  environment: Env,
): IrNativeCall {
  const {module, method} = callee;
  const binding = ((node.callee as t.MemberExpression).object as t.Identifier)
    .name;
  const what = `${binding}.${method}`;
  if (!environment.outsideRender || environment.workedInRender) {
    throw errorAt(
      node,
      `AOT: ${what}() is called in render`,
      'call a native method in a handler, an effect or a timer, and keep what it returns in state: ' +
        'Flow A would call it on every render, and Flow B only when what it reads changes.',
    );
  }
  const member = module.members.get(method);
  if (!member) {
    throw errorAt(
      node,
      `AOT: ${module.interfaceName} has no method ${method}`,
      `declare it in the interface: ${method}(…): number;`,
    );
  }
  if (member.kind === 'refused') {
    throw errorAt(
      node,
      `AOT: ${module.interfaceName}.${method} ${member.reason}`,
      member.hint,
    );
  }
  if (node.arguments.length !== member.params.length) {
    throw errorAt(
      node,
      `AOT: ${what}() takes ${member.params.length} argument${member.params.length === 1 ? '' : 's'}, ` +
        `not ${node.arguments.length}`,
      `pass one for each parameter ${module.interfaceName}.${method} declares.`,
    );
  }

  // Each argument must already be of its parameter's type, except a whole number passed as a number.
  const callArguments = node.arguments.map((argument, index) => {
    if (
      argument.type === 'SpreadElement' ||
      argument.type === 'ArgumentPlaceholder'
    ) {
      throw errorAt(
        argument,
        `AOT: ${what}() takes plain arguments`,
        'pass each argument by itself.',
      );
    }
    const lowered = lowerExpr(argument, environment);
    const param = member.params[index];
    const type = lowered.type.kind;
    const fits =
      param === 'float'
        ? type === 'int' || type === 'float'
        : param === 'int'
          ? type === 'int'
          : type === 'bool';
    if (!fits) {
      throw errorAt(
        argument,
        `AOT: ${what}() takes ${param === 'int' ? 'an Int' : param === 'float' ? 'a number' : 'a boolean'} here`,
        param === 'int'
          ? 'pass a whole number, e.g. Math.round(value), Math.floor(value) or Math.trunc(value).'
          : param === 'bool'
            ? 'pass a boolean, e.g. value > 0.'
            : 'pass a number.',
      );
    }
    return lowered;
  });

  const nativeImport: NativeImport = {
    name: `${module.name}.${method}`,
    version: module.version,
    signature: signatureOf(member),
    params: member.params,
    result: member.result,
  };
  return {import: nativeImport, callArguments};
}

/**
 * The event a `useNativeEvent(Module, 'event', handler)` call subscribes to, checked against its module's interface.
 *
 * @param call  The call.
 * @param environment  The expression environment.
 *
 * @returns The event, and the handler with no more parameters than the event's values.
 */
export function nativeEventOf(
  call: t.CallExpression,
  environment: Env,
): {
  event: NativeEvent;
  handler: t.ArrowFunctionExpression | t.FunctionExpression;
} {
  const [moduleArgument, eventArgument, handler] = call.arguments;
  const usage =
    "subscribe as useNativeEvent(Sensors, 'reading', celsius => setTemperature(celsius)).";
  if (
    call.arguments.length !== 3 ||
    moduleArgument.type !== 'Identifier' ||
    !readsNativeModule(moduleArgument.name, environment)
  ) {
    throw errorAt(
      call,
      'AOT: useNativeEvent() takes a native module, an event name and a handler',
      usage,
    );
  }
  const module = environment.nativeModules!.get(moduleArgument.name)!;
  if (eventArgument.type !== 'StringLiteral') {
    throw errorAt(
      eventArgument,
      "AOT: useNativeEvent()'s event is named by a string literal",
      usage,
    );
  }
  const event = module.events.get(eventArgument.value);
  if (!event) {
    throw errorAt(
      eventArgument,
      `AOT: ${module.interfaceName} has no event ${eventArgument.value}`,
      `declare it in the interface: events: {${eventArgument.value}(…): void};`,
    );
  }
  if (event.kind === 'refused') {
    throw errorAt(
      eventArgument,
      `AOT: ${module.interfaceName}'s event ${eventArgument.value} ${event.reason}`,
      event.hint,
    );
  }
  if (
    handler.type !== 'ArrowFunctionExpression' &&
    handler.type !== 'FunctionExpression'
  ) {
    throw errorAt(
      handler,
      "AOT: useNativeEvent()'s handler must be an inline function",
      usage,
    );
  }
  if (handler.async || handler.generator) {
    throw errorAt(
      handler,
      "AOT: useNativeEvent()'s handler can't be async or a generator",
      'write a plain function; the firmware posts the event when its work is done.',
    );
  }
  if (handler.params.length > event.params.length) {
    throw errorAt(
      handler,
      `AOT: ${module.interfaceName}'s event ${eventArgument.value} has ${event.params.length} value` +
        `${event.params.length === 1 ? '' : 's'}, but the handler takes ${handler.params.length}`,
      'take one parameter for each value the interface declares, or fewer.',
    );
  }
  for (const parameter of handler.params) {
    if (parameter.type !== 'Identifier') {
      throw errorAt(
        parameter,
        "AOT: useNativeEvent()'s handler takes plain parameters",
        'name each value: (celsius, ok) => …',
      );
    }
  }
  return {
    event: {
      name: `${module.name}.${eventArgument.value}`,
      version: module.version,
      signature: signatureOf(event),
      params: event.params,
    },
    handler,
  };
}
