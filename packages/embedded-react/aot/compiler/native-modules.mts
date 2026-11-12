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
 * the app's file or in a local .ts file the CLI reads. An app may import a module declared in another file or a
 * package instead, `import {Can} from 'my-device'`, which the CLI reads too (see native-files.mts). Each method called becomes an import named
 * `module.method`, and each call a CALLN. The interface's `events: {reading(celsius: number): void}` are what the
 * firmware posts; each useNativeEvent becomes a handler the runtime runs with the event's values. In Flow A the
 * bundler passes nativeModule() each method's and event's signature instead, and in the simulators the module's mock
 * too.
 *
 * `nativeModule.optional<Wifi>('wifi', 1)` is a module only some boards have. `Wifi.available` says whether the
 * firmware has every method and event of it; the package lists them all, so both flows agree.
 *
 * A record, an interface or object type of fields declared beside the module's interface, passes as its fields one by
 * one, in order; its signature names them, so a renamed or reordered field doesn't load.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {parse} from '@babel/parser';
import {aotError} from './diagnostics.mts';
import {buildsText, lowerExpr, lowerExprWide} from './expressions.mts';
import {flattenTree, leafNode, objectTreeOf} from './records.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {
  IrExpr,
  IrNativeCall,
  NativeImport,
  NativeRecord,
  NativeType,
  NativeValue,
} from './ir/expressions.mts';
import {nativeAvailableSlot} from './boot.mts';
import {BOOL} from './ir/types.mts';
import type {IrOptionalModule, NativeEvent} from './ir/functions.mts';
import type {NativeFile} from './native-files.mts';
import type {Env} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A method of a native module's interface, or why it can't be called yet. */
type NativeMember =
  | {kind: 'method'; params: NativeValue[]; result: NativeValue | 'void'}
  | {kind: 'refused'; reason: string; hint: string};

/** The members of a record type the interface's file declares, by its name (see declaredMembers). */
type RecordScope = (name: string) => t.TSTypeElement[] | string | null;

/** One of a value's values: a record's field by its path, or the value itself at ''. */
interface NativeLeaf {
  path: string;
  type: NativeType;
}

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
  /** Its index among the optional modules, or null when the firmware must have it. */
  optional: number | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Native methods a package may import: the runtime's ERB_MAX_IMPORTS. */
export const MAX_IMPORTS = 32;

const MAX_VERSION = 0xffff;

/** Values a native method may take, and return, a record's fields counted one by one: ERB_NATIVE_MAX_PARAMETERS. */
const MAX_PARAMETERS = 16;

/** What a record's field may be named, as a signature names it. */
const FIELD_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

const SIGNATURE_CODES: Record<NativeType | 'void', string> = {
  int: 'i',
  float: 'f',
  bool: 'b',
  int64: 'l',
  string: 's',
  void: 'v',
};

/** The types a native method takes and returns, as embedded-react exports them. */
const WHOLE_TYPES: Record<string, NativeType> = {Int: 'int', Int64: 'int64'};

/** The types of value each parameter takes: a whole number may be passed as a number or an Int64. */
const ARGUMENT_TYPES: Record<NativeType, string[]> = {
  int: ['int'],
  float: ['int', 'float'],
  bool: ['bool'],
  int64: ['int', 'int64'],
  string: ['string'],
};

/** What each parameter takes, and how to pass one, for errors. */
const ARGUMENT_HINTS: Record<NativeType, {what: string; hint: string}> = {
  int: {
    what: 'an Int',
    hint: 'pass a whole number, e.g. Math.round(value), Math.floor(value) or Math.trunc(value).',
  },
  float: {what: 'a number', hint: 'pass a number.'},
  bool: {what: 'a boolean', hint: 'pass a boolean, e.g. value > 0.'},
  int64: {
    what: 'an Int64',
    hint: 'pass a whole number, e.g. Date.now() or Math.trunc(value).',
  },
  string: {
    what: 'a string',
    hint: 'build text in a const first, and pass that: const label = `${count} items`;',
  },
};

const DECLARATION_HINT =
  "declare it at module level: const Sensors = nativeModule<Sensors>('sensors', 1)";

/** Optional modules a package may have: the runtime's ERB_MAX_OPTIONAL_MODULES. */
const MAX_OPTIONAL_MODULES = 32;

const EVENTS_HINT =
  'declare the events as an object of methods that return void: events: {reading(celsius: number): void};';

/** useNativeEvents a package may have, counting each list row; so no more events than the runtime's ERB_MAX_NATIVE_EVENTS. */
export const MAX_NATIVE_EVENTS = 32;

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
export function declarationsOf(program: t.Program): t.VariableDeclarator[] {
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
  ((node.callee.type === 'Identifier' && node.callee.name === 'nativeModule') ||
    isOptionalCallee(node.callee));

/**
 * Whether a callee is `nativeModule.optional`.
 *
 * @param callee  The callee.
 *
 * @returns True when it is.
 */
const isOptionalCallee = (callee: t.Node): boolean =>
  callee.type === 'MemberExpression' &&
  !callee.computed &&
  callee.object.type === 'Identifier' &&
  callee.object.name === 'nativeModule' &&
  callee.property.type === 'Identifier' &&
  callee.property.name === 'optional';

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
export function declaredMembers(
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
export function localImportOf(
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
 * The type a parameter, result or field is passed as, or what it is that can't be.
 *
 * @param type  The TypeScript type.
 * @param isResult  It is the result, which may be void.
 * @param scope  The record types the interface's file declares.
 * @param seen  The records it is part of, which it can't hold.
 *
 * @returns The native type, or what it is, as in "takes …".
 */
function nativeValueOf(
  type: t.TSType,
  isResult: boolean,
  scope: RecordScope,
  seen: ReadonlySet<string> = new Set(),
): NativeValue | 'void' | {what: string} {
  switch (type.type) {
    case 'TSNumberKeyword':
      return 'float';
    case 'TSBooleanKeyword':
      return 'bool';
    case 'TSStringKeyword':
      return 'string';
    case 'TSVoidKeyword':
      return isResult ? 'void' : {what: 'void'};
    case 'TSTypeLiteral':
      return recordOf(type.members, scope, seen);
    case 'TSTypeReference': {
      if (type.typeName.type !== 'Identifier' || type.typeParameters) break;
      const {name} = type.typeName;
      if (Object.hasOwn(WHOLE_TYPES, name)) {
        return WHOLE_TYPES[name];
      }
      const members = scope(name);
      if (typeof members === 'string') {
        return {what: `${name}, which isn't a plain interface or object type`};
      }
      if (!members) break;
      if (seen.has(name)) {
        return {what: `the record ${name}, which holds itself`};
      }
      return recordOf(members, scope, new Set(seen).add(name));
    }
  }
  return {what: "a type native modules don't pass"};
}

/**
 * A record: an interface or object type whose fields are each a value native modules pass, required, and named as a
 * signature names them.
 *
 * @param members  Its members.
 * @param scope  The record types the interface's file declares.
 * @param seen  The records it is part of.
 *
 * @returns The record, or what it is that can't be.
 */
function recordOf(
  members: t.TSTypeElement[],
  scope: RecordScope,
  seen: ReadonlySet<string>,
): NativeRecord | {what: string} {
  const record: NativeRecord = {kind: 'record', fields: []};
  for (const member of members) {
    if (
      member.type !== 'TSPropertySignature' ||
      member.computed ||
      (member.key.type !== 'Identifier' && member.key.type !== 'StringLiteral')
    ) {
      return {what: 'a record with a member that is not a plain field'};
    }
    const name =
      member.key.type === 'Identifier' ? member.key.name : member.key.value;
    if (!FIELD_NAME.test(name) || name === '__proto__') {
      return {
        what: `a record with a field named ${name}, which a native signature can't name`,
      };
    }
    if (record.fields.some(field => field.name === name)) {
      return {what: `a record that declares ${name} twice`};
    }
    if (member.optional) {
      return {what: `a record whose field ${name} is optional`};
    }
    const fieldType = member.typeAnnotation?.typeAnnotation;
    if (!fieldType) {
      return {what: `a record whose field ${name} has no type`};
    }
    const type = nativeValueOf(fieldType, false, scope, seen);
    if (typeof type === 'object' && !('kind' in type)) {
      return {what: `a record whose field ${name} is ${type.what}`};
    }
    record.fields.push({name, type: type as NativeValue});
  }
  return record.fields.length ? record : {what: 'an empty record'};
}

/**
 * A value's values one by one, a record's fields in order, each by its path.
 *
 * @param value  The value.
 * @param path  Its path, '' at the top.
 *
 * @returns Its values.
 */
const leavesOf = (value: NativeValue, path = ''): NativeLeaf[] =>
  typeof value === 'string'
    ? [{path, type: value}]
    : value.fields.flatMap(field =>
        leavesOf(field.type, path ? `${path}.${field.name}` : field.name),
      );

/**
 * One member of a module's interface: a method whose parameters and result are Int, Int64, number, boolean, string,
 * or records of them.
 *
 * @param member  The interface member.
 * @param scope  The record types the interface's file declares.
 *
 * @returns Its name and what it is, or null for a member without a plain name.
 */
function memberOf(
  member: t.TSTypeElement,
  scope: RecordScope,
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
    'a native method takes and returns Int and Int64 (from embedded-react), number, boolean, string and records of ' +
    'them, and may return void. A record is an interface or object type of required fields, declared beside the ' +
    "module's: interface Fix { lat: number; lon: number; }";

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

  const params: NativeValue[] = [];
  for (const parameter of functionType.parameters) {
    if (parameter.type !== 'Identifier' || parameter.optional) {
      return refuse(
        'has an optional, rest or destructured parameter',
        'give each parameter a name and a type: read(channel: Int): number;',
      );
    }
    if (parameter.typeAnnotation?.type !== 'TSTypeAnnotation') {
      return refuse('has no parameter type', typeHint);
    }
    const type = nativeValueOf(
      parameter.typeAnnotation.typeAnnotation,
      false,
      scope,
    );
    if (typeof type === 'object' && !('kind' in type)) {
      return refuse(`takes ${type.what}`, typeHint);
    }
    params.push(type as NativeValue);
  }
  const resultType = functionType.typeAnnotation?.typeAnnotation;
  if (!resultType) {
    return refuse('has no result type', typeHint);
  }
  const result = nativeValueOf(resultType, true, scope);
  if (typeof result === 'object' && !('kind' in result)) {
    return refuse(`returns ${result.what}`, typeHint);
  }

  if (params.flatMap(param => leavesOf(param)).length > MAX_PARAMETERS) {
    return refuse(
      `takes more than ${MAX_PARAMETERS} values, counting each field of a record`,
      'split it into methods that take fewer.',
    );
  }
  if (
    result !== 'void' &&
    leavesOf(result as NativeValue).length > MAX_PARAMETERS
  ) {
    return refuse(
      `returns more than ${MAX_PARAMETERS} values, counting each field of its record`,
      'split it into methods that return fewer.',
    );
  }
  return {
    name,
    member: {kind: 'method', params, result: result as NativeValue | 'void'},
  };
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
 * @param scope  The record types the interface's file declares.
 *
 * @returns Each event, or why it can't be used.
 */
function eventsOf(
  member: t.TSTypeElement,
  call: t.CallExpression,
  interfaceName: string,
  scope: RecordScope,
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
    const event = memberOf(element, scope);
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
 * The native modules a file declares, by the const each is bound to, before they are numbered (see
 * collectNativeModules): an optional module's index is 0.
 *
 * @param program  The file's program, types and all.
 * @param files  The local files it imports a module's interface from, by import path (see readNativeFiles).
 *
 * @returns Each module, and the call that declares it.
 */
function declaredIn(
  program: t.Program,
  files: Record<string, NativeFile>,
): Map<string, {module: NativeModule; call: t.CallExpression}> {
  const modules = new Map<
    string,
    {module: NativeModule; call: t.CallExpression}
  >();
  const declarators = declarationsOf(program);
  refuseInnerDeclarations(
    program.body,
    new Set(declarators.map(declarator => declarator.init!)),
  );
  for (const declarator of declarators) {
    const call = declarator.init as t.CallExpression;
    const interfaceName = interfaceNameOf(call);
    const {name, version} = nameAndVersionOf(call);

    // The interface, in this file or in the local file the app imports it from, and the records that file declares.
    let members = declaredMembers(program, interfaceName);
    let scope: RecordScope = name => declaredMembers(program, name);
    if (members === null) {
      const local = localImportOf(program, interfaceName);
      const source = local ? files[local.path]?.source : undefined;
      if (!local || source == null) {
        throw errorAt(
          call,
          `AOT: can't find the interface ${interfaceName}`,
          'declare it in this file, or in a local .ts file the app imports it from: ' +
            "import type {Sensors} from './sensors';",
        );
      }
      const interfaceFile = parseInterfaceFile(source);
      members = declaredMembers(interfaceFile, local.exported);
      scope = name => declaredMembers(interfaceFile, name);
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

    const optional = isOptionalCallee(call.callee);
    const module: NativeModule = {
      name,
      version,
      interfaceName,
      members: new Map(),
      events: new Map(),
      optional: optional ? 0 : null,
    };
    for (const element of members) {
      if (isEventsMember(element)) {
        module.events = eventsOf(element, call, interfaceName, scope);
        continue;
      }
      const member = memberOf(element, scope);
      if (!member) continue;
      if (optional && member.name === 'available') {
        throw errorAt(
          call,
          `AOT: ${interfaceName} has a method named available, which an optional native module can't have`,
          'rename it; Wifi.available says whether the firmware has the module.',
        );
      }
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
    const usable = [...module.members.values(), ...module.events.values()];
    if (optional && !usable.some(member => member.kind === 'method')) {
      throw errorAt(
        call,
        `AOT: the optional native module "${name}" has no methods or events to look for`,
        'declare what the firmware provides in its interface; whether the firmware has them all is what ' +
          'available says.',
      );
    }
    modules.set((declarator.id as t.Identifier).name, {module, call});
  }
  return modules;
}

/**
 * The const a file exports under a name: `export const Can = …`, or `export {can as Can}`.
 *
 * @param program  The file.
 * @param exported  The name it is imported by.
 *
 * @returns The const's name, 'reexport' when the file may pass it on from another file, or null.
 */
function exportedConstOf(
  program: t.Program,
  exported: string,
): string | 'reexport' | null {
  let reexports = false;
  for (const statement of program.body) {
    if (
      statement.type === 'ExportAllDeclaration' &&
      statement.exportKind !== 'type'
    ) {
      reexports = true;
    }
    if (
      statement.type !== 'ExportNamedDeclaration' ||
      statement.exportKind === 'type'
    ) {
      continue;
    }
    const declaration = statement.declaration;
    if (declaration?.type === 'VariableDeclaration') {
      const declares = declaration.declarations.some(
        declarator =>
          declarator.id.type === 'Identifier' &&
          declarator.id.name === exported,
      );
      if (declares) return exported;
    }
    for (const specifier of statement.specifiers) {
      if (
        specifier.type !== 'ExportSpecifier' ||
        specifier.exportKind === 'type'
      ) {
        continue;
      }
      const name =
        specifier.exported.type === 'Identifier'
          ? specifier.exported.name
          : specifier.exported.value;
      if (name !== exported) continue;
      return statement.source ? 'reexport' : specifier.local.name;
    }
  }
  return reexports ? 'reexport' : null;
}

/**
 * Pins an error from another file's module to the app's import of it, and says where in that file it is.
 *
 * @param error  The thrown value.
 * @param file  The file, as the app imports it.
 * @param at  The app's import.
 *
 * @returns The error, for the caller to throw.
 */
function importedError(error: unknown, file: NativeFile, at: t.Node): unknown {
  const aotError = error as AotError;
  if (!aotError?.message?.startsWith('AOT:')) return error;
  const where = aotError.aotLoc
    ? `${file.path}:${aotError.aotLoc.line}:${aotError.aotLoc.column + 1}`
    : file.path;
  aotError.message += ` (in ${where})`;
  aotError.aotLoc = at.loc?.start;
  return aotError;
}

/**
 * The modules an app imports from other files, by the name it imports each as: the file, or the package's, declares
 * the module.
 *
 * @param program  The app's program.
 * @param files  The files it imports from (see readNativeFiles).
 *
 * @returns Each module, where the app imports it, and where it is declared.
 */
function importedIn(
  program: t.Program,
  files: Record<string, NativeFile>,
): {binding: string; module: NativeModule; at: t.Node; origin: string}[] {
  const found: {
    binding: string;
    module: NativeModule;
    at: t.Node;
    origin: string;
  }[] = [];
  for (const statement of program.body) {
    if (
      statement.type !== 'ImportDeclaration' ||
      statement.importKind === 'type' ||
      !Object.hasOwn(files, statement.source.value)
    ) {
      continue;
    }
    const importPath = statement.source.value;
    const file = files[importPath];
    const specifiers = statement.specifiers.filter(
      specifier =>
        specifier.type === 'ImportSpecifier' && specifier.importKind !== 'type',
    ) as t.ImportSpecifier[];
    if (!specifiers.length) continue;
    if (file.source === null) {
      throw errorAt(
        statement.source,
        `AOT: ${importPath} is JavaScript or a .d.ts, which a native module isn't read from (${file.path})`,
        'ship the package\'s .ts source, and point its exports at it: "exports": "./index.ts".',
      );
    }
    let fileProgram: t.Program;
    let declared: ReturnType<typeof declaredIn>;
    try {
      fileProgram = parseInterfaceFile(file.source);
      declared = declaredIn(fileProgram, file.imports);
    } catch (error) {
      throw importedError(error, file, statement.source);
    }
    for (const specifier of specifiers) {
      const imported =
        specifier.imported.type === 'Identifier'
          ? specifier.imported.name
          : specifier.imported.value;
      const local = exportedConstOf(fileProgram, imported);
      if (local === 'reexport') {
        throw errorAt(
          specifier,
          `AOT: ${importPath} passes ${imported} on from another file, and a native module isn't followed further`,
          'declare the module in the file the app imports, or import it from the file that declares it.',
        );
      }
      const entry = local === null ? undefined : declared.get(local);
      if (!entry) continue;
      found.push({
        binding: specifier.local.name,
        module: entry.module,
        at: specifier,
        origin: `${file.path}:${entry.call.start}`,
      });
    }
  }
  return found;
}

/**
 * The native modules an app declares or imports, by the const each is bound to. Run on the app before its types are
 * stripped. Optional modules are numbered in order, the app's own first.
 *
 * @param program  The app's program, types and all.
 * @param files  The files the app imports a module or its interface from, by import path (see readNativeFiles).
 *
 * @returns The modules.
 */
export function collectNativeModules(
  program: t.Program,
  files: Record<string, NativeFile> = {},
): Map<string, NativeModule> {
  const found = [
    ...[...declaredIn(program, files)].map(([binding, {module, call}]) => ({
      binding,
      module,
      at: call as t.Node,
      origin: `:${call.start}`,
    })),
    ...importedIn(program, files),
  ];
  const modules = new Map<string, NativeModule>();
  const byName = new Map<string, {module: NativeModule; origin: string}>();
  let optionalCount = 0;
  for (const {binding, module, at, origin} of found) {
    // The same declaration imported twice is one module.
    const first = byName.get(module.name);
    if (first) {
      if (first.origin !== origin) {
        throw errorAt(
          at,
          `AOT: the native module "${module.name}" is declared twice`,
          'declare each module once, and call its methods through that const.',
        );
      }
      modules.set(binding, first.module);
      continue;
    }
    if (module.optional !== null) {
      if (optionalCount === MAX_OPTIONAL_MODULES) {
        throw errorAt(
          at,
          `AOT: an app may declare at most ${MAX_OPTIONAL_MODULES} optional native modules`,
          'declare fewer optional modules, or merge ones that boards always have together.',
        );
      }
      module.optional = optionalCount++;
    }
    byName.set(module.name, {module, origin});
    modules.set(binding, module);
  }
  return modules;
}

/**
 * A value's code in a signature: a record names its fields in order, {x:i,y:i}.
 *
 * @param value  The value.
 *
 * @returns The code.
 */
const codeOf = (value: NativeValue | 'void'): string =>
  typeof value === 'string'
    ? SIGNATURE_CODES[value]
    : `{${value.fields.map(field => `${field.name}:${codeOf(field.type)}`).join(',')}}`;

/**
 * A method's signature: its result, ':', then its parameters.
 *
 * @param member  The method.
 *
 * @returns The signature, such as "i:if".
 */
const signatureOf = (member: Extract<NativeMember, {kind: 'method'}>): string =>
  codeOf(member.result) + ':' + member.params.map(codeOf).join('');

/**
 * The optional modules, in order, each with every method and event of its interface that can be used: the package
 * lists them all, called or not, so that whether the module is there means what it does in Flow A.
 *
 * @param modules  The app's modules (see collectNativeModules).
 *
 * @returns The optional modules, by index.
 */
export function optionalModulesOf(
  modules: Map<string, NativeModule>,
): IrOptionalModule[] {
  const optional = [...modules.values()]
    .filter(module => module.optional !== null)
    .sort((first, second) => first.optional! - second.optional!);
  return optional.map(module => ({
    name: module.name,
    methods: [...module.members]
      .filter(([, member]) => member.kind === 'method')
      .map(([method]) => nativeImportOf(module, method)),
    events: [...module.events]
      .filter(([, event]) => event.kind === 'method')
      .map(([event]) => nativeEventFor(module, event)),
  }));
}

/**
 * A method's import, as the firmware registers it.
 *
 * @param module  Its module.
 * @param method  A method the interface declares, that can be called.
 *
 * @returns The import.
 */
function nativeImportOf(module: NativeModule, method: string): NativeImport {
  const member = module.members.get(method) as Extract<
    NativeMember,
    {kind: 'method'}
  >;
  const {result} = member;
  return {
    name: `${module.name}.${method}`,
    version: module.version,
    signature: signatureOf(member),
    params: member.params.flatMap(param =>
      leavesOf(param).map(leaf => leaf.type),
    ),
    result: typeof result === 'string' ? result : 'record',
    results: result === 'void' ? [] : leavesOf(result).map(leaf => leaf.type),
    module: module.optional,
  };
}

/**
 * An event, as the firmware registers it.
 *
 * @param module  Its module.
 * @param event  An event the interface declares, that can be used.
 *
 * @returns The event.
 */
function nativeEventFor(module: NativeModule, event: string): NativeEvent {
  const member = module.events.get(event) as Extract<
    NativeMember,
    {kind: 'method'}
  >;
  return {
    name: `${module.name}.${event}`,
    version: module.version,
    signature: signatureOf(member),
    params: member.params.flatMap(param =>
      leavesOf(param).map(leaf => leaf.type),
    ),
    module: module.optional,
  };
}

/**
 * Flow A's side: since the bundle has no types, each nativeModule() call is given its methods' and events' signatures
 * as a third argument, `{methods, events}`, read from its interface as the compiler reads it. One that can't be used
 * gets the reason instead.
 * With mocks, for the simulators, each call is also given its module's mock, or null when it has none.
 *
 * @param source  An app file's source.
 * @param files  The files it imports a module or its interface from (see readNativeFiles).
 * @param mocks  Each module's mock as the file imports it, or null when it has none, by module name.
 *
 * @returns The source with the signatures added.
 */
export function addNativeSignatures(
  source: string,
  files: Record<string, NativeFile>,
  mocks?: Map<string, string | null>,
): string {
  const program = parse(source, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript'],
  }).program;
  const modules = collectNativeModules(program, files);
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

  // Each argument must already be of its parameter's type, except a whole number passed as a number. A record's
  // fields are passed one by one, each read from the object given by its name.
  const callArguments = node.arguments.flatMap((argument, index) => {
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
    const param = member.params[index];
    if (typeof param === 'string') {
      return [lowerArgument(argument, param, environment, what, null)];
    }
    const tree = objectTreeOf(argument, environment);
    const leaves = leavesOf(param);
    if (!tree) {
      throw errorAt(
        argument,
        `AOT: ${what}() takes a record here`,
        `pass an object of its fields: {${leaves.map(leaf => leaf.path).join(', ')}}.`,
      );
    }
    const fields = flattenTree(tree);
    return leaves.map(leaf => {
      if (!fields.has(leaf.path)) {
        throw errorAt(
          argument,
          `AOT: the record ${what}() takes has a field ${leaf.path}, which this object doesn't`,
          'give the object every field the interface declares.',
        );
      }
      const field = leafNode(fields.get(leaf.path), environment);
      return lowerArgument(
        field.node as t.Expression,
        leaf.type,
        field.environment,
        what,
        leaf.path,
        argument,
      );
    });
  });

  return {import: nativeImportOf(module, method), callArguments};
}

/**
 * Lowers one value a native method is given, checked against its type.
 *
 * @param argument  The value.
 * @param param  Its type.
 * @param environment  The expression environment.
 * @param what  The method, for errors: `Sensors.read`.
 * @param field  The record's field it is, or null for an argument.
 * @param at  Where an error points, when not at the value: the record a field is part of.
 *
 * @returns The value.
 */
function lowerArgument(
  argument: t.Expression,
  param: NativeType,
  environment: Env,
  what: string,
  field: string | null,
  at: t.Node = argument,
): IrExpr {
  // Built text needs a buffer to be built in, which a const has.
  if (param === 'string' && buildsText(argument, environment)) {
    throw errorAt(
      at,
      `AOT: ${what}() takes a string${field === null ? '' : ` as ${field}`}, not text built in the call`,
      ARGUMENT_HINTS.string.hint,
    );
  }
  // Only an Int64 parameter holds a 64-bit value whole.
  const lowered =
    param === 'int64'
      ? lowerExprWide(argument, environment)
      : lowerExpr(argument, environment);
  if (!ARGUMENT_TYPES[param].includes(lowered.type.kind)) {
    throw errorAt(
      at,
      `AOT: ${what}() takes ${ARGUMENT_HINTS[param].what} ${field === null ? 'here' : `as ${field}`}`,
      ARGUMENT_HINTS[param].hint,
    );
  }
  return lowered;
}

/**
 * The native method a call calls, when it returns a record: a const or an object state keeps it, as JS would.
 *
 * @param node  The expression.
 * @param environment  The expression environment.
 *
 * @returns The call's module, method and record, or null.
 */
export function nativeRecordCallOf(
  node: t.Node | null | undefined,
  environment: Env,
): {module: NativeModule; method: string; record: NativeRecord} | null {
  if (node?.type !== 'CallExpression') return null;
  const callee = nativeCalleeOf(node, environment);
  const member = callee?.module.members.get(callee.method);
  return member?.kind === 'method' && typeof member.result === 'object'
    ? {...callee!, record: member.result}
    : null;
}

/**
 * Refuses a record a native method returns where it can't be kept: anywhere but a const, an object state's setter, a
 * native method's record parameter, or a field read of one of those.
 *
 * @param node  The call.
 * @param environment  The expression environment.
 *
 * @returns The error, or null when the call returns no record.
 */
export function unkeptRecordError(
  node: t.Node,
  environment: Env,
): AotError | null {
  const callee = nativeRecordCallOf(node, environment);
  if (!callee) return null;
  const binding = ((node as t.CallExpression).callee as t.MemberExpression)
    .object as t.Identifier;
  const what = `${binding.name}.${callee.method}`;
  return errorAt(
    node,
    `AOT: ${what}() returns a record, which only a const or an object state keeps`,
    `keep it first, then read its fields: const fix = ${what}(…); or setFix(${what}(…));`,
  );
}

/**
 * How many values a value is, a record's fields counted one by one.
 *
 * @param value  The value.
 *
 * @returns The count.
 */
export const valueCount = (value: NativeValue): number =>
  leavesOf(value).length;

/**
 * Where each of a record's values is, by its path: a tree of `read(index, type)`, in field order.
 *
 * @param record  The record.
 * @param read  Each value, by its index among the record's values.
 * @param from  The index of the record's first value.
 *
 * @returns The tree.
 */
export function recordTreeOf<Leaf>(
  record: NativeRecord,
  read: (index: number, type: NativeType) => Leaf,
  from = 0,
): Record<string, unknown> {
  const tree: Record<string, unknown> = {};
  let index = from;
  for (const field of record.fields) {
    if (typeof field.type === 'string') {
      tree[field.name] = read(index++, field.type);
    } else {
      tree[field.name] = recordTreeOf(field.type, read, index);
      index += valueCount(field.type);
    }
  }
  return tree;
}

/**
 * `Wifi.available`: whether the firmware has every method and event of an optional module. Boot reads it, and it
 * never changes after.
 *
 * @param node  A member read.
 * @param environment  The expression environment.
 *
 * @returns The read, or null when the node isn't a native module's `available`.
 */
export function nativeAvailableOf(
  node: t.MemberExpression,
  environment: Env,
): IrExpr | null {
  if (
    node.computed ||
    node.object.type !== 'Identifier' ||
    node.property.type !== 'Identifier' ||
    node.property.name !== 'available' ||
    !readsNativeModule(node.object.name, environment)
  ) {
    return null;
  }
  const module = environment.nativeModules!.get(node.object.name)!;
  if (module.optional === null) {
    throw errorAt(
      node,
      `AOT: ${node.object.name} is always available, since the firmware must have it`,
      `declare it with nativeModule.optional<${module.interfaceName}>('${module.name}', ${module.version}) ` +
        "if some boards don't have it.",
    );
  }
  return {
    kind: 'boot',
    slot: nativeAvailableSlot(module.optional, module.name),
    type: BOOL,
  };
}

/**
 * The event a `useNativeEvent(Module, 'event', handler)` call subscribes to, checked against its module's interface.
 *
 * @param call  The call.
 * @param environment  The expression environment.
 *
 * @returns The event and its values, and the handler with no more parameters than the event's values.
 */
export function nativeEventOf(
  call: t.CallExpression,
  environment: Env,
): {
  event: NativeEvent;
  /** The event's values, as the interface declares them. */
  values: NativeValue[];
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
  handler.params.forEach((parameter, position) => {
    // A record may be destructured, as JS would.
    if (
      parameter.type !== 'Identifier' &&
      (parameter.type !== 'ObjectPattern' ||
        typeof event.params[position] === 'string')
    ) {
      throw errorAt(
        parameter,
        "AOT: useNativeEvent()'s handler takes plain parameters",
        'name each value: (celsius, ok) => …, or destructure a record: ({lat, lon}) => …',
      );
    }
  });
  return {
    event: nativeEventFor(module, eventArgument.value),
    values: event.params,
    handler,
  };
}
