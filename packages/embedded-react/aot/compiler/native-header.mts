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
 * A C header for each native module a file declares, so the firmware's C is written against what apps compile
 * against: a struct per record with helpers that pack and unpack its cells, where each argument starts in a call's
 * or post's cells, and an entry per method and event with its name, version and signature. The modules are read as
 * the compiler reads them (see native-modules.mts).
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {codeOf, declaredModules, signatureOf} from './native-modules.mts';
import type {AotError} from './diagnostics.mts';
import type {NativeRecord, NativeType, NativeValue} from './ir/expressions.mts';
import type {NativeFile} from './native-files.mts';
import type {NativeMember, NativeModule} from './native-modules.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One module's header. */
export interface NativeHeader {
  /** The module's name. */
  module: string;
  /** The header's file name. */
  file: string;
  text: string;
}

/** A record's struct. */
interface Struct {
  /** The struct's type, ProbePoint. */
  type: string;
  /** What its macro and helpers are named after, as words. */
  words: string[];
  record: NativeRecord;
  /** Each field's struct, or null for a field that isn't a record. */
  fields: (Struct | null)[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The cells each type takes. */
const CELLS: Record<NativeType, number> = {
  int: 1,
  float: 1,
  bool: 1,
  int64: 2,
  string: 1,
};

const C_TYPES: Record<NativeType, string> = {
  int: 'int32_t',
  float: 'float',
  bool: 'bool',
  int64: 'int64_t',
  string: 'const char*',
};

/** Each type as the interface writes it, for the comments. */
const TYPESCRIPT_TYPES: Record<NativeType | 'void', string> = {
  int: 'Int',
  float: 'number',
  bool: 'boolean',
  int64: 'Int64',
  string: 'string',
  void: 'void',
};

/** The keywords of C and C++, and the names their headers define as keywords, which a struct's field can't have. */
const KEYWORDS = new Set(
  (
    'alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class ' +
    'compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype ' +
    'default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline ' +
    'int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public ' +
    'register reinterpret_cast requires restrict return short signed sizeof static static_assert static_cast ' +
    'struct switch template this thread_local throw true try typedef typeid typename typeof typeof_unqual union ' +
    'unsigned using virtual void volatile wchar_t while xor xor_eq'
  ).split(' '),
);

/** A name C or C++ keeps for itself: `_Atomic`, `__packed`, or one with `__` in it. */
const KEPT_NAME = /^_[A-Z]|__/;

const INDENT = '    ';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An error with how to fix it.
 *
 * @param message  What is wrong.
 * @param hint  How to fix it.
 *
 * @returns The error, for the caller to throw.
 */
function headerError(message: string, hint: string): AotError {
  const error: AotError = new Error(message);
  error.aotHint = hint;
  return error;
}

/**
 * A name's words: freeMemory and free_memory are both free, memory.
 *
 * @param name  The name.
 *
 * @returns Its words.
 */
const wordsOf = (name: string): string[] =>
  name
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2')
    .split('_')
    .filter(Boolean);

const macroOf = (words: string[]): string => words.join('_').toUpperCase();

const functionOf = (words: string[]): string => words.join('_').toLowerCase();

const typeOf = (words: string[]): string =>
  words.map(word => word[0].toUpperCase() + word.slice(1)).join('');

/**
 * How many cells a value takes.
 *
 * @param value  The value.
 *
 * @returns The count.
 */
const cellsOf = (value: NativeValue): number =>
  typeof value === 'string'
    ? CELLS[value]
    : value.fields.reduce((cells, field) => cells + cellsOf(field.type), 0);

/**
 * One module's header.
 *
 * @param module  The module.
 *
 * @returns The header.
 */
function headerOf(module: NativeModule): NativeHeader {
  /** A name's words, which every C name it is part of needs at least one of. */
  const namedWords = (name: string, what: string): string[] => {
    const words = wordsOf(name);
    if (!words.length) {
      throw headerError(
        `${module.interfaceName}: ${what} is named ${name}, which leaves nothing to name it by in the header`,
        'give it a name with a letter in it.',
      );
    }
    return words;
  };

  // Every name in the header starts with the module's, so it must start as a C name does.
  const moduleWords = namedWords(module.name, 'the module');
  if (/^[0-9]/.test(moduleWords[0])) {
    throw headerError(
      `${module.interfaceName}: the module is named ${module.name}, so its names in the header would start with a digit`,
      'start its name with a letter.',
    );
  }
  const structs: Struct[] = [];
  const taken = new Map<string, string>();

  /** Keeps a name the header defines, and refuses one defined twice. */
  const define = (name: string, what: string): string => {
    const first = taken.get(name);
    if (first) {
      throw headerError(
        `${module.interfaceName}: ${first} and ${what} would both be ${name} in the header`,
        'rename one of them.',
      );
    }
    taken.set(name, what);
    return name;
  };

  /** A record's struct, its records first. A record with no name is named after where it is. */
  const structOf = (record: NativeRecord, place: string[]): Struct => {
    const words = [
      ...moduleWords,
      ...(record.name ? namedWords(record.name, 'a record') : place),
    ];
    const type = typeOf(words);
    const known = structs.find(struct => struct.type === type);
    if (known && codeOf(known.record) === codeOf(record)) return known;
    const what = record.name ? `the record ${record.name}` : 'a record';
    const fields = record.fields.map(field => {
      if (KEYWORDS.has(field.name) || KEPT_NAME.test(field.name)) {
        throw headerError(
          `${module.interfaceName}: ${what} has a field named ${field.name}, which C keeps for itself`,
          'rename the field.',
        );
      }
      return typeof field.type === 'string'
        ? null
        : structOf(field.type, [
            ...words.slice(moduleWords.length),
            ...namedWords(field.name, `${what}'s field`),
          ]);
    });
    define(type, what);
    define(`${macroOf(words)}_CELLS`, what);
    define(`${functionOf(words)}_pack`, what);
    define(`${functionOf(words)}_unpack`, what);
    const struct = {type, words, record, fields};
    structs.push(struct);
    return struct;
  };

  /** A value's type in the comments, and its struct when it is a record. */
  const shownType = (value: NativeValue | 'void', place: string[]): string =>
    typeof value === 'string'
      ? TYPESCRIPT_TYPES[value]
      : structOf(value, place).type;

  /** A method's or event's macros. */
  const memberLines = (name: string, member: NativeMember): string[] => {
    if (member.kind === 'refused') {
      throw headerError(
        `${module.interfaceName}.${name} ${member.reason}`,
        member.hint,
      );
    }
    const what = `${name}()`;
    const memberWords = namedWords(name, 'a method or event');
    const words = [...moduleWords, ...memberWords];
    const argumentWords = member.names.map(argument =>
      namedWords(argument, `${what}'s argument`),
    );
    const parameters = member.params.map(
      (param, index) =>
        `${member.names[index]}: ${shownType(param, [...memberWords, ...argumentWords[index]])}`,
    );
    const lines = [
      `/* ${name}(${parameters.join(', ')}): ${shownType(member.result, [...memberWords, 'result'])} */`,
      `#define ${define(`${macroOf(words)}_ENTRY`, what)} ` +
        `"${module.name}.${name}", ${module.version}, "${signatureOf(member)}"`,
    ];
    let offset = 0;
    member.params.forEach((param, index) => {
      const macro = macroOf([...words, 'ARG', ...argumentWords[index]]);
      lines.push(
        `#define ${define(macro, `${what}'s ${member.names[index]}`)} ${offset}`,
      );
      offset += cellsOf(param);
    });
    if (member.params.length) {
      lines.push(
        `#define ${define(`${macroOf(words)}_ARG_CELLS`, `${what}'s cell count`)} ${offset}`,
      );
    }
    return lines;
  };

  const methods = [...module.members].map(([name, member]) =>
    memberLines(name, member),
  );
  const events = [...module.events].map(([name, member]) =>
    memberLines(name, member),
  );
  if (!methods.length && !events.length) {
    throw headerError(
      `${module.interfaceName} has no methods or events`,
      'declare what the firmware provides in its interface.',
    );
  }

  const guard = `${macroOf(moduleWords)}_MODULE_H`;
  const [first] = module.members.size ? module.members.keys() : [''];
  const [firstEvent] = module.events.size ? module.events.keys() : [''];
  const example = [
    first &&
      `{${macroOf([...moduleWords, ...wordsOf(first)])}_ENTRY, ${functionOf([...moduleWords, ...wordsOf(first)])}}`,
    firstEvent &&
      `{${macroOf([...moduleWords, ...wordsOf(firstEvent)])}_ENTRY, NULL} for an event`,
  ]
    .filter(Boolean)
    .join(', or ');
  const lines = [
    '/*',
    ` * The ${module.name} native module, version ${module.version}, generated by \`embedded-react native-header\`. Don't edit it:`,
    " * change the module's interface and generate it again.",
    ' *',
    " * Each method and event has an entry for the firmware's ErbNative table:",
    ` * ${example}.`,
    " * An _ARG_ macro is where that argument starts in a call's or post's cells, and a record's pack and unpack write",
    ' * and read its cells.',
    ' */',
    '',
    `#ifndef ${guard}`,
    `#define ${guard}`,
    '',
    '#include "erb_native.h"',
    '',
    '#include <stdbool.h>',
    '#include <stdint.h>',
  ];
  const section = (title: string, blocks: string[][]) => {
    if (!blocks.length) return;
    lines.push('', `/* ${title} */`);
    for (const block of blocks) {
      lines.push('', ...block);
    }
  };
  section('Records', structs.map(structLines));
  section('Methods', methods);
  section('Events', events);
  lines.push('', '#endif', '');

  return {
    module: module.name,
    file: `${functionOf(moduleWords)}_module.h`,
    text: lines.join('\n'),
  };
}

/**
 * A record's struct, cell count, and pack and unpack helpers.
 *
 * @param struct  The struct.
 *
 * @returns The lines.
 */
function structLines(struct: Struct): string[] {
  const pack: string[] = [];
  const unpack: string[] = [];
  const fields: string[] = [];
  let offset = 0;
  struct.record.fields.forEach(({name, type}, index) => {
    const cell = `cells[${offset}]`;
    const inner = struct.fields[index];
    if (typeof type !== 'string') {
      const helper = functionOf(inner!.words);
      pack.push(`${helper}_pack(&${cell}, &value->${name});`);
      unpack.push(`value.${name} = ${helper}_unpack(&${cell});`);
    } else if (type === 'int') {
      pack.push(`${cell} = value->${name};`);
      unpack.push(`value.${name} = ${cell};`);
    } else if (type === 'bool') {
      pack.push(`${cell} = value->${name} ? 1 : 0;`);
      unpack.push(`value.${name} = ${cell} != 0;`);
    } else if (type === 'float') {
      pack.push(`erb_native_set_float(&${cell}, value->${name});`);
      unpack.push(`value.${name} = erb_native_float(${cell});`);
    } else if (type === 'int64') {
      pack.push(`erb_native_set_int64(&${cell}, value->${name});`);
      unpack.push(`value.${name} = erb_native_int64(&${cell});`);
    } else {
      pack.push(`${cell} = erb_native_text(value->${name});`);
      unpack.push(`value.${name} = erb_native_string(${cell});`);
    }
    fields.push(
      `${typeof type === 'string' ? C_TYPES[type] : inner!.type} ${name};`,
    );
    offset += cellsOf(type);
  });
  const indent = (lines: string[]) => lines.map(line => INDENT + line);
  const helper = functionOf(struct.words);
  return [
    'typedef struct',
    '{',
    ...indent(fields),
    `} ${struct.type};`,
    '',
    `#define ${macroOf(struct.words)}_CELLS ${offset}`,
    '',
    `static inline void ${helper}_pack(int32_t* cells, const ${struct.type}* value)`,
    '{',
    ...indent(pack),
    '}',
    '',
    `static inline ${struct.type} ${helper}_unpack(const int32_t* cells)`,
    '{',
    ...indent([`${struct.type} value;`, ...unpack, 'return value;']),
    '}',
  ];
}

/**
 * A C header for each native module a file declares.
 *
 * @param source  The file's source.
 * @param files  The local files it imports a module's interface from (see readModuleFile).
 *
 * @returns Each module's header, in the order the file declares them.
 */
export function nativeHeaders(
  source: string,
  files: Record<string, NativeFile> = {},
): NativeHeader[] {
  const modules = declaredModules(source, files);
  if (!modules.length) {
    throw headerError(
      'The file declares no native modules',
      "pass the file that declares them: export const Can = nativeModule<Can>('can', 1);",
    );
  }
  const headers = modules.map(headerOf);
  for (const header of headers) {
    const twice = headers.find(
      other => other !== header && other.file === header.file,
    );
    if (twice) {
      throw headerError(
        `The modules "${header.module}" and "${twice.module}" would both be written to ${header.file}`,
        'rename one of them.',
      );
    }
  }
  return headers;
}
