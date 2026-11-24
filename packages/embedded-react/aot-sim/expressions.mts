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

// The expressions the simulator's debugger reads, as gdb/erb.py reads them on a board: names, members and indexes,
// literals and operators, over a frame's locals and the app's state. Nothing in one runs the app's code or changes it.

import type {
  DebugLocal,
  DebugState,
} from '../aot/compiler/package/debug-file.mts';
import {
  App,
  DebugError,
  Fields,
  Place,
  Row,
  Rows,
  StatePlace,
  Unreadable,
  frameLocals,
  isStructured,
  localsTree,
  settled,
  show,
  stateLabels,
  stateTree,
  typeName,
} from './values.mts';
import type {Found, Value} from './values.mts';

const TOKEN =
  /\s*(?:(?<number>0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)|(?<name>[A-Za-z_$][\w$]*)|(?<string>"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')|(?<operator>>>>=|\.\.\.|===|!==|\*\*=|<<=|>>=|>>>|&&=|\|\|=|\?\?=|=>|==|!=|<=|>=|&&|\|\||\?\?|\?\.|\*\*|\+\+|--|<<|>>|[-+*/%&|^]=|[-+*/%<>=!&|^~?:.,()[\]{}`]))/y;

/** Each binary operator's precedence: a higher one binds tighter. */
const BINARY: Record<string, number> = {
  '??': 1,
  '||': 2,
  '&&': 3,
  '|': 4,
  '^': 5,
  '&': 6,
  '==': 7,
  '!=': 7,
  '===': 7,
  '!==': 7,
  '<': 8,
  '<=': 8,
  '>': 8,
  '>=': 8,
  '<<': 9,
  '>>': 9,
  '>>>': 9,
  '+': 10,
  '-': 10,
  '*': 11,
  '/': 11,
  '%': 11,
  '**': 12,
};
const ASSIGNMENTS = new Set([
  '=',
  '+=',
  '-=',
  '*=',
  '/=',
  '%=',
  '**=',
  '<<=',
  '>>=',
  '>>>=',
  '&=',
  '|=',
  '^=',
  '&&=',
  '||=',
  '??=',
  '++',
  '--',
]);
const CONSTANTS = new Map<string, Value>([
  ['true', true],
  ['false', false],
  ['null', null],
  ['undefined', undefined],
  ['NaN', NaN],
  ['Infinity', Infinity],
]);
const REFUSED_WORDS = new Set([
  'new',
  'delete',
  'void',
  'await',
  'yield',
  'function',
  'class',
  'this',
  'super',
  'async',
  'in',
  'instanceof',
]);
const ESCAPES: Record<string, string> = {
  n: '\n',
  t: '\t',
  r: '\r',
  b: '\b',
  f: '\f',
  v: '\v',
  0: '\0',
};

type TokenKind = 'number' | 'name' | 'string' | 'operator';

interface Token {
  kind: TokenKind | null;
  text: string | null;
  start: number;
  end: number;
}

/** A part of a parsed expression, with its source text for an error to quote. */
export type Node =
  | {kind: 'literal'; text: string; value: Value}
  | {kind: 'name'; text: string; name: string}
  | {kind: 'unary'; text: string; operator: string; operand: Node}
  | {kind: 'binary'; text: string; operator: string; left: Node; right: Node}
  | {kind: 'conditional'; text: string; test: Node; then: Node; otherwise: Node}
  | {
      kind: 'member';
      text: string;
      target: Node;
      name: string;
      optional: boolean;
    }
  | {kind: 'index'; text: string; target: Node; index: Node; optional: boolean};

/** Parses an expression. Whatever would run the app's code or change it is refused by name. */
class Parser {
  private readonly source: string;
  private readonly tokens: Token[] = [];
  private at = 0;
  private lastEnd = 0;

  constructor(source: string) {
    this.source = source.trim();
    let position = 0;
    while (position < this.source.length) {
      TOKEN.lastIndex = position;
      const match = TOKEN.exec(this.source);
      if (!match?.groups) {
        throw this.broken(
          `\`${this.source.slice(position).trimStart()[0]}\` isn't expected there`,
        );
      }
      const kind = (['number', 'name', 'string', 'operator'] as const).find(
        group => match.groups![group] !== undefined,
      )!;
      const text = match.groups[kind];
      const end = match.index + match[0].length;
      this.tokens.push({kind, text, start: end - text.length, end});
      position = end;
    }
    if (!this.tokens.length) {
      throw new DebugError('an expression is missing');
    }
  }

  private broken(why: string): DebugError {
    return new DebugError(
      `\`${this.source}\` isn't an expression the debugger reads: ${why}`,
    );
  }

  private refuse(what: string): DebugError {
    return new DebugError(
      `\`${this.source}\` has ${what}, which the debugger doesn't evaluate: it reads names, members and indexes, literals and operators, and runs nothing in the app`,
    );
  }

  private peek(): Token {
    return (
      this.tokens[this.at] ?? {
        kind: null,
        text: null,
        start: this.source.length,
        end: this.source.length,
      }
    );
  }

  private take(): Token {
    const token = this.tokens[this.at++];
    this.lastEnd = token.end;
    return token;
  }

  private expect(text: string): void {
    if (this.peek().text !== text) {
      throw this.unexpected(`\`${text}\` is missing`);
    }
    this.take();
  }

  private textFrom(start: number): string {
    return this.source.slice(start, this.lastEnd);
  }

  /** The error for the next token: what it is when the debugger refuses it, else that it doesn't belong. */
  private unexpected(otherwise?: string): DebugError {
    const {kind, text} = this.peek();
    if (text === '(') return this.refuse('a call');
    if (text !== null && ASSIGNMENTS.has(text)) {
      return this.refuse(`an assignment (${text})`);
    }
    if (text === '=>') return this.refuse('a function');
    if (text === '`') return this.refuse('a template literal');
    if (text === ',' || text === '...') return this.refuse(`\`${text}\``);
    if (kind === 'name' && REFUSED_WORDS.has(text!)) {
      return this.refuse(`\`${text}\``);
    }
    if (otherwise) return this.broken(otherwise);
    if (kind === null) return this.broken('it ends where a value should be');
    return this.broken(`\`${text}\` isn't expected there`);
  }

  parse(): Node {
    const tree = this.conditional();
    if (this.at < this.tokens.length) {
      throw this.unexpected();
    }
    return tree;
  }

  private conditional(): Node {
    const start = this.peek().start;
    const test = this.binary(1);
    if (this.peek().text !== '?') {
      return test;
    }
    this.take();
    const then = this.conditional();
    this.expect(':');
    const otherwise = this.conditional();
    return {
      kind: 'conditional',
      text: this.textFrom(start),
      test,
      then,
      otherwise,
    };
  }

  private binary(least: number): Node {
    const start = this.peek().start;
    let left = this.unary();
    for (;;) {
      const {kind, text} = this.peek();
      const level = kind === 'operator' ? BINARY[text!] : undefined;
      if (level === undefined || level < least) {
        return left;
      }
      this.take();
      // ** groups from the right.
      const right = this.binary(text === '**' ? level : level + 1);
      left = {
        kind: 'binary',
        text: this.textFrom(start),
        operator: text!,
        left,
        right,
      };
    }
  }

  private unary(): Node {
    const {kind, text, start} = this.peek();
    if (
      (kind === 'operator' && ['!', '-', '+', '~'].includes(text!)) ||
      (kind === 'name' && text === 'typeof')
    ) {
      this.take();
      const operand = this.unary();
      return {
        kind: 'unary',
        text: this.textFrom(start),
        operator: text!,
        operand,
      };
    }
    return this.postfix();
  }

  private postfix(): Node {
    const start = this.peek().start;
    let node = this.primary();
    for (;;) {
      let {kind, text} = this.peek();
      if (kind !== 'operator' || !['.', '?.', '['].includes(text!)) {
        if (text !== null && ['(', '++', '--', '`'].includes(text)) {
          throw this.unexpected();
        }
        return node;
      }
      this.take();
      const optional = text === '?.';
      if (optional && this.peek().text === '[') {
        text = this.take().text;
      }
      if (text === '[') {
        const index = this.conditional();
        this.expect(']');
        node = {
          kind: 'index',
          text: this.textFrom(start),
          target: node,
          index,
          optional,
        };
        continue;
      }
      const name = this.peek();
      if (name.kind !== 'name') {
        throw this.unexpected(`a name should follow \`${text}\``);
      }
      this.take();
      node = {
        kind: 'member',
        text: this.textFrom(start),
        target: node,
        name: name.text!,
        optional,
      };
    }
  }

  private primary(): Node {
    const {kind, text, start} = this.peek();
    if (kind === 'number') {
      this.take();
      return {kind: 'literal', text: this.textFrom(start), value: Number(text)};
    }
    if (kind === 'string') {
      this.take();
      return {
        kind: 'literal',
        text: this.textFrom(start),
        value: unquoted(text!),
      };
    }
    if (kind === 'name' && CONSTANTS.has(text!)) {
      this.take();
      return {
        kind: 'literal',
        text: this.textFrom(start),
        value: CONSTANTS.get(text!),
      };
    }
    if (kind === 'name' && !REFUSED_WORDS.has(text!)) {
      this.take();
      return {kind: 'name', text: this.textFrom(start), name: text!};
    }
    if (text === '(') {
      this.take();
      const inner = this.conditional();
      this.expect(')');
      return inner;
    }
    if (text === '[') throw this.refuse('an array literal');
    if (text === '{') throw this.refuse('an object literal');
    throw this.unexpected();
  }
}

const LAST_CODE_POINT = 0x10ffff;

/** A string literal's value. */
function unquoted(text: string): string {
  return text
    .slice(1, -1)
    .replace(
      /\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|.)/g,
      (escape, body: string) => {
        if (!'ux'.includes(body[0]) || body.length === 1) {
          return ESCAPES[body] ?? body;
        }
        const codePoint = parseInt(body.replace(/[ux{}]/g, ''), 16);
        if (codePoint > LAST_CODE_POINT) {
          throw new DebugError(
            `\`${text}\` isn't a string the debugger reads: ${escape} is past the last character, \\u{10ffff}`,
          );
        }
        return String.fromCodePoint(codePoint);
      },
    );
}

export function parseExpression(text: string): Node {
  return new Parser(text).parse();
}

/** A value as text, as JavaScript joins it to a string. */
export function toText(value: Value): string {
  return typeof value === 'string' ? value : show(value);
}

export function truthy(value: Value): boolean {
  if (value instanceof Unreadable) {
    throw new DebugError(
      `a value that can't be read (${value.text}) is neither true nor false`,
    );
  }
  return Boolean(value);
}

/** === on two values. A row is the same as itself: its list's row of that number. */
function same(left: Value, right: Value): boolean {
  if (left instanceof Row && right instanceof Row) {
    return (
      left.state === right.state &&
      left.index !== null &&
      left.index === right.index
    );
  }
  if (left instanceof Rows && right instanceof Rows) {
    return left.state === right.state;
  }
  return left === right;
}

/** == on two values. */
function looselySame(left: Value, right: Value): boolean {
  const nullish = (value: Value) => value === null || value === undefined;
  if (nullish(left) || nullish(right)) {
    return nullish(left) && nullish(right);
  }
  if (
    isStructured(left) ||
    isStructured(right) ||
    typeof left === typeof right
  ) {
    return same(left, right);
  }
  // Only numbers, strings and booleans are left.
  // eslint-disable-next-line eqeqeq
  return (left as number) == (right as number);
}

/** What a value is, for an error. */
function kindOf(value: Value): string {
  if (value instanceof Row) return 'a row';
  if (value instanceof Rows) return 'a list';
  if (value instanceof Fields) return 'an object';
  if (typeof value === 'string') return 'a string';
  return typeof value === 'boolean' ? 'a boolean' : 'a number';
}

/** A value an arithmetic or bitwise operator takes. */
type Plain = number | boolean | string | null | undefined;

/** What an expression's names read: a frame's parameters and locals, then the app's state. */
export class Scope {
  readonly app: App;
  readonly locals = new Map<string, DebugLocal>();
  readonly frame: Fields = new Fields();
  private readonly state: Fields;
  private readonly written: (state: DebugState) => void;

  /**
   * @param app  The running app.
   * @param frame  The frame's place in the app's call chain, innermost 0, or -1 for the state alone.
   * @param written  Called after the debugger writes a state.
   */
  constructor(app: App, frame: number, written: (state: DebugState) => void) {
    this.app = app;
    this.written = written;
    this.state = stateTree(app, written, true);
    const chain = app.inVm ? app.frameChain() : [];
    if (frame >= 0 && frame < chain.length) {
      for (const local of frameLocals(app, chain[frame])) {
        this.locals.set(local.name, local);
      }
      this.frame = localsTree(app, chain[frame]);
    }
  }

  has(name: string): boolean {
    const labels = stateLabels(this.app.panel);
    return (
      this.frame.members.has(name) ||
      this.app.panel.states.some(
        state =>
          name === state.name ||
          name === state.storage ||
          name === labels.get(state.storage),
      ) ||
      this.object(name) !== null
    );
  }

  /** An object state by its name, or by the name alone when one component has it. */
  private object(name: string): Fields | null {
    let found = this.state.members.get(name);
    if (found === undefined) {
      const named = [...this.state.members]
        .filter(([key]) => key.endsWith(`.${name}`))
        .map(([, member]) => member);
      found = named.length === 1 ? named[0] : undefined;
    }
    return found instanceof Fields ? found : null;
  }

  /** What a name is: a local's place, a state's, or an object of them. */
  find(name: string): Place | Fields {
    const local = this.frame.members.get(name);
    if (local !== undefined) {
      return local;
    }
    const found = this.object(name);
    if (found !== null) {
      return found;
    }
    if (!this.has(name)) {
      const names = [
        ...this.frame.members.keys(),
        ...stateTree(this.app, this.written).members.keys(),
      ];
      throw new DebugError(
        `nothing here is named ${name}; the locals and the app's state: ${names.join(', ') || 'none'}`,
      );
    }
    return new StatePlace(this.app, this.app.state(name), this.written);
  }

  /** A state by the label it is shown with, such as a component's `Counter.taps`, or an object state by its label. */
  labelled(label: string): Place | Fields | null {
    const labels = stateLabels(this.app.panel);
    for (const state of this.app.panel.states) {
      if (label === labels.get(state.storage) || label === state.storage) {
        return new StatePlace(this.app, state, this.written);
      }
    }
    const found = this.state.members.get(label);
    return found instanceof Fields ? found : null;
  }
}

/** An optional chain (`a?.b.c`) met null or undefined: the whole chain is undefined. */
class Skipped extends Error {}

class Evaluator {
  private readonly scope: Scope;

  constructor(scope: Scope) {
    this.scope = scope;
  }

  value(node: Node): Value {
    switch (node.kind) {
      case 'member':
      case 'index':
        try {
          return settled(this.find(node));
        } catch (error) {
          if (error instanceof Skipped) return undefined;
          throw error;
        }
      case 'literal':
        return node.value;
      case 'name':
        return settled(this.scope.find(node.name));
      case 'unary':
        return this.unary(node);
      case 'binary':
        return this.binary(node);
      case 'conditional':
        return this.value(
          truthy(this.usable(node.test)) ? node.then : node.otherwise,
        );
    }
  }

  /** A node's value, for an operator to work on. */
  private usable(node: Node): Value {
    const value = this.value(node);
    if (value instanceof Unreadable) {
      throw new DebugError(`${node.text} can't be read: it is ${value.text}`);
    }
    return value;
  }

  /** A node's value, for an operator that takes numbers, strings and booleans. */
  private plain(node: Node, operator: string): Plain {
    const value = this.usable(node);
    if (isStructured(value)) {
      throw new DebugError(
        `${node.text} is ${kindOf(value)}, which \`${operator}\` can't take`,
      );
    }
    return value as Plain;
  }

  /** What a name, a member or an index names, in a chain that an optional link ends. */
  find(node: Node): Found {
    if (node.kind === 'name') {
      return this.scope.find(node.name);
    }
    if (node.kind !== 'member' && node.kind !== 'index') {
      return this.value(node);
    }
    const targetNode = node.target;
    if (
      node.kind === 'member' &&
      targetNode.kind === 'name' &&
      !this.scope.has(targetNode.name)
    ) {
      const found = this.scope.labelled(`${targetNode.name}.${node.name}`);
      if (found !== null) {
        return found;
      }
    }
    const target = settled(this.find(targetNode));
    if (target instanceof Unreadable) {
      throw new DebugError(
        `${targetNode.text} can't be read: it is ${target.text}`,
      );
    }
    const key: Value =
      node.kind === 'index' ? this.usable(node.index) : node.name;
    if (target === null || target === undefined) {
      if (node.optional) {
        throw new Skipped();
      }
      throw new DebugError(
        `${targetNode.text} is ${show(target)}, so it has no ${toText(key)}`,
      );
    }
    const whole = typeof key === 'number' && Number.isInteger(key);
    if (target instanceof Row && typeof key === 'string') {
      return target.child(key);
    }
    if (target instanceof Fields && typeof key === 'string') {
      const member = target.members.get(key);
      if (member === undefined) {
        throw new DebugError(
          `${targetNode.text} has no ${key}; it has ${[...target.members.keys()].join(', ')}`,
        );
      }
      return member;
    }
    if (
      target instanceof Rows &&
      node.kind === 'index' &&
      typeof key === 'number'
    ) {
      return whole ? target.child(key) : undefined;
    }
    if (
      typeof target === 'string' &&
      node.kind === 'index' &&
      typeof key === 'number'
    ) {
      // By UTF-16 unit, as `length` counts.
      return whole && key >= 0 && key < target.length ? target[key] : undefined;
    }
    if (target instanceof Rows && key === 'length') {
      return target.count;
    }
    if (typeof target === 'string' && key === 'length') {
      return target.length;
    }
    throw new DebugError(
      `${targetNode.text} is ${kindOf(target)}, which has no ${toText(key)} here`,
    );
  }

  private unary(node: Extract<Node, {kind: 'unary'}>): Value {
    const {operator, operand} = node;
    if (operator === 'typeof') {
      const value = this.usable(operand);
      if (value === undefined) {
        return 'undefined';
      }
      return ['boolean', 'number', 'string'].includes(typeof value)
        ? typeName(value)
        : 'object';
    }
    if (operator === '!') {
      return !truthy(this.usable(operand));
    }
    const value = this.plain(operand, operator);
    if (operator === '~') {
      return ~Number(value);
    }
    return operator === '-' ? -Number(value) : Number(value);
  }

  private binary(node: Extract<Node, {kind: 'binary'}>): Value {
    const {operator} = node;
    if (operator === '&&' || operator === '||' || operator === '??') {
      const left = this.usable(node.left);
      const keep =
        operator === '??'
          ? left !== null && left !== undefined
          : truthy(left) === (operator === '||');
      return keep ? left : this.usable(node.right);
    }
    if (['===', '!==', '==', '!='].includes(operator)) {
      const left = this.usable(node.left);
      const right = this.usable(node.right);
      const equal =
        operator.length === 3 ? same(left, right) : looselySame(left, right);
      return equal === (operator[0] === '=');
    }
    const left = this.plain(node.left, operator);
    const right = this.plain(node.right, operator);
    if (
      operator === '+' &&
      (typeof left === 'string' || typeof right === 'string')
    ) {
      return toText(left) + toText(right);
    }
    if (['<', '<=', '>', '>='].includes(operator)) {
      return compare(operator, left, right);
    }
    const leftNumber = Number(left);
    const rightNumber = Number(right);
    switch (operator) {
      case '+':
        return leftNumber + rightNumber;
      case '-':
        return leftNumber - rightNumber;
      case '*':
        return leftNumber * rightNumber;
      case '/':
        return leftNumber / rightNumber;
      case '%':
        return leftNumber % rightNumber;
      case '**':
        return leftNumber ** rightNumber;
      case '&':
        return leftNumber & rightNumber;
      case '|':
        return leftNumber | rightNumber;
      case '^':
        return leftNumber ^ rightNumber;
      case '<<':
        return leftNumber << rightNumber;
      case '>>':
        return leftNumber >> rightNumber;
      default:
        return leftNumber >>> rightNumber;
    }
  }
}

function compare(operator: string, left: Plain, right: Plain): boolean {
  // Two strings order by UTF-16 unit; anything else as numbers.
  const both = typeof left === 'string' && typeof right === 'string';
  const first = both ? left : Number(left);
  const second = both ? right : Number(right);
  switch (operator) {
    case '<':
      return first < second;
    case '<=':
      return first <= second;
    case '>':
      return first > second;
    default:
      return first >= second;
  }
}

/** An expression's value in a scope. */
export function evaluate(tree: Node, scope: Scope): Value {
  return new Evaluator(scope).value(tree);
}

/**
 * What an expression names in a scope: a place, or a value kept nowhere.
 *
 * @param scope  The scope.
 * @param text  The expression, or a state's label, which needn't read as one: `Counter:16.taps`.
 *
 * @returns What it names.
 */
export function findText(scope: Scope, text: string): Found {
  // A local of the name comes first, an object too.
  const local =
    scope.locals.has(text) || scope.frame.members.has(text.split(/[.[]/)[0]);
  const found = local ? null : scope.labelled(text);
  if (found !== null) {
    return found;
  }
  try {
    return new Evaluator(scope).find(parseExpression(text));
  } catch (error) {
    if (error instanceof Skipped) return undefined;
    throw error;
  }
}
