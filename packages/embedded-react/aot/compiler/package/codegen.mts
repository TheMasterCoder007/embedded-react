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
 * Lowers IR expressions, text, statements and state-driven prop values to bytecode.
 *
 * The C output leans on C's conversions: an int beside a float is a float, a value worked out at boot is a
 * double, and an assignment converts to the field it writes. The VM converts nothing by itself, so each value
 * here is worked out in the type C works it out in (its "C type"), and converted where C would convert it.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError, constantText} from '../diagnostics.mts';
import {animationStatement} from './animations.mts';
import {EventField, MathFn} from './format.mts';
import {abiProp, floatBits, readConstant} from './props.mts';
import {updateVector} from './vectors.mts';
import type {AnimationPlan} from './animations.mts';
import type {Arena, Slot} from './arena.mts';
import type {
  FunctionBuilder,
  Label,
  Register,
  RegisterType,
} from './bytecode.mts';
import type {ProgramTables} from './tables.mts';
import type {VectorPlan} from './vectors.mts';
import type {IrBootSlot} from '../ir/boot.mts';
import type {
  IrExpr,
  IrGesture,
  LayoutField,
  WholeHelper,
} from '../ir/expressions.mts';
import type {IrPropValue, IrShow} from '../ir/nodes.mts';
import type {IrAppendItem, IrStmt, IrTimer} from '../ir/statements.mts';
import type {IrTextPart} from '../ir/text.mts';
import type {ListState, Local, RefRecord, ScalarState} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** The type C works a value out in. A boolean is C's int holding 0 or 1; an enum is an interned string. */
export type CType =
  | 'int'
  | 'bool'
  | 'float'
  | 'double'
  | 'int64'
  | 'string'
  | 'enum';

/** An interned string state's set: its index in the enums table, and its values. */
export interface EnumSet {
  set: number;
  values: string[];
}

/** A list's pool, and the buffer an appended item's text is built in, by field. */
export interface ListPool {
  slot: Slot;
  buffers: Map<string, Slot>;
}

/** A PanResponder's gesture state, as C keeps it: whether it is granted, the travel at the grant, and x0/y0. */
export interface GestureSlots {
  granted: Slot;
  baseDx: Slot;
  baseDy: Slot;
  x0: Slot;
  y0: Slot;
}

/** The slots the program's code names, by what they hold. */
export interface ProgramSlots {
  arena: Arena;
  tables: ProgramTables;
  states: Map<ScalarState, Slot>;
  lists: Map<ListState, ListPool>;
  refs: Map<RefRecord, Slot>;
  boot: Map<IrBootSlot, Slot>;
  /** The buffer each string local is copied into, by its C name. */
  stringLocals: Map<string, Slot>;
  /** The string states held as interned enums. */
  enums: Map<ScalarState, EnumSet>;
  /** The slot each effect local its cleanup reads is kept in, by its C name. */
  hoisted: Map<string, Slot>;
  /** Each effect's flag that its last run reached its cleanup, by the flag's C name. */
  armed: Map<string, Slot>;
  /** Each timer callback's function, by its C name. */
  timerFunctions: Map<string, number>;
  animations: AnimationPlan;
  /** Each PanResponder's gesture state, by its C prefix. */
  gestures: Map<string, GestureSlots>;
  vectors: VectorPlan;
}

/** The <TextInput> a CHANGE_TEXT handler reads its new text from, and the buffer it reads it into. */
export interface InputText {
  node: Slot;
  text: Slot;
}

/** A local's value: a register, or, for a string, its buffer. */
type LocalBinding = {register: Register} | {slot: Slot};

/** A state-driven prop value worked out, and the conversion C applies to it on its way into the field. */
interface PropResult {
  register: Register;
  type: CType;
  conversion: 'opacity' | 'dimension' | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The whole-number helpers' opcodes; the 64-bit ones add L. */
const HELPER_OPS: Record<WholeHelper, string> = {
  add: 'ADD',
  sub: 'SUB',
  mul: 'MUL',
  neg: 'NEG',
  mod: 'MOD',
  abs: 'ABS',
  min: 'MIN',
  max: 'MAX',
  floordiv: 'FLOORDIV',
  ceildiv: 'CEILDIV',
  rounddiv: 'ROUNDDIV',
  div: 'DIV',
};

/** The arithmetic opcodes for each C type. */
const ARITHMETIC_OPS: Record<string, Record<string, string>> = {
  int: {'+': 'ADD', '-': 'SUB', '*': 'MUL', '/': 'DIV', '%': 'MOD'},
  int64: {'+': 'ADDL', '-': 'SUBL', '*': 'MULL', '/': 'DIVL', '%': 'MODL'},
  float: {'+': 'FADD', '-': 'FSUB', '*': 'FMUL', '/': 'FDIV'},
  double: {'+': 'DADD', '-': 'DSUB', '*': 'DMUL', '/': 'DDIV'},
};

/** The comparison opcodes for each C type: equal, less than, at most. */
const COMPARE_OPS: Record<string, [string, string, string]> = {
  int: ['EQ', 'LT', 'LE'],
  int64: ['LEQ', 'LLT', 'LLE'],
  float: ['FEQ', 'FLT', 'FLE'],
  double: ['DEQ', 'DLT', 'DLE'],
};

/** The conversion opcodes, by source and destination C type. */
const CONVERSIONS: Record<string, string> = {
  'int>float': 'ITOF',
  'int>double': 'ITOD',
  'int>int64': 'ITOL',
  'float>int': 'FTOI',
  'float>double': 'FTOD',
  'float>int64': 'FTOL',
  'double>int': 'DTOI',
  'double>float': 'DTOF',
  'double>int64': 'DTOL',
  'int64>int': 'LTOI',
  'int64>float': 'LTOF',
  'int64>double': 'LTOD',
};

/** The event field each touch field, and what a <Dial> passes, reads. */
const EVENT_FIELDS: Record<string, number> = {
  x: EventField.x,
  y: EventField.y,
  dx: EventField.dx,
  dy: EventField.dy,
  vx: EventField.vx,
  vy: EventField.vy,
  value: EventField.value,
  value_start: EventField.valueStart,
};

/** The event field each onLayout field reads. */
const LAYOUT_FIELDS: Record<LayoutField, number> = {
  x: EventField.layoutX,
  y: EventField.layoutY,
  width: EventField.layoutW,
  height: EventField.layoutH,
};

/** The engine's display values. */
const DISPLAY_FLEX = 0;
const DISPLAY_NONE = 1;

/** A timer delay is ToInt32 of the value: its low 32 bits. */
const TWO_TO_32 = 2 ** 32;
const INT32_MAX = 0x7fffffff;

/** C's int range, for an immediate that fits LOADI. */
const IMMEDIATE_MIN = -32768;
const IMMEDIATE_MAX = 32767;

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Lowers IR into one function.
 *
 * @class Codegen
 */
export class Codegen {
  readonly fn: FunctionBuilder;
  private readonly program: ProgramSlots;
  private readonly inDoubles: boolean;
  private readonly input: InputText | null;
  private readonly onReturn: Label | null;
  private readonly row: Register | null;
  private readonly locals = new Map<string, LocalBinding>();
  /** A string register nothing writes, so it stays the empty string. Reserved, so no release reuses it. */
  private emptyString: Register | null = null;

  /**
   * @param fn  The function being built.
   * @param program  The program's slots and tables.
   * @param options  inDoubles: fractions are worked out in double precision, as C works out a value at boot.
   *                 input: the <TextInput> a CHANGE_TEXT handler reads.
   *                 onReturn: where an effect's early `return` goes.
   *                 row: the row a list row's handler was built for, which its item reads.
   *                 finished: what an animation completion was passed, which its `finished` reads.
   */
  constructor(
    fn: FunctionBuilder,
    program: ProgramSlots,
    options: {
      inDoubles?: boolean;
      input?: InputText;
      onReturn?: Label;
      row?: Register;
      finished?: Register;
    } = {},
  ) {
    this.fn = fn;
    this.program = program;
    this.inDoubles = options.inDoubles ?? false;
    this.input = options.input ?? null;
    this.onReturn = options.onReturn ?? null;
    this.row = options.row ?? null;
    if (options.finished) {
      this.locals.set('finished', {register: options.finished});
    }
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Types
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * The type C works an expression out in.
   *
   * @param expr  The expression.
   *
   * @returns Its C type.
   */
  typeOf(expr: IrExpr): CType {
    const fraction: CType = this.inDoubles ? 'double' : 'float';
    switch (expr.kind) {
      case 'number':
        return expr.type.kind === 'float'
          ? fraction
          : expr.type.kind === 'int64'
            ? 'int64'
            : expr.type.kind === 'bool'
              ? 'bool'
              : 'int';
      case 'string':
        return 'string';
      case 'boolean':
        return 'bool';
      case 'state':
        // An interned state is still a string to C.
        return this.program.enums.has(expr.state)
          ? 'string'
          : slotType(this.stateSlot(expr.state));
      case 'boot':
        return slotType(this.bootSlot(expr.slot));
      case 'ref':
        return slotType(this.refSlot(expr.ref));
      case 'local': {
        if (expr.local.expr) {
          return this.typeOf(expr.local.expr);
        }
        const binding = this.binding(expr.local.code);
        return !binding
          ? 'string'
          : 'register' in binding
            ? (binding.register.type as CType)
            : slotType(binding.slot);
      }
      case 'event':
        return expr.field === 'changed_text'
          ? 'string'
          : ['vx', 'vy', 'value', 'value_start'].includes(expr.field)
            ? 'float'
            : 'int';
      case 'gesture':
        return expr.field === 'vx' || expr.field === 'vy' ? 'float' : 'int';
      case 'clock':
        return 'int64';
      case 'itemField':
        return expr.field.kind;
      case 'pi':
      case 'toFloat':
      case 'floatDivide':
      case 'floatMath':
        return fraction;
      case 'unary':
        return expr.op === '!' ? 'bool' : promoted(this.typeOf(expr.operand));
      case 'helper':
        return expr.bits === 64 ? 'int64' : 'int';
      case 'arithmetic':
        return usual(this.typeOf(expr.left), this.typeOf(expr.right));
      case 'wholeRound':
        return this.typeOf(expr.operand);
      case 'compare':
      case 'stringEquals':
      case 'logical':
        return 'bool';
      case 'pick':
        return usual(this.typeOf(expr.left), this.typeOf(expr.right));
      case 'conditional':
        return usual(this.typeOf(expr.consequent), this.typeOf(expr.alternate));
      default:
        // layout, narrowRemainder, trunc, toInt, and what the package output refuses
        return 'int';
    }
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Values
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Works an expression out into a register of its C type.
   *
   * @param expr  The expression.
   *
   * @returns The register. A local's may be its own: never write to what this returns.
   */
  value(expr: IrExpr): Register {
    const {fn} = this;
    const type = this.typeOf(expr);
    switch (expr.kind) {
      case 'number':
        return this.load(type, expr.value);
      case 'string': {
        const register = fn.register('string');
        fn.abx('LOADS', register.index, this.program.tables.string(expr.value));
        return register;
      }
      case 'boolean':
        return this.load('bool', expr.value ? 1 : 0);
      case 'state':
        if (this.program.enums.has(expr.state)) {
          throw new Error(
            `AOT internal: state "${expr.state.name}" is interned, but read as a string`,
          );
        }
        return this.get(this.stateSlot(expr.state));
      case 'boot':
        return this.get(this.bootSlot(expr.slot));
      case 'ref':
        return this.get(this.refSlot(expr.ref));
      case 'local':
        return this.local(expr);
      case 'listLength': {
        const register = fn.register('int');
        fn.abx('LEN', register.index, this.listPool(expr.list).slot.index);
        return register;
      }
      case 'itemField':
        return this.itemField(expr.item, expr.field.key, type);
      case 'gesture':
        return this.gesture(expr);
      case 'event':
        return expr.field === 'changed_text'
          ? this.changedText()
          : this.event(type, EVENT_FIELDS[expr.field]);
      case 'layout':
        return this.event('int', LAYOUT_FIELDS[expr.field]);
      case 'clock': {
        const register = fn.register('int64');
        fn.abc(expr.clock === 'Date' ? 'DATENOW' : 'NOW', register.index);
        return register;
      }
      case 'pi':
        return this.load(type, Math.PI);
      case 'toFloat':
      case 'wholeRound':
        return this.convert(this.value(expr.operand), type);
      case 'unary':
        return this.unary(expr.op, expr.operand, type);
      case 'helper': {
        const argType: CType = expr.bits === 64 ? 'int64' : 'int';
        const args = expr.args.map(arg => this.valueAs(arg, argType));
        const register = fn.register(argType);
        fn.abc(
          HELPER_OPS[expr.helper] + (expr.bits === 64 ? 'L' : ''),
          register.index,
          args[0].index,
          (args[1] ?? args[0]).index,
        );
        return register;
      }
      case 'arithmetic':
        return this.arithmetic(expr.op, expr.left, expr.right, type);
      case 'floatDivide':
        return this.arithmetic('/', expr.left, expr.right, type);
      case 'narrowRemainder': {
        const operandType = usual(
          this.typeOf(expr.left),
          this.typeOf(expr.right),
        );
        return this.convert(
          this.arithmetic('%', expr.left, expr.right, operandType),
          'int',
        );
      }
      case 'floatMath': {
        const args = expr.args.map(arg => this.valueAs(arg, type));
        const register = fn.register(type as RegisterType);
        fn.abc(
          type === 'double' ? 'DMATH' : 'FMATH',
          register.index,
          args[0].index,
          (args[1] ?? args[0]).index,
          MathFn[expr.fn],
        );
        return register;
      }
      case 'trunc':
      case 'toInt':
        // app_f2i takes a float, and app_d2i a double.
        return this.convert(
          this.valueAs(expr.operand, this.inDoubles ? 'double' : 'float'),
          'int',
        );
      case 'compare':
        return this.compare(expr.op, expr.left, expr.right);
      case 'stringEquals': {
        if (this.enumOf(expr.left) || this.enumOf(expr.right)) {
          return this.enumEquals(expr.op, expr.left, expr.right);
        }
        const register = fn.register('bool');
        fn.abc(
          'SEQ',
          register.index,
          this.valueAs(expr.left, 'string').index,
          this.valueAs(expr.right, 'string').index,
        );
        if (expr.op === '!=') {
          fn.abc('NOT', register.index, register.index);
        }
        return register;
      }
      case 'logical': {
        const register = fn.register('bool');
        const end = fn.label();
        fn.abc('MOVE', register.index, this.condition(expr.left).index);
        fn.branch(expr.op === '&&' ? 'JF' : 'JT', end, register);
        fn.abc('MOVE', register.index, this.condition(expr.right).index);
        fn.place(end);
        return register;
      }
      case 'pick':
        return this.pick(expr.op, expr.left, expr.right, type);
      case 'conditional':
        return this.choose(
          this.condition(expr.test),
          () => this.valueAs(expr.consequent, type),
          () => this.valueAs(expr.alternate, type),
          type,
        );
    }
  }

  /**
   * The set of an interned state an expression reads, or null when it reads none.
   *
   * @param expr  The expression.
   *
   * @returns The set.
   */
  private enumOf(expr: IrExpr): EnumSet | null {
    return expr.kind === 'state'
      ? (this.program.enums.get(expr.state) ?? null)
      : null;
  }

  /**
   * A comparison of an interned state with a literal, or with itself, as an int compare. A literal it never
   * holds is never equal.
   *
   * @param op  `==` or `!=`.
   * @param left  One side.
   * @param right  The other.
   *
   * @returns A bool register.
   */
  private enumEquals(op: '==' | '!=', left: IrExpr, right: IrExpr): Register {
    const read = (this.enumOf(left) ? left : right) as IrExpr & {kind: 'state'};
    const other = read === left ? right : left;
    const {set, values} = this.enumOf(read)!;
    if (other.kind !== 'string') {
      return this.load('bool', op === '==' ? 1 : 0);
    }
    const index = values.indexOf(other.value);
    if (index < 0) {
      return this.load('bool', op === '==' ? 0 : 1);
    }
    const register = this.fn.register('bool');
    this.fn.abc(
      'EQ',
      register.index,
      this.get(this.stateSlot(read.state)).index,
      this.enumValue(set, index).index,
    );
    if (op === '!=') {
      this.fn.abc('NOT', register.index, register.index);
    }
    return register;
  }

  /**
   * One value of an enum set in a register.
   *
   * @param set  The set.
   * @param index  The value's index in it.
   *
   * @returns The register.
   */
  private enumValue(set: number, index: number): Register {
    const register = this.fn.register('enum');
    this.fn.abx('LOADE', register.index, index, set);
    return register;
  }

  /**
   * Works an expression out and converts it as C converts a value assigned to a type.
   *
   * @param expr  The expression.
   * @param type  The type it goes into.
   *
   * @returns A register of that type.
   */
  valueAs(expr: IrExpr, type: CType): Register {
    return this.convert(this.value(expr), type);
  }

  /**
   * An expression as a condition: true exactly when JS (and the C output) find it truthy. A string is truthy
   * when it isn't empty.
   *
   * @param expr  The expression.
   *
   * @returns A bool register.
   */
  condition(expr: IrExpr): Register {
    return this.truthy(this.value(expr));
  }

  /**
   * Converts a register as C converts a value assigned to another type. A number to a boolean is a test against
   * zero; a boolean to a number is 0 or 1.
   *
   * @param register  The register.
   * @param type  The type.
   *
   * @returns A register of that type: this one when it already is.
   */
  convert(register: Register, type: CType): Register {
    const from = register.type as CType;
    if (from === type) {
      return register;
    }
    if (type === 'bool') {
      return this.truthy(register);
    }
    if (from === 'bool') {
      const whole = this.fn.register('int');
      const end = this.fn.label();
      this.fn.abx('LOADI', whole.index, 0);
      this.fn.branch('JF', end, register);
      this.fn.abx('LOADI', whole.index, 1);
      this.fn.place(end);
      return this.convert(whole, type);
    }
    const op = CONVERSIONS[`${from}>${type}`];
    if (!op) {
      throw new Error(`AOT internal: no conversion from ${from} to ${type}`);
    }
    const converted = this.fn.register(type as RegisterType);
    this.fn.abc(op, converted.index, register.index);
    return converted;
  }

  /**
   * Whether a register's value is truthy.
   *
   * @param register  The register.
   *
   * @returns A bool register.
   */
  truthy(register: Register): Register {
    if (register.type === 'bool') {
      return register;
    }
    const test = this.fn.register('bool');
    if (register.type === 'string') {
      this.fn.abc('SEQ', test.index, register.index, this.zero('string').index);
    } else {
      const zero = this.load(register.type as CType, 0);
      this.fn.abc(
        COMPARE_OPS[register.type][0],
        test.index,
        register.index,
        zero.index,
      );
    }
    this.fn.abc('NOT', test.index, test.index);
    return test;
  }

  /**
   * A constant in a register of a type.
   *
   * @param type  The type.
   * @param value  The value.
   *
   * @returns The register.
   */
  load(type: CType, value: number): Register {
    const {fn} = this;
    const {tables} = this.program;
    const register = fn.register(type as RegisterType);
    switch (type) {
      case 'int':
      case 'bool': {
        const whole = Math.trunc(value) | 0;
        if (whole >= IMMEDIATE_MIN && whole <= IMMEDIATE_MAX) {
          fn.abx('LOADI', register.index, whole);
        } else {
          fn.abx('LOADK', register.index, tables.constant32(whole));
        }
        break;
      }
      case 'float':
        fn.abx('LOADK', register.index, tables.constant32(floatBits(value)));
        break;
      case 'double':
        fn.abx('LOADK64', register.index, tables.constant64(doubleBits(value)));
        break;
      case 'int64':
        fn.abx(
          'LOADK64',
          register.index,
          tables.constant64(BigInt(Math.trunc(value))),
        );
        break;
      default:
        throw new Error(`AOT internal: no ${type} constant`);
    }
    return register;
  }

  /**
   * Whether two registers of one type hold the same value, as C's `==`, or strcmp() for strings, finds it.
   *
   * @param left  One.
   * @param right  The other.
   *
   * @returns A bool register.
   */
  same(left: Register, right: Register): Register {
    const register = this.fn.register('bool');
    const op =
      left.type === 'string'
        ? 'SEQ'
        : left.type === 'bool' || left.type === 'enum'
          ? 'EQ'
          : COMPARE_OPS[left.type][0];
    this.fn.abc(op, register.index, left.index, right.index);
    return register;
  }

  /**
   * A slot's value in a register of its type.
   *
   * @param slot  The slot.
   *
   * @returns The register.
   */
  get(slot: Slot): Register {
    const type = slotType(slot);
    const register = this.fn.register(type as RegisterType);
    const op =
      type === 'string'
        ? 'GETS'
        : type === 'int64' || type === 'double'
          ? 'GET64'
          : 'GET32';
    this.fn.abx(op, register.index, slot.index);
    return register;
  }

  /**
   * Writes a register to a slot, converting it to the slot's type. A slot with a dirty bit has it set when the
   * value changes.
   *
   * @param slot  The slot.
   * @param value  The value.
   */
  set(slot: Slot, value: Register): void {
    const type = slotType(slot);
    const register = this.convert(value, type);
    const op =
      type === 'string'
        ? 'SETS'
        : type === 'int64' || type === 'double'
          ? 'SET64'
          : 'SET32';
    this.fn.abx(op, register.index, slot.index);
  }

  private event(type: CType, field: number): Register {
    const register = this.fn.register(type as RegisterType);
    this.fn.abc('EVT', register.index, field);
    return register;
  }

  /**
   * A gestureState field, as C reads it: travel from the grant, x0/y0 from the responder's state, the rest
   * off the event.
   *
   * @param expr  The field.
   *
   * @returns The register.
   */
  private gesture(expr: IrGesture): Register {
    const slots = this.program.gestures.get(expr.statePrefix)!;
    switch (expr.field) {
      case 'dx':
      case 'dy': {
        const travel = this.event('int', EVENT_FIELDS[expr.field]);
        const base = this.get(
          expr.field === 'dx' ? slots.baseDx : slots.baseDy,
        );
        const register = this.fn.register('int');
        this.fn.abc('SUB', register.index, travel.index, base.index);
        return register;
      }
      case 'moveX':
        return this.event('int', EventField.x);
      case 'moveY':
        return this.event('int', EventField.y);
      case 'x0':
        return this.get(slots.x0);
      case 'y0':
        return this.get(slots.y0);
      case 'vx':
      case 'vy':
        return this.event('float', EVENT_FIELDS[expr.field]);
      case 'numberActiveTouches':
        return this.event('int', EventField.touches);
      case 'stateID':
        return this.load('int', expr.stateID);
    }
  }

  /** The text a <TextInput> holds now, as its CHANGE_TEXT handler receives it. */
  private changedText(): Register {
    if (!this.input) {
      throw new Error(
        'AOT internal: changed text outside a CHANGE_TEXT handler',
      );
    }
    const node = this.fn.register('node');
    this.fn.abx('GET64', node.index, this.input.node.index);
    this.fn.abx('INPUT', node.index, this.input.text.index);
    return this.get(this.input.text);
  }

  /**
   * A field of a list row's item. A row past the list's end, as a hidden row is, reads as zero or "".
   *
   * @param item  The row's item.
   * @param key  The field.
   * @param type  Its C type.
   *
   * @returns The register.
   */
  private itemField(item: Local, key: string, type: CType): Register {
    const {fn} = this;
    const list = required(item.list, `the list of "${item.code}"`);
    const field = list.struct.fields.findIndex(entry => entry.key === key);
    const pool = this.listPool(list).slot;
    const row = this.row ?? this.load('int', required(item.row, item.code));
    const count = fn.register('int');
    const inUse = fn.register('bool');
    fn.abx('LEN', count.index, pool.index);
    fn.abc('LT', inUse.index, row.index, count.index);
    return this.choose(
      inUse,
      () => {
        const register = fn.register(type as RegisterType);
        fn.abc(
          'ROWGET',
          register.index,
          row.index,
          0,
          pool.index | (field << 16),
        );
        return register;
      },
      () => this.zero(type),
      type,
    );
  }

  /** Zero, or the empty string, in a register of a type. */
  private zero(type: CType): Register {
    if (type !== 'string') {
      return this.load(type, 0);
    }
    this.emptyString ??= this.fn.reserved('string');
    return this.emptyString;
  }

  private local(expr: IrExpr & {kind: 'local'}): Register {
    // A memo, a component's prop or a helper's argument stands for its expression, as C inlines it.
    if (expr.local.expr) {
      return this.value(expr.local.expr);
    }
    const binding = this.binding(expr.local.code);
    if (!binding) {
      return notYet('the value an element passes its callback');
    }
    return 'register' in binding ? binding.register : this.get(binding.slot);
  }

  /** A local's value, by its C name: this function's own, or an effect local kept in a slot. */
  private binding(code: string): LocalBinding | undefined {
    const hoisted = this.program.hoisted.get(code);
    return this.locals.get(code) ?? (hoisted && {slot: hoisted});
  }

  private unary(op: '-' | '+' | '!', operand: IrExpr, type: CType): Register {
    if (op === '!') {
      const register = this.fn.register('bool');
      this.fn.abc('NOT', register.index, this.condition(operand).index);
      return register;
    }
    const value = this.valueAs(operand, type);
    if (op === '+') {
      return value;
    }
    const register = this.fn.register(type as RegisterType);
    const neg = {int: 'NEG', int64: 'NEGL', float: 'FNEG', double: 'DNEG'}[
      type as string
    ];
    this.fn.abc(neg!, register.index, value.index);
    return register;
  }

  private arithmetic(
    op: string,
    left: IrExpr,
    right: IrExpr,
    type: CType,
  ): Register {
    const a = this.valueAs(left, type);
    const b = this.valueAs(right, type);
    const register = this.fn.register(type as RegisterType);
    // C has no float %, so the IR's float remainder is fmod.
    if (op === '%' && (type === 'float' || type === 'double')) {
      this.fn.abc(
        type === 'double' ? 'DMATH' : 'FMATH',
        register.index,
        a.index,
        b.index,
        MathFn.mod,
      );
      return register;
    }
    this.fn.abc(ARITHMETIC_OPS[type][op], register.index, a.index, b.index);
    return register;
  }

  private compare(op: string, left: IrExpr, right: IrExpr): Register {
    const type = promoted(usual(this.typeOf(left), this.typeOf(right)));
    const a = this.valueAs(left, type);
    const b = this.valueAs(right, type);
    const [equal, less, atMost] = COMPARE_OPS[type];
    const register = this.fn.register('bool');
    const [opcode, first, second] =
      op === '<'
        ? [less, a, b]
        : op === '>'
          ? [less, b, a]
          : op === '<='
            ? [atMost, a, b]
            : op === '>='
              ? [atMost, b, a]
              : [equal, a, b];
    this.fn.abc(opcode, register.index, first.index, second.index);
    if (op === '!=') {
      this.fn.abc('NOT', register.index, register.index);
    }
    return register;
  }

  /** `a || b` as C's `(a ? a : b)`, and `a && b` as `(a ? b : a)`. */
  private pick(
    op: '&&' | '||',
    left: IrExpr,
    right: IrExpr,
    type: CType,
  ): Register {
    // C tests a string's pointer, which is never null.
    if (type === 'string') {
      return this.valueAs(op === '||' ? left : right, type);
    }
    const leftValue = this.value(left);
    return this.choose(
      this.truthy(leftValue),
      () => this.convert(op === '||' ? leftValue : this.value(right), type),
      () => this.convert(op === '||' ? this.value(right) : leftValue, type),
      type,
    );
  }

  /**
   * `test ? whenTrue : whenFalse`, each side worked out only when it is chosen.
   *
   * @param test  A bool register.
   * @param whenTrue  Works out the value when the test holds.
   * @param whenFalse  Works out the value when it doesn't.
   * @param type  The result's type.
   *
   * @returns The result.
   */
  choose(
    test: Register,
    whenTrue: () => Register,
    whenFalse: () => Register,
    type: CType,
  ): Register {
    const {fn} = this;
    const register = fn.register(type as RegisterType);
    const otherwise = fn.label();
    const end = fn.label();
    fn.branch('JF', otherwise, test);
    fn.abc('MOVE', register.index, whenTrue().index);
    fn.branch('JMP', end);
    fn.place(otherwise);
    fn.abc('MOVE', register.index, whenFalse().index);
    fn.place(end);
    return register;
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Text
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Builds a text in the format buffer, each part as the C output's printf prints it. The caller puts it
   * somewhere (FMTTEXT, FMTSET or FMTSPAN), which empties the buffer.
   *
   * @param parts  The text's parts.
   */
  text(parts: IrTextPart[]): void {
    const {fn} = this;
    for (const part of parts) {
      const checkpoint = fn.checkpoint();
      if (part.kind === 'literal') {
        if (part.text) {
          fn.abx('FMTK', 0, this.program.tables.string(part.text));
        }
      } else if (part.kind === 'booleanWord') {
        fn.abc('FMTB', this.condition(part.value).index);
      } else if (this.enumOf(part.value)) {
        const value = part.value as IrExpr & {kind: 'state'};
        fn.abx(
          'FMTE',
          this.get(this.stateSlot(value.state)).index,
          this.enumOf(value)!.set,
        );
      } else {
        // The spec is by the value's IR type: a float is printed from a float, so a boot double is narrowed first.
        const type = part.value.type.kind;
        const [op, as]: [string, CType] =
          type === 'string'
            ? ['FMTS', 'string']
            : type === 'float'
              ? ['FMTF', 'float']
              : type === 'int64'
                ? ['FMTL', 'int64']
                : ['FMTI', 'int'];
        fn.abc(op, this.valueAs(part.value, as).index);
      }
      fn.release(checkpoint);
    }
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Statements
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Lowers statements, in order.
   *
   * @param stmts  The statements.
   */
  statements(stmts: IrStmt[]): void {
    for (const stmt of stmts) {
      this.statement(stmt);
    }
  }

  private statement(stmt: IrStmt): void {
    const {fn} = this;
    fn.at(stmt.loc);
    switch (stmt.kind) {
      case 'declareLocal': {
        const hoisted = this.program.hoisted.get(stmt.name);
        if (stmt.isHoisted && !hoisted) {
          throw new Error(`AOT internal: no slot for local "${stmt.name}"`);
        }
        if (hoisted && slotType(hoisted) !== 'string') {
          this.scoped(() => this.set(hoisted, this.value(stmt.init)));
          this.locals.set(stmt.name, {slot: hoisted});
          return;
        }
        if (this.typeOf(stmt.init) === 'string') {
          // A string local is a copy of its own, so a later write to the state it was read from can't change it.
          const slot = hoisted ?? this.program.stringLocals.get(stmt.name)!;
          const checkpoint = fn.checkpoint();
          this.text([{kind: 'value', value: stmt.init}]);
          fn.abx('FMTSET', 0, slot.index);
          fn.release(checkpoint);
          this.locals.set(stmt.name, {slot});
          return;
        }
        const type = declaredType(stmt.init);
        const register = fn.register(type as RegisterType);
        const checkpoint = fn.checkpoint();
        fn.abc('MOVE', register.index, this.valueAs(stmt.init, type).index);
        fn.release(checkpoint);
        this.locals.set(stmt.name, {register});
        return;
      }
      case 'setState':
        this.scoped(() =>
          this.set(this.stateSlot(stmt.state), this.value(stmt.value)),
        );
        return;
      case 'setStateText':
        this.scoped(() => {
          // An interned state is set to the index of the constant it is given.
          const set = this.program.enums.get(stmt.state);
          if (set) {
            const index = set.values.indexOf(constantText(stmt.text)!);
            fn.abx(
              'SET32',
              this.enumValue(set.set, index).index,
              this.stateSlot(stmt.state).index,
            );
            return;
          }
          this.text(stmt.text.parts);
          fn.abx('FMTSET', 0, this.stateSlot(stmt.state).index);
        });
        return;
      case 'writeRef':
        this.scoped(() => this.writeRef(stmt.ref, stmt.op, stmt.value));
        return;
      case 'stepRef':
        this.scoped(() => {
          const slot = this.refSlot(stmt.ref);
          const type = slotType(slot);
          const register = fn.register(type as RegisterType);
          fn.abc(
            ARITHMETIC_OPS[type][stmt.op === '++' ? '+' : '-'],
            register.index,
            this.get(slot).index,
            this.load(type, 1).index,
          );
          this.set(slot, register);
        });
        return;
      case 'if': {
        const otherwise = fn.label();
        const checkpoint = fn.checkpoint();
        fn.branch('JF', otherwise, this.condition(stmt.test));
        fn.release(checkpoint);
        this.statements(stmt.consequent);
        if (stmt.alternate) {
          const end = fn.label();
          fn.branch('JMP', end);
          fn.place(otherwise);
          this.statements(stmt.alternate);
          fn.place(end);
        } else {
          fn.place(otherwise);
        }
        return;
      }
      case 'startTimer':
        this.startTimer(stmt.timer, stmt.idLocal?.name ?? null);
        return;
      case 'clearTimer':
        this.scoped(() => fn.abc('TCLEAR', this.valueAs(stmt.id, 'int').index));
        return;
      case 'clearList':
        fn.abx('CLEAR', 0, this.listPool(stmt.list).slot.index);
        return;
      case 'appendItem':
        this.scoped(() => this.appendItem(stmt));
        return;
      case 'dropLastItem':
        fn.abx('POP', 0, this.listPool(stmt.list).slot.index);
        return;
      case 'keepFirstItems':
        this.scoped(() =>
          fn.abx(
            'KEEP',
            this.load('int', stmt.count).index,
            this.listPool(stmt.list).slot.index,
          ),
        );
        return;
      case 'sliceItems':
        // app_slice_len is slice(0, end), as SLICE counts it.
        this.scoped(() =>
          fn.abc(
            'SLICE',
            this.load('int', 0).index,
            this.valueAs(stmt.end, 'int').index,
            0,
            this.listPool(stmt.list).slot.index,
          ),
        );
        return;
      case 'stopAnimations':
      case 'startAnimations':
      case 'startChain':
      case 'completeAfter':
        animationStatement(this, stmt, this.program.animations);
        return;
      case 'return':
        if (!this.onReturn) {
          throw new Error('AOT internal: a `return` outside an effect');
        }
        fn.branch('JMP', this.onReturn);
        return;
      case 'armCleanup':
        this.scoped(() =>
          this.set(
            required(this.program.armed.get(stmt.flag), stmt.flag),
            this.load('bool', 1),
          ),
        );
        return;
      case 'updateVector':
        updateVector(this, stmt, this.program.vectors);
    }
  }

  /**
   * Appends an item, its fields worked out in order into consecutive registers. A full list drops it, as C does.
   * A text field is built in its own buffer first.
   *
   * @param stmt  The append.
   */
  private appendItem(stmt: IrAppendItem): void {
    const {fn} = this;
    const pool = this.listPool(stmt.list);
    const row = fn.consecutive(
      stmt.values.map(({field}) =>
        field.kind === 'string' ? 'string' : field.kind,
      ),
    );
    stmt.values.forEach((itemValue, index) =>
      this.scoped(() => {
        if ('text' in itemValue) {
          const buffer = required(
            pool.buffers.get(itemValue.field.key),
            `field "${itemValue.field.key}"`,
          );
          this.text(itemValue.text.parts);
          fn.abx('FMTSET', 0, buffer.index);
          fn.abx('GETS', row[index].index, buffer.index);
        } else {
          fn.abc(
            'MOVE',
            row[index].index,
            this.valueAs(itemValue.value, itemValue.field.kind).index,
          );
        }
      }),
    );
    fn.abx('APPEND', row[0].index, pool.slot.index);
  }

  /**
   * Starts a timer, keeping its id in a local when the app names it.
   *
   * @param timer  The timer.
   * @param idLocal  The local's C name, or null.
   */
  private startTimer(timer: IrTimer, idLocal: string | null): void {
    const {fn} = this;
    const hoisted =
      idLocal === null ? undefined : this.program.hoisted.get(idLocal);
    const start = (id: Register): void =>
      this.scoped(() => {
        fn.abc(
          'TSTART',
          id.index,
          this.timerDelay(timer.delay).index,
          timer.isRepeating ? 1 : 0,
          required(
            this.program.timerFunctions.get(timer.fn),
            `timer "${timer.fn}"`,
          ),
        );
        if (hoisted) {
          this.set(hoisted, id);
        }
      });
    if (idLocal === null || hoisted) {
      this.scoped(() => start(fn.register('int')));
      if (hoisted) {
        this.locals.set(idLocal!, {slot: hoisted});
      }
      return;
    }
    // The id outlives this statement, so its register is taken outside the scope.
    const id = fn.register('int');
    start(id);
    this.locals.set(idLocal, {register: id});
  }

  /**
   * A timer's delay in ms, as the C output converts it: a 64-bit or fractional delay through ToInt32, with a
   * negative one as 0. (The runtime, like er_timer_add, waits at least 1 ms.)
   *
   * @param delay  The delay, or null for none.
   *
   * @returns An int register.
   */
  private timerDelay(delay: IrExpr | null): Register {
    if (!delay) {
      return this.load('int', 0);
    }
    const kind = delay.type.kind;
    if (kind !== 'int64' && kind !== 'float') {
      return this.valueAs(delay, 'int');
    }

    // app_delay_msf takes a float, and makes it a 64-bit int as app_delay_ms64 takes; the VM's saturates.
    const {fn} = this;
    const whole =
      kind === 'float'
        ? this.convert(this.valueAs(delay, 'float'), 'int64')
        : this.valueAs(delay, 'int64');
    const modulus = this.load('int64', TWO_TO_32);
    const low = fn.register('int64');
    fn.abc('MODL', low.index, whole.index, modulus.index);
    fn.abc('ADDL', low.index, low.index, modulus.index);
    fn.abc('MODL', low.index, low.index, modulus.index);
    const tooBig = fn.register('bool');
    fn.abc('LLT', tooBig.index, this.load('int64', INT32_MAX).index, low.index);
    return this.choose(
      tooBig,
      () => this.load('int', 0),
      () => this.convert(low, 'int'),
      'int',
    );
  }

  private writeRef(ref: RefRecord, op: string, value: IrExpr): void {
    const slot = this.refSlot(ref);
    if (op === '=') {
      this.set(slot, this.value(value));
      return;
    }
    // `ref op= value` is `ref = ref op value`, worked out in the type C promotes the two to.
    const type = usual(slotType(slot), this.typeOf(value));
    const register = this.fn.register(type as RegisterType);
    this.fn.abc(
      ARITHMETIC_OPS[type][op.slice(0, -1)],
      register.index,
      this.convert(this.get(slot), type).index,
      this.valueAs(value, type).index,
    );
    this.set(slot, register);
  }

  /**
   * Runs a piece of lowering, then releases the registers it took.
   *
   * @param lower  The lowering.
   */
  scoped(lower: () => void): void {
    const checkpoint = this.fn.checkpoint();
    lower();
    this.fn.release(checkpoint);
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Props
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Sets a state-driven prop in the props scratch, converting its value as C assigns it to the ERProps field.
   *
   * @param field  The ERProps field.
   * @param value  Its value.
   */
  setProp(field: string, value: IrPropValue): void {
    this.scoped(() => {
      const prop = abiProp(field);
      const result = this.propValue(value);
      const {fn} = this;
      if (result.conversion === 'opacity' || prop.kind === 'float') {
        // The runtime's PSETF on an opacity is app_opacity's rule.
        fn.abx('PSETF', this.convert(result.register, 'float').index, prop.id);
        return;
      }
      if (result.conversion === 'dimension') {
        this.setDimension(prop.id, result.register);
        return;
      }
      // Any other field is a whole number: a fraction goes in truncated, as C converts it.
      fn.abx('PSETI', this.convert(result.register, 'int').index, prop.id);
    });
  }

  /**
   * A state-driven prop value, such as a paint's color, in the type C works it out in.
   *
   * @param value  The value.
   *
   * @returns The register.
   */
  propRegister(value: IrPropValue): Register {
    const result = this.propValue(value);
    if (result.conversion) {
      notYet(`a ${result.conversion} outside a prop`);
    }
    return result.register;
  }

  /**
   * A dimension, as app_round_dim rounds it: halves up, clamped to int16, NaN as 0.
   *
   * @param prop  The prop's id.
   * @param value  The value.
   */
  private setDimension(prop: number, value: Register): void {
    const {fn} = this;
    if (value.type === 'int' || value.type === 'bool') {
      fn.abx('PSETI', this.convert(value, 'int').index, prop);
      return;
    }
    if (value.type === 'float') {
      // The runtime rounds a float the way app_round_dim does.
      fn.abx('PSETF', value.index, prop);
      return;
    }
    // A double (or a 64-bit int, which C makes a double) is rounded here, since a float can't hold it exactly.
    const rounded = fn.register('double');
    fn.abc(
      'DADD',
      rounded.index,
      this.convert(value, 'double').index,
      this.load('double', 0.5).index,
    );
    fn.abc('DMATH', rounded.index, rounded.index, rounded.index, MathFn.floor);
    fn.abx('PSETI', this.convert(rounded, 'int').index, prop);
  }

  /**
   * A state-driven prop value, with the conversion C applies on top of it lifted out. A conversion that maps 0 to
   * 0 (app_opacity, app_round_dim) gives the same answer applied after a choice as inside it.
   *
   * @param value  The value.
   *
   * @returns Its register, C type and conversion.
   */
  private propValue(value: IrPropValue): PropResult {
    const plain = (register: Register): PropResult => ({
      register,
      type: register.type as CType,
      conversion: null,
    });
    switch (value.kind) {
      case 'constant':
        return plain(this.constantValue(value.code));
      case 'opacity':
      case 'dimension':
        return {
          ...plain(this.value(value.value)),
          conversion: value.kind,
        };
      case 'float':
        return plain(this.valueAs(value.value, 'float'));
      case 'byte':
        return this.propValue(value.value);
      case 'visible':
        // C tests the value itself, and a string's pointer is never null.
        return plain(
          this.display(
            value.value.type.kind === 'string'
              ? this.load('bool', 1)
              : this.condition(value.value),
          ),
        );
      case 'shown':
        return plain(this.display(this.shown(value.show)));
      case 'flag': {
        const test =
          value.truthiness === 'value' && value.test.type.kind === 'string'
            ? this.load('bool', 1)
            : this.condition(value.test);
        const register = this.fn.register('int');
        const end = this.fn.label();
        this.fn.abx('LOADI', register.index, 0);
        this.fn.branch(value.negate ? 'JT' : 'JF', end, test);
        this.fn.abx('LOADI', register.index, value.whenTrue);
        this.fn.place(end);
        return plain(register);
      }
      case 'whileShown': {
        const inner = this.propValue(value.value);
        return {
          ...plain(
            this.choose(
              this.shown(value.show),
              () => inner.register,
              () => this.load(inner.type, 0),
              inner.type,
            ),
          ),
          conversion: inner.conversion,
        };
      }
      case 'select': {
        const test = this.condition(value.test);
        const whenTrue = this.propValue(value.whenTrue);
        const whenFalse = this.propValue(value.whenFalse);
        if (whenTrue.conversion !== whenFalse.conversion) {
          notYet('a state-driven prop that converts one of its choices only');
        }
        const type = usual(whenTrue.type, whenFalse.type);
        return {
          ...plain(
            this.choose(
              test,
              () => this.convert(whenTrue.register, type),
              () => this.convert(whenFalse.register, type),
              type,
            ),
          ),
          conversion: whenTrue.conversion,
        };
      }
    }
  }

  /**
   * A constant prop value as the C output spells it, in a register of the type C gives it.
   *
   * @param code  The C.
   *
   * @returns The register.
   */
  private constantValue(code: string): Register {
    const value = readConstant(code);
    if (value === null) {
      return notYet(`the constant \`${code}\``);
    }
    const isFraction = !/0x/i.test(code) && /[.e]|f\b|f$/i.test(code);
    return this.load(isFraction ? 'float' : 'int', value);
  }

  /** ER_DISPLAY_FLEX while a test holds, else ER_DISPLAY_NONE. */
  private display(test: Register): Register {
    const register = this.fn.register('int');
    const end = this.fn.label();
    this.fn.abx('LOADI', register.index, DISPLAY_NONE);
    this.fn.branch('JF', end, test);
    this.fn.abx('LOADI', register.index, DISPLAY_FLEX);
    this.fn.place(end);
    return register;
  }

  /**
   * Whether a node under a state-driven conditional shows: its test, negated for an else branch, within the
   * conditions it is nested in.
   *
   * @param show  The condition.
   *
   * @returns A bool register.
   */
  shown(show: IrShow): Register {
    if (show.kind === 'rowInUse') {
      const count = this.fn.register('int');
      const register = this.fn.register('bool');
      this.fn.abx('LEN', count.index, this.listPool(show.list).slot.index);
      this.fn.abc(
        'LT',
        register.index,
        this.load('int', show.row).index,
        count.index,
      );
      return register;
    }
    const register = this.fn.register('bool');
    this.fn.abc('MOVE', register.index, this.condition(show.test).index);
    if (show.negate) {
      this.fn.abc('NOT', register.index, register.index);
    }
    if (show.within) {
      const end = this.fn.label();
      this.fn.branch('JF', end, register);
      this.fn.abc('MOVE', register.index, this.shown(show.within).index);
      this.fn.place(end);
    }
    return register;
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Slots
   -------------------------------------------------------------------------------------------------------------------*/

  stateSlot(state: ScalarState): Slot {
    return required(this.program.states.get(state), `state "${state.name}"`);
  }

  listPool(list: ListState): ListPool {
    return required(this.program.lists.get(list), `list "${list.name}"`);
  }

  refSlot(ref: RefRecord): Slot {
    return ref.kind === 'node'
      ? notYet('reading a node ref')
      : required(this.program.refs.get(ref), `ref "${ref.cVar}"`);
  }

  bootSlot(slot: IrBootSlot): Slot {
    return required(this.program.boot.get(slot), `boot value "${slot.field}"`);
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses what the package output can't lower yet.
 *
 * @param what  What the app uses.
 */
export function notYet(what: string): never {
  throw aotError(
    `AOT: the package output doesn't support ${what} yet`,
    'build with --c to get the C output, which supports it.',
  );
}

/**
 * A slot or function that has to have been planned.
 *
 * @param planned  The slot or function, if it was.
 * @param what  What it holds, for the error.
 *
 * @returns The slot or function.
 */
function required<T>(planned: T | undefined, what: string): T {
  if (planned === undefined) {
    throw new Error(`AOT internal: nothing planned for ${what}`);
  }
  return planned;
}

/**
 * The C type a slot holds.
 *
 * @param slot  The slot.
 *
 * @returns Its C type.
 */
export function slotType(slot: Slot): CType {
  return (
    {
      1: 'int',
      2: 'bool',
      3: 'float',
      4: 'int64',
      5: 'double',
      6: 'string',
      7: 'enum',
    } as Record<number, CType>
  )[slot.type];
}

/**
 * The type C works out an operation on two values in: the wider of the two, a double over a float over a 64-bit
 * int over an int. Two booleans stay a boolean (C's int holding 0 or 1).
 *
 * @param left  One operand's C type.
 * @param right  The other's.
 *
 * @returns The type.
 */
function usual(left: CType, right: CType): CType {
  if (left === 'string' || right === 'string') {
    return 'string';
  }
  if (left === 'bool' && right === 'bool') {
    return 'bool';
  }
  for (const type of ['double', 'float', 'int64'] as const) {
    if (left === type || right === type) {
      return type;
    }
  }
  return 'int';
}

/** A boolean worked on as a number is C's int. */
const promoted = (type: CType): CType => (type === 'bool' ? 'int' : type);

/**
 * The type a handler local holds: what its IR type says, as C declares it.
 *
 * @param init  The local's initial value.
 *
 * @returns Its type.
 */
function declaredType(init: IrExpr): CType {
  switch (init.type.kind) {
    case 'bool':
      return 'bool';
    case 'float':
      return 'float';
    case 'int64':
      return 'int64';
    default:
      return 'int';
  }
}

/**
 * A double's 64 bits.
 *
 * @param value  The number.
 *
 * @returns The bits.
 */
function doubleBits(value: number): bigint {
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, value, true);
  return view.getBigUint64(0, true);
}
