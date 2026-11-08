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
 * The VM converts nothing by itself, so each value here is worked out in a type of its own, and converted where the
 * language needs it: an int beside a float is a float, a value worked out at boot is a double, and an assignment
 * converts to the field it writes.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError, constantText} from '../diagnostics.mts';
import {animationStatement} from './animations.mts';
import {EventField, MathFn, ValueType} from './format.mts';
import {abiProp, constantNumber, floatBits} from './props.mts';
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
import type {IrTextPad, IrTextPart} from '../ir/text.mts';
import type {
  ListState,
  Local,
  PropConstant,
  RefRecord,
  ScalarState,
} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** The type a value is worked out in. A boolean is an int holding 0 or 1; an enum is an interned string. */
export type ExprType =
  'int' | 'bool' | 'float' | 'double' | 'int64' | 'string' | 'enum';

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

/** A PanResponder's gesture state: whether it is granted, the travel at the grant, and x0/y0. */
export interface GestureSlots {
  granted: Slot;
  baseDeltaX: Slot;
  baseDeltaY: Slot;
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
  /** The buffer each string local is copied into, by its name. */
  stringLocals: Map<string, Slot>;
  /** The string states held as interned enums. */
  enums: Map<ScalarState, EnumSet>;
  /** The slot each effect local its cleanup reads is kept in, by its name. */
  hoisted: Map<string, Slot>;
  /** Each effect's flag that its last run reached its cleanup, by the flag's name. */
  armed: Map<string, Slot>;
  /** Each timer callback's function, by its name. */
  timerFunctions: Map<string, number>;
  animations: AnimationPlan;
  /** Each PanResponder's gesture state, by its prefix. */
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

/** A state-driven prop value worked out, and the conversion applied to it on its way into the field. */
interface PropResult {
  register: Register;
  type: ExprType;
  conversion: 'opacity' | 'dimension' | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** What rounding a double adds before it floors: a half rounds up. */
const ROUND_HALF_UP = 0.5;

/** The whole-number helpers' opcodes; the 64-bit ones add L. */
const HELPER_OPCODES: Record<WholeHelper, string> = {
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

/** The arithmetic opcodes for each type. */
const ARITHMETIC_OPCODES: Record<string, Record<string, string>> = {
  int: {'+': 'ADD', '-': 'SUB', '*': 'MUL', '/': 'DIV', '%': 'MOD'},
  int64: {'+': 'ADDL', '-': 'SUBL', '*': 'MULL', '/': 'DIVL', '%': 'MODL'},
  float: {'+': 'FADD', '-': 'FSUB', '*': 'FMUL', '/': 'FDIV'},
  double: {'+': 'DADD', '-': 'DSUB', '*': 'DMUL', '/': 'DDIV'},
};

/** The comparison opcodes for each type: equal, less than, at most. */
const COMPARE_OPCODES: Record<string, [string, string, string]> = {
  int: ['EQ', 'LT', 'LE'],
  int64: ['LEQ', 'LLT', 'LLE'],
  float: ['FEQ', 'FLT', 'FLE'],
  double: ['DEQ', 'DLT', 'DLE'],
};

/** The conversion opcodes, by source and destination type. */
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
  width: EventField.layoutWidth,
  height: EventField.layoutHeight,
};

/** The engine's display values. */
const DISPLAY_FLEX = 0;
const DISPLAY_NONE = 1;

/** A timer delay is ToInt32 of the value: its low 32 bits. */
const TWO_TO_32 = 2 ** 32;
const INT32_MAX = 0x7fffffff;

/** The int range, for an immediate that fits LOADI. */
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
  readonly builder: FunctionBuilder;
  private readonly program: ProgramSlots;
  private readonly inDoubles: boolean;
  private readonly input: InputText | null;
  private readonly onReturn: Label | null;
  private readonly row: Register | null;
  private readonly locals = new Map<string, LocalBinding>();
  /** A string register nothing writes, so it stays the empty string. Reserved, so no release reuses it. */
  private emptyString: Register | null = null;

  /**
   * @param builder  The function being built.
   * @param program  The program's slots and tables.
   * @param options  inDoubles: fractions are worked out in double precision, for a value worked out at boot.
   *                 input: the <TextInput> a CHANGE_TEXT handler reads.
   *                 onReturn: where an effect's early `return` goes.
   *                 row: the row a list row's handler was built for, which its item reads.
   *                 finished: what an animation completion was passed, which its `finished` reads.
   */
  constructor(
    builder: FunctionBuilder,
    program: ProgramSlots,
    options: {
      inDoubles?: boolean;
      input?: InputText;
      onReturn?: Label;
      row?: Register;
      finished?: Register;
    } = {},
  ) {
    this.builder = builder;
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
   * The type an expression is worked out in.
   *
   * @param expression  The expression.
   *
   * @returns Its type.
   */
  typeOf(expression: IrExpr): ExprType {
    const fraction: ExprType = this.inDoubles ? 'double' : 'float';
    switch (expression.kind) {
      case 'number':
        return expression.type.kind === 'float'
          ? fraction
          : expression.type.kind === 'int64'
            ? 'int64'
            : expression.type.kind === 'bool'
              ? 'bool'
              : 'int';
      case 'string':
        return 'string';
      case 'boolean':
        return 'bool';
      case 'state':
        // An interned state still reads as a string.
        return this.program.enums.has(expression.state)
          ? 'string'
          : slotType(this.stateSlot(expression.state));
      case 'boot':
        return slotType(this.bootSlot(expression.slot));
      case 'ref':
        return slotType(this.refSlot(expression.ref));
      case 'local': {
        if (expression.local.expression) {
          return this.typeOf(expression.local.expression);
        }
        const binding = this.binding(expression.local.name!);
        return !binding
          ? 'string'
          : 'register' in binding
            ? (binding.register.type as ExprType)
            : slotType(binding.slot);
      }
      case 'event':
        return expression.field === 'changed_text'
          ? 'string'
          : ['vx', 'vy', 'value', 'value_start'].includes(expression.field)
            ? 'float'
            : 'int';
      case 'gesture':
        return expression.field === 'vx' || expression.field === 'vy'
          ? 'float'
          : 'int';
      case 'clock':
        return 'int64';
      case 'itemField':
        return expression.field.kind;
      case 'pi':
      case 'toFloat':
      case 'floatDivide':
      case 'floatMath':
        return fraction;
      case 'unary':
        return expression.operator === '!'
          ? 'bool'
          : promoted(this.typeOf(expression.operand));
      case 'helper':
        return expression.bits === 64 ? 'int64' : 'int';
      case 'arithmetic':
        return usual(
          this.typeOf(expression.left),
          this.typeOf(expression.right),
        );
      case 'wholeRound':
        return this.typeOf(expression.operand);
      case 'compare':
      case 'stringEquals':
      case 'logical':
        return 'bool';
      case 'pick':
        return usual(
          this.typeOf(expression.left),
          this.typeOf(expression.right),
        );
      case 'conditional':
        return usual(
          this.typeOf(expression.consequent),
          this.typeOf(expression.alternate),
        );
      default:
        // layout, narrowRemainder, trunc, toInt, and what a package refuses
        return 'int';
    }
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Values
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Works an expression out into a register of its type.
   *
   * @param expression  The expression.
   *
   * @returns The register. A local's may be its own: never write to what this returns.
   */
  value(expression: IrExpr): Register {
    const {builder} = this;
    const type = this.typeOf(expression);
    switch (expression.kind) {
      case 'number':
        return this.load(type, expression.value);
      case 'string': {
        const register = builder.register('string');
        builder.abx(
          'LOADS',
          register.index,
          this.program.tables.string(expression.value),
        );
        return register;
      }
      case 'boolean':
        return this.load('bool', expression.value ? 1 : 0);
      case 'state':
        if (this.program.enums.has(expression.state)) {
          throw new Error(
            `AOT internal: state "${expression.state.name}" is interned, but read as a string`,
          );
        }
        return this.get(this.stateSlot(expression.state));
      case 'boot':
        return this.get(this.bootSlot(expression.slot));
      case 'ref':
        return this.get(this.refSlot(expression.ref));
      case 'local':
        return this.local(expression);
      case 'listLength': {
        const register = builder.register('int');
        builder.abx(
          'LEN',
          register.index,
          this.listPool(expression.list).slot.index,
        );
        return register;
      }
      case 'itemField':
        return this.itemField(expression.item, expression.field.key, type);
      case 'gesture':
        return this.gesture(expression);
      case 'event':
        return expression.field === 'changed_text'
          ? this.changedText()
          : this.event(type, EVENT_FIELDS[expression.field]);
      case 'layout':
        return this.event('int', LAYOUT_FIELDS[expression.field]);
      case 'clock': {
        const register = builder.register('int64');
        builder.abc(
          expression.clock === 'Date' ? 'DATENOW' : 'NOW',
          register.index,
        );
        return register;
      }
      case 'pi':
        return this.load(type, Math.PI);
      case 'toFloat':
      case 'wholeRound':
        return this.convert(this.value(expression.operand), type);
      case 'unary':
        return this.unary(expression.operator, expression.operand, type);
      case 'helper': {
        const argumentType: ExprType = expression.bits === 64 ? 'int64' : 'int';
        const argumentRegisters = expression.callArguments.map(argument =>
          this.valueAs(argument, argumentType),
        );
        const register = builder.register(argumentType);
        builder.abc(
          HELPER_OPCODES[expression.helper] +
            (expression.bits === 64 ? 'L' : ''),
          register.index,
          argumentRegisters[0].index,
          (argumentRegisters[1] ?? argumentRegisters[0]).index,
        );
        return register;
      }
      case 'arithmetic':
        return this.arithmetic(
          expression.operator,
          expression.left,
          expression.right,
          type,
        );
      case 'floatDivide':
        return this.arithmetic('/', expression.left, expression.right, type);
      case 'narrowRemainder': {
        const operandType = usual(
          this.typeOf(expression.left),
          this.typeOf(expression.right),
        );
        return this.convert(
          this.arithmetic('%', expression.left, expression.right, operandType),
          'int',
        );
      }
      case 'floatMath': {
        const argumentRegisters = expression.callArguments.map(argument =>
          this.valueAs(argument, type),
        );
        const register = builder.register(type as RegisterType);
        builder.abc(
          type === 'double' ? 'DMATH' : 'FMATH',
          register.index,
          argumentRegisters[0].index,
          (argumentRegisters[1] ?? argumentRegisters[0]).index,
          MathFn[expression.mathFunction],
        );
        return register;
      }
      case 'trunc':
      case 'toInt':
        // Worked out as a float, or a double at boot.
        return this.convert(
          this.valueAs(expression.operand, this.inDoubles ? 'double' : 'float'),
          'int',
        );
      case 'compare':
        return this.compare(
          expression.operator,
          expression.left,
          expression.right,
        );
      case 'stringEquals': {
        if (this.enumOf(expression.left) || this.enumOf(expression.right)) {
          return this.enumEquals(
            expression.operator,
            expression.left,
            expression.right,
          );
        }
        const register = builder.register('bool');
        builder.abc(
          'SEQ',
          register.index,
          this.valueAs(expression.left, 'string').index,
          this.valueAs(expression.right, 'string').index,
        );
        if (expression.operator === '!=') {
          builder.abc('NOT', register.index, register.index);
        }
        return register;
      }
      case 'logical': {
        const register = builder.register('bool');
        const end = builder.label();
        builder.abc(
          'MOVE',
          register.index,
          this.condition(expression.left).index,
        );
        builder.branch(
          expression.operator === '&&' ? 'JF' : 'JT',
          end,
          register,
        );
        builder.abc(
          'MOVE',
          register.index,
          this.condition(expression.right).index,
        );
        builder.place(end);
        return register;
      }
      case 'pick':
        return this.pick(
          expression.operator,
          expression.left,
          expression.right,
          type,
        );
      case 'conditional':
        return this.choose(
          this.condition(expression.test),
          () => this.valueAs(expression.consequent, type),
          () => this.valueAs(expression.alternate, type),
          type,
        );
    }
  }

  /**
   * The set of an interned state an expression reads, or null when it reads none.
   *
   * @param expression  The expression.
   *
   * @returns The set.
   */
  private enumOf(expression: IrExpr): EnumSet | null {
    return expression.kind === 'state'
      ? (this.program.enums.get(expression.state) ?? null)
      : null;
  }

  /**
   * A comparison of an interned state with a literal, or with itself, as an int compare. A literal it never
   * holds is never equal.
   *
   * @param operator  `==` or `!=`.
   * @param left  One side.
   * @param right  The other.
   *
   * @returns A bool register.
   */
  private enumEquals(
    operator: '==' | '!=',
    left: IrExpr,
    right: IrExpr,
  ): Register {
    const read = (this.enumOf(left) ? left : right) as IrExpr & {kind: 'state'};
    const other = read === left ? right : left;
    const {set, values} = this.enumOf(read)!;
    if (other.kind !== 'string') {
      return this.load('bool', operator === '==' ? 1 : 0);
    }
    const index = values.indexOf(other.value);
    if (index < 0) {
      return this.load('bool', operator === '==' ? 0 : 1);
    }
    const register = this.builder.register('bool');
    this.builder.abc(
      'EQ',
      register.index,
      this.get(this.stateSlot(read.state)).index,
      this.enumValue(set, index).index,
    );
    if (operator === '!=') {
      this.builder.abc('NOT', register.index, register.index);
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
    const register = this.builder.register('enum');
    this.builder.abx('LOADE', register.index, index, set);
    return register;
  }

  /**
   * Works an expression out and converts it as an assignment to a type does.
   *
   * @param expression  The expression.
   * @param type  The type it goes into.
   *
   * @returns A register of that type.
   */
  valueAs(expression: IrExpr, type: ExprType): Register {
    return this.convert(this.value(expression), type);
  }

  /**
   * An expression as a condition: true exactly when JS finds it truthy. A string is truthy
   * when it isn't empty.
   *
   * @param expression  The expression.
   *
   * @returns A bool register.
   */
  condition(expression: IrExpr): Register {
    return this.truthy(this.value(expression));
  }

  /**
   * Converts a register to another type, as an assignment does. A number to a boolean is a test against
   * zero; a boolean to a number is 0 or 1.
   *
   * @param register  The register.
   * @param type  The type.
   *
   * @returns A register of that type: this one when it already is.
   */
  convert(register: Register, type: ExprType): Register {
    const from = register.type as ExprType;
    if (from === type) {
      return register;
    }
    if (type === 'bool') {
      return this.truthy(register);
    }
    if (from === 'bool') {
      const whole = this.builder.register('int');
      const end = this.builder.label();
      this.builder.abx('LOADI', whole.index, 0);
      this.builder.branch('JF', end, register);
      this.builder.abx('LOADI', whole.index, 1);
      this.builder.place(end);
      return this.convert(whole, type);
    }
    const opcode = CONVERSIONS[`${from}>${type}`];
    if (!opcode) {
      throw new Error(`AOT internal: no conversion from ${from} to ${type}`);
    }
    const converted = this.builder.register(type as RegisterType);
    this.builder.abc(opcode, converted.index, register.index);
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
    const test = this.builder.register('bool');
    if (register.type === 'string') {
      this.builder.abc(
        'SEQ',
        test.index,
        register.index,
        this.zero('string').index,
      );
    } else {
      const zero = this.load(register.type as ExprType, 0);
      this.builder.abc(
        COMPARE_OPCODES[register.type][0],
        test.index,
        register.index,
        zero.index,
      );
    }
    this.builder.abc('NOT', test.index, test.index);
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
  load(type: ExprType, value: number): Register {
    const {builder} = this;
    const {tables} = this.program;
    const register = builder.register(type as RegisterType);
    switch (type) {
      case 'int':
      case 'bool': {
        const whole = Math.trunc(value) | 0;
        if (whole >= IMMEDIATE_MIN && whole <= IMMEDIATE_MAX) {
          builder.abx('LOADI', register.index, whole);
        } else {
          builder.abx('LOADK', register.index, tables.constant32(whole));
        }
        break;
      }
      case 'float':
        builder.abx(
          'LOADK',
          register.index,
          tables.constant32(floatBits(value)),
        );
        break;
      case 'double':
        builder.abx(
          'LOADK64',
          register.index,
          tables.constant64(doubleBits(value)),
        );
        break;
      case 'int64':
        builder.abx(
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
   * Whether two registers of one type hold the same value.
   *
   * @param left  One.
   * @param right  The other.
   *
   * @returns A bool register.
   */
  same(left: Register, right: Register): Register {
    const register = this.builder.register('bool');
    const opcode =
      left.type === 'string'
        ? 'SEQ'
        : left.type === 'bool' || left.type === 'enum'
          ? 'EQ'
          : COMPARE_OPCODES[left.type][0];
    this.builder.abc(opcode, register.index, left.index, right.index);
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
    const register = this.builder.register(type as RegisterType);
    const opcode =
      type === 'string'
        ? 'GETS'
        : type === 'int64' || type === 'double'
          ? 'GET64'
          : 'GET32';
    this.builder.abx(opcode, register.index, slot.index);
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
    const opcode =
      type === 'string'
        ? 'SETS'
        : type === 'int64' || type === 'double'
          ? 'SET64'
          : 'SET32';
    this.builder.abx(opcode, register.index, slot.index);
  }

  private event(type: ExprType, field: number): Register {
    const register = this.builder.register(type as RegisterType);
    this.builder.abc('EVT', register.index, field);
    return register;
  }

  /**
   * A gestureState field: travel from the grant, x0/y0 from the responder's state, the rest
   * off the event.
   *
   * @param expression  The field.
   *
   * @returns The register.
   */
  private gesture(expression: IrGesture): Register {
    const slots = this.program.gestures.get(expression.statePrefix)!;
    switch (expression.field) {
      case 'dx':
      case 'dy': {
        const travel = this.event('int', EVENT_FIELDS[expression.field]);
        const base = this.get(
          expression.field === 'dx' ? slots.baseDeltaX : slots.baseDeltaY,
        );
        const register = this.builder.register('int');
        this.builder.abc('SUB', register.index, travel.index, base.index);
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
        return this.event('float', EVENT_FIELDS[expression.field]);
      case 'numberActiveTouches':
        return this.event('int', EventField.touches);
      case 'stateID':
        return this.load('int', expression.stateID);
    }
  }

  /** The text a <TextInput> holds now, as its CHANGE_TEXT handler receives it. */
  private changedText(): Register {
    if (!this.input) {
      throw new Error(
        'AOT internal: changed text outside a CHANGE_TEXT handler',
      );
    }
    const node = this.builder.register('node');
    this.builder.abx('GET64', node.index, this.input.node.index);
    this.builder.abx('INPUT', node.index, this.input.text.index);
    return this.get(this.input.text);
  }

  /**
   * A field of a list row's item. A row past the list's end, as a hidden row is, reads as zero or "".
   *
   * @param item  The row's item.
   * @param key  The field.
   * @param type  Its type.
   *
   * @returns The register.
   */
  private itemField(item: Local, key: string, type: ExprType): Register {
    const {builder} = this;
    const list = required(item.list, `the list of "${item.name}"`);
    const field = list.struct.fields.findIndex(entry => entry.key === key);
    const pool = this.listPool(list).slot;
    const row = this.row ?? this.load('int', required(item.row, item.name!));
    const count = builder.register('int');
    const inUse = builder.register('bool');
    builder.abx('LEN', count.index, pool.index);
    builder.abc('LT', inUse.index, row.index, count.index);
    return this.choose(
      inUse,
      () => {
        const register = builder.register(type as RegisterType);
        builder.abc(
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
  private zero(type: ExprType): Register {
    if (type !== 'string') {
      return this.load(type, 0);
    }
    this.emptyString ??= this.builder.reserved('string');
    return this.emptyString;
  }

  private local(expression: IrExpr & {kind: 'local'}): Register {
    // A memo, a component's prop or a helper's argument stands for its expression.
    if (expression.local.expression) {
      return this.value(expression.local.expression);
    }
    const binding = this.binding(expression.local.name!);
    if (!binding) {
      return notYet('the value an element passes its callback');
    }
    return 'register' in binding ? binding.register : this.get(binding.slot);
  }

  /** A local's value, by name: this function's own, or an effect local kept in a slot. */
  private binding(name: string): LocalBinding | undefined {
    const hoisted = this.program.hoisted.get(name);
    return this.locals.get(name) ?? (hoisted && {slot: hoisted});
  }

  private unary(
    operator: '-' | '+' | '!',
    operand: IrExpr,
    type: ExprType,
  ): Register {
    if (operator === '!') {
      const register = this.builder.register('bool');
      this.builder.abc('NOT', register.index, this.condition(operand).index);
      return register;
    }
    const value = this.valueAs(operand, type);
    if (operator === '+') {
      return value;
    }
    const register = this.builder.register(type as RegisterType);
    const negateOpcode = {
      int: 'NEG',
      int64: 'NEGL',
      float: 'FNEG',
      double: 'DNEG',
    }[type as string];
    this.builder.abc(negateOpcode!, register.index, value.index);
    return register;
  }

  private arithmetic(
    operator: string,
    left: IrExpr,
    right: IrExpr,
    type: ExprType,
  ): Register {
    const leftRegister = this.valueAs(left, type);
    const rightRegister = this.valueAs(right, type);
    const register = this.builder.register(type as RegisterType);
    // The IR's float remainder is fmod.
    if (operator === '%' && (type === 'float' || type === 'double')) {
      this.builder.abc(
        type === 'double' ? 'DMATH' : 'FMATH',
        register.index,
        leftRegister.index,
        rightRegister.index,
        MathFn.mod,
      );
      return register;
    }
    this.builder.abc(
      ARITHMETIC_OPCODES[type][operator],
      register.index,
      leftRegister.index,
      rightRegister.index,
    );
    return register;
  }

  private compare(operator: string, left: IrExpr, right: IrExpr): Register {
    const type = promoted(usual(this.typeOf(left), this.typeOf(right)));
    const leftRegister = this.valueAs(left, type);
    const rightRegister = this.valueAs(right, type);
    const [equal, less, atMost] = COMPARE_OPCODES[type];
    const register = this.builder.register('bool');
    const [opcode, first, second] =
      operator === '<'
        ? [less, leftRegister, rightRegister]
        : operator === '>'
          ? [less, rightRegister, leftRegister]
          : operator === '<='
            ? [atMost, leftRegister, rightRegister]
            : operator === '>='
              ? [atMost, rightRegister, leftRegister]
              : [equal, leftRegister, rightRegister];
    this.builder.abc(opcode, register.index, first.index, second.index);
    if (operator === '!=') {
      this.builder.abc('NOT', register.index, register.index);
    }
    return register;
  }

  /** `a || b` as `(a ? a : b)`, and `a && b` as `(a ? b : a)`. */
  private pick(
    operator: '&&' | '||',
    left: IrExpr,
    right: IrExpr,
    type: ExprType,
  ): Register {
    // A string slot always reads as set here.
    if (type === 'string') {
      return this.valueAs(operator === '||' ? left : right, type);
    }
    const leftValue = this.value(left);
    return this.choose(
      this.truthy(leftValue),
      () =>
        this.convert(operator === '||' ? leftValue : this.value(right), type),
      () =>
        this.convert(operator === '||' ? this.value(right) : leftValue, type),
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
    type: ExprType,
  ): Register {
    const {builder} = this;
    const register = builder.register(type as RegisterType);
    const otherwise = builder.label();
    const end = builder.label();
    builder.branch('JF', otherwise, test);
    builder.abc('MOVE', register.index, whenTrue().index);
    builder.branch('JMP', end);
    builder.place(otherwise);
    builder.abc('MOVE', register.index, whenFalse().index);
    builder.place(end);
    return register;
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Text
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Builds a text in the format buffer, each part printed in turn. The caller puts it
   * somewhere (FMTTEXT, FMTSET, or FMTSPAN), which empties the buffer.
   *
   * @param parts  The text's parts.
   */
  text(parts: IrTextPart[]): void {
    const {builder} = this;
    for (const part of parts) {
      const checkpoint = builder.checkpoint();
      if (part.kind === 'literal') {
        if (part.text) {
          builder.abx('FMTK', 0, this.program.tables.string(part.text));
        }
      } else if (part.kind === 'booleanWord') {
        builder.abc('FMTB', this.condition(part.value).index);
      } else if (this.enumOf(part.value)) {
        const value = part.value as IrExpr & {kind: 'state'};
        builder.abx(
          'FMTE',
          this.get(this.stateSlot(value.state)).index,
          this.enumOf(value)!.set,
        );
      } else {
        // The spec is by the value's IR type: a float is printed from a float, so a boot double is narrowed first.
        const type = part.value.type.kind;
        const [opcode, formatType]: [string, ExprType] =
          type === 'string'
            ? ['FMTS', 'string']
            : type === 'float'
              ? ['FMTF', 'float']
              : type === 'int64'
                ? ['FMTL', 'int64']
                : ['FMTI', 'int'];
        const register = this.valueAs(part.value, formatType).index;
        if (part.digits !== undefined && formatType === 'float') {
          builder.abc('FMTFX', register, part.digits);
          this.pads(part.pads, 0);
        } else if (part.digits) {
          // A whole number's decimals are all zeros, so pad the number to leave room for them.
          builder.abc(opcode, register);
          this.pads(part.pads, part.digits + 1);
          builder.abx(
            'FMTK',
            0,
            this.program.tables.string(`.${'0'.repeat(part.digits)}`),
          );
        } else {
          builder.abc(opcode, register);
          this.pads(part.pads, 0);
        }
      }
      builder.release(checkpoint);
    }
  }

  /**
   * Pads the piece just printed, as padStart would.
   *
   * @param pads  The padStart calls, in order.
   * @param suffix  How many bytes are still to be printed after the piece, which each width leaves room for.
   */
  private pads(pads: IrTextPad[] | undefined, suffix: number): void {
    for (const {width, fill} of pads ?? []) {
      if (width > suffix) {
        this.builder.abc('FMTPAD', width - suffix, fill.charCodeAt(0));
      }
    }
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Statements
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Lowers statements, in order.
   *
   * @param statements  The statements.
   */
  statements(statements: IrStmt[]): void {
    for (const statement of statements) {
      this.statement(statement);
    }
  }

  private statement(statement: IrStmt): void {
    const {builder} = this;
    builder.at(statement.location);
    switch (statement.kind) {
      case 'declareLocal': {
        const hoisted = this.program.hoisted.get(statement.name);
        if (statement.isHoisted && !hoisted) {
          throw new Error(
            `AOT internal: no slot for local "${statement.name}"`,
          );
        }
        if (hoisted && slotType(hoisted) !== 'string') {
          this.scoped(() =>
            this.set(hoisted, this.value(statement.initializer)),
          );
          this.locals.set(statement.name, {slot: hoisted});
          return;
        }
        if (this.typeOf(statement.initializer) === 'string') {
          // A string local is a copy of its own, so a later write to the state it was read from can't change it.
          const slot =
            hoisted ?? this.program.stringLocals.get(statement.name)!;
          const checkpoint = builder.checkpoint();
          this.text(
            statement.text?.parts ?? [
              {kind: 'value', value: statement.initializer},
            ],
          );
          builder.abx('FMTSET', 0, slot.index);
          builder.release(checkpoint);
          this.locals.set(statement.name, {slot});
          return;
        }
        const type = declaredType(statement.initializer);
        const register = builder.register(type as RegisterType);
        const checkpoint = builder.checkpoint();
        builder.abc(
          'MOVE',
          register.index,
          this.valueAs(statement.initializer, type).index,
        );
        builder.release(checkpoint);
        this.locals.set(statement.name, {register});
        return;
      }
      case 'setState':
        this.scoped(() =>
          this.set(
            this.stateSlot(statement.state),
            this.value(statement.value),
          ),
        );
        return;
      case 'setStateText':
        this.scoped(() => {
          // An interned state is set to the index of the constant it is given.
          const set = this.program.enums.get(statement.state);
          if (set) {
            const index = set.values.indexOf(constantText(statement.text)!);
            builder.abx(
              'SET32',
              this.enumValue(set.set, index).index,
              this.stateSlot(statement.state).index,
            );
            return;
          }
          this.text(statement.text.parts);
          builder.abx('FMTSET', 0, this.stateSlot(statement.state).index);
        });
        return;
      case 'writeRef':
        this.scoped(() =>
          this.writeRef(statement.ref, statement.operator, statement.value),
        );
        return;
      case 'stepRef':
        this.scoped(() => {
          const slot = this.refSlot(statement.ref);
          const type = slotType(slot);
          const register = builder.register(type as RegisterType);
          builder.abc(
            ARITHMETIC_OPCODES[type][statement.operator === '++' ? '+' : '-'],
            register.index,
            this.get(slot).index,
            this.load(type, 1).index,
          );
          this.set(slot, register);
        });
        return;
      case 'if': {
        const otherwise = builder.label();
        const checkpoint = builder.checkpoint();
        builder.branch('JF', otherwise, this.condition(statement.test));
        builder.release(checkpoint);
        this.statements(statement.consequent);
        if (statement.alternate) {
          const end = builder.label();
          builder.branch('JMP', end);
          builder.place(otherwise);
          this.statements(statement.alternate);
          builder.place(end);
        } else {
          builder.place(otherwise);
        }
        return;
      }
      case 'startTimer':
        this.startTimer(statement.timer, statement.idLocal?.name ?? null);
        return;
      case 'clearTimer':
        this.scoped(() =>
          builder.abc('TCLEAR', this.valueAs(statement.id, 'int').index),
        );
        return;
      case 'clearList':
        builder.abx('CLEAR', 0, this.listPool(statement.list).slot.index);
        return;
      case 'appendItem':
        this.scoped(() => this.appendItem(statement));
        return;
      case 'dropLastItem':
        builder.abx('POP', 0, this.listPool(statement.list).slot.index);
        return;
      case 'keepFirstItems':
        this.scoped(() =>
          builder.abx(
            'KEEP',
            this.load('int', statement.count).index,
            this.listPool(statement.list).slot.index,
          ),
        );
        return;
      case 'sliceItems':
        // slice(0, end), as SLICE counts it.
        this.scoped(() =>
          builder.abc(
            'SLICE',
            this.load('int', 0).index,
            this.valueAs(statement.end, 'int').index,
            0,
            this.listPool(statement.list).slot.index,
          ),
        );
        return;
      case 'stopAnimations':
      case 'startAnimations':
      case 'startChain':
      case 'completeAfter':
        animationStatement(this, statement, this.program.animations);
        return;
      case 'return':
        if (!this.onReturn) {
          throw new Error('AOT internal: a `return` outside an effect');
        }
        builder.branch('JMP', this.onReturn);
        return;
      case 'armCleanup':
        this.scoped(() =>
          this.set(
            required(this.program.armed.get(statement.flag), statement.flag),
            this.load('bool', 1),
          ),
        );
        return;
      case 'updateVector':
        updateVector(this, statement, this.program.vectors);
    }
  }

  /**
   * Appends an item, its fields worked out in order into consecutive registers. A full list drops it.
   * A text field is built in its own buffer first.
   *
   * @param statement  The append.
   */
  private appendItem(statement: IrAppendItem): void {
    const {builder} = this;
    const pool = this.listPool(statement.list);
    const row = builder.consecutive(
      statement.values.map(({field}) =>
        field.kind === 'string' ? 'string' : field.kind,
      ),
    );
    statement.values.forEach((itemValue, index) =>
      this.scoped(() => {
        if ('text' in itemValue) {
          const buffer = required(
            pool.buffers.get(itemValue.field.key),
            `field "${itemValue.field.key}"`,
          );
          this.text(itemValue.text.parts);
          builder.abx('FMTSET', 0, buffer.index);
          builder.abx('GETS', row[index].index, buffer.index);
        } else {
          builder.abc(
            'MOVE',
            row[index].index,
            this.valueAs(itemValue.value, itemValue.field.kind).index,
          );
        }
      }),
    );
    builder.abx('APPEND', row[0].index, pool.slot.index);
  }

  /**
   * Starts a timer, keeping its id in a local when the app names it.
   *
   * @param timer  The timer.
   * @param idLocal  The local's name, or null.
   */
  private startTimer(timer: IrTimer, idLocal: string | null): void {
    const {builder} = this;
    const hoisted =
      idLocal === null ? undefined : this.program.hoisted.get(idLocal);
    const start = (id: Register): void =>
      this.scoped(() => {
        builder.abc(
          'TSTART',
          id.index,
          this.timerDelay(timer.delay).index,
          timer.isRepeating ? 1 : 0,
          required(
            this.program.timerFunctions.get(timer.functionName),
            `timer "${timer.functionName}"`,
          ),
        );
        if (hoisted) {
          this.set(hoisted, id);
        }
      });
    if (idLocal === null || hoisted) {
      this.scoped(() => start(builder.register('int')));
      if (hoisted) {
        this.locals.set(idLocal!, {slot: hoisted});
      }
      return;
    }
    // The id outlives this statement, so its register is taken outside the scope.
    const id = builder.register('int');
    start(id);
    this.locals.set(idLocal, {register: id});
  }

  /**
   * A timer's delay in ms: a 64-bit or fractional delay through ToInt32, with a
   * negative one as 0. (The runtime waits at least 1 ms.)
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

    // A float delay becomes a 64-bit int; the VM's conversion saturates.
    const {builder} = this;
    const whole =
      kind === 'float'
        ? this.convert(this.valueAs(delay, 'float'), 'int64')
        : this.valueAs(delay, 'int64');
    const modulus = this.load('int64', TWO_TO_32);
    const low = builder.register('int64');
    builder.abc('MODL', low.index, whole.index, modulus.index);
    builder.abc('ADDL', low.index, low.index, modulus.index);
    builder.abc('MODL', low.index, low.index, modulus.index);
    const tooBig = builder.register('bool');
    builder.abc(
      'LLT',
      tooBig.index,
      this.load('int64', INT32_MAX).index,
      low.index,
    );
    return this.choose(
      tooBig,
      () => this.load('int', 0),
      () => this.convert(low, 'int'),
      'int',
    );
  }

  private writeRef(ref: RefRecord, operator: string, value: IrExpr): void {
    const slot = this.refSlot(ref);
    if (operator === '=') {
      this.set(slot, this.value(value));
      return;
    }
    // `ref op= value` is `ref = ref op value`, worked out in the wider of the two types.
    const type = usual(slotType(slot), this.typeOf(value));
    const register = this.builder.register(type as RegisterType);
    this.builder.abc(
      ARITHMETIC_OPCODES[type][operator.slice(0, -1)],
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
    const checkpoint = this.builder.checkpoint();
    lower();
    this.builder.release(checkpoint);
  }

  /*--------------------------------------------------------------------------------------------------------------------
   - Props
   -------------------------------------------------------------------------------------------------------------------*/

  /**
   * Sets a state-driven prop in the props scratch, converting its value to the ERProps field's type.
   *
   * @param field  The ERProps field.
   * @param value  Its value.
   */
  setProp(field: string, value: IrPropValue): void {
    this.scoped(() => {
      const prop = abiProp(field);
      const result = this.propValue(value);
      const {builder} = this;
      if (result.conversion === 'opacity' || prop.kind === 'float') {
        // The runtime's PSETF scales an opacity to the engine's byte.
        builder.abx(
          'PSETF',
          this.convert(result.register, 'float').index,
          prop.id,
        );
        return;
      }
      if (result.conversion === 'dimension') {
        this.setDimension(prop.id, result.register);
        return;
      }
      // Any other field is a whole number: a fraction goes in truncated.
      builder.abx('PSETI', this.convert(result.register, 'int').index, prop.id);
    });
  }

  /**
   * A state-driven prop value, such as a paint's color, in the type it is worked out in.
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
   * A dimension, rounded: halves up, clamped to int16, NaN as 0.
   *
   * @param prop  The prop's id.
   * @param value  The value.
   */
  private setDimension(prop: number, value: Register): void {
    const {builder} = this;
    if (value.type === 'int' || value.type === 'bool') {
      builder.abx('PSETI', this.convert(value, 'int').index, prop);
      return;
    }
    if (value.type === 'float') {
      // The runtime rounds a float.
      builder.abx('PSETF', value.index, prop);
      return;
    }
    // A double (or a 64-bit int, which becomes a double) is rounded here, since a float can't hold it exactly.
    const rounded = builder.register('double');
    builder.abc(
      'DADD',
      rounded.index,
      this.convert(value, 'double').index,
      this.load('double', ROUND_HALF_UP).index,
    );
    builder.abc(
      'DMATH',
      rounded.index,
      rounded.index,
      rounded.index,
      MathFn.floor,
    );
    builder.abx('PSETI', this.convert(rounded, 'int').index, prop);
  }

  /**
   * A state-driven prop value, with the conversion on top of it lifted out. A conversion that maps 0 to
   * 0 (an opacity, a dimension) gives the same answer applied after a choice as inside it.
   *
   * @param value  The value.
   *
   * @returns Its register, type and conversion.
   */
  private propValue(value: IrPropValue): PropResult {
    const plain = (register: Register): PropResult => ({
      register,
      type: register.type as ExprType,
      conversion: null,
    });
    switch (value.kind) {
      case 'constant':
        return plain(this.constantValue(value.value));
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
        // This tests the value itself, and a string slot always reads as set.
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
        const register = this.builder.register('int');
        const end = this.builder.label();
        this.builder.abx('LOADI', register.index, 0);
        this.builder.branch(value.negate ? 'JT' : 'JF', end, test);
        this.builder.abx('LOADI', register.index, value.whenTrue);
        this.builder.place(end);
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
   * A constant prop value in a register.
   *
   * @param constant  The constant.
   *
   * @returns The register.
   */
  private constantValue(constant: PropConstant): Register {
    const value = constantNumber(constant);
    if (value === null) {
      return notYet(`the constant \`${constant}\``);
    }
    return this.load('int', value);
  }

  /** ER_DISPLAY_FLEX while a test holds, else ER_DISPLAY_NONE. */
  private display(test: Register): Register {
    const register = this.builder.register('int');
    const end = this.builder.label();
    this.builder.abx('LOADI', register.index, DISPLAY_NONE);
    this.builder.branch('JF', end, test);
    this.builder.abx('LOADI', register.index, DISPLAY_FLEX);
    this.builder.place(end);
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
      const count = this.builder.register('int');
      const register = this.builder.register('bool');
      this.builder.abx('LEN', count.index, this.listPool(show.list).slot.index);
      this.builder.abc(
        'LT',
        register.index,
        this.load('int', show.row).index,
        count.index,
      );
      return register;
    }
    const register = this.builder.register('bool');
    this.builder.abc('MOVE', register.index, this.condition(show.test).index);
    if (show.negate) {
      this.builder.abc('NOT', register.index, register.index);
    }
    if (show.within) {
      const end = this.builder.label();
      this.builder.branch('JF', end, register);
      this.builder.abc('MOVE', register.index, this.shown(show.within).index);
      this.builder.place(end);
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
      : required(this.program.refs.get(ref), `ref "${ref.slotName}"`);
  }

  bootSlot(slot: IrBootSlot): Slot {
    return required(this.program.boot.get(slot), `boot value "${slot.field}"`);
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses what a package can't lower yet.
 *
 * @param what  What the app uses.
 */
export function notYet(what: string): never {
  throw aotError(`AOT: a package doesn't support ${what} yet`);
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
 * The type a slot holds.
 *
 * @param slot  The slot.
 *
 * @returns Its type.
 */
export function slotType(slot: Slot): ExprType {
  return (
    {
      [ValueType.int]: 'int',
      [ValueType.bool]: 'bool',
      [ValueType.float]: 'float',
      [ValueType.int64]: 'int64',
      [ValueType.double]: 'double',
      [ValueType.string]: 'string',
      [ValueType.enum]: 'enum',
    } as Record<number, ExprType>
  )[slot.type];
}

/**
 * The type an operation on two values is worked out in: the wider of the two, a double over a float over a 64-bit
 * int over an int. Two booleans stay a boolean (an int holding 0 or 1).
 *
 * @param left  One operand's type.
 * @param right  The other's.
 *
 * @returns The type.
 */
function usual(left: ExprType, right: ExprType): ExprType {
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

/** A boolean worked on as a number is an int. */
const promoted = (type: ExprType): ExprType => (type === 'bool' ? 'int' : type);

/**
 * The type a handler local holds: what its IR type says.
 *
 * @param initializer  The local's initial value.
 *
 * @returns Its type.
 */
function declaredType(initializer: IrExpr): ExprType {
  switch (initializer.type.kind) {
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
