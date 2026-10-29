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
 * Builds one function's bytecode: typed registers, instructions in the forms erb_format.h gives each opcode,
 * and forward branches to labels placed later.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from '../diagnostics.mts';
import {OPCODES, ValueType} from './format.mts';
import type {ProgramFunction} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** The register types the compiler uses. */
export type RegisterType =
  | 'int'
  | 'bool'
  | 'float'
  | 'int64'
  | 'double'
  | 'string'
  | 'enum'
  | 'node'
  | 'anim';

/** A register: its first cell, and the type it holds for the whole function. */
export interface Register {
  index: number;
  type: RegisterType;
}

/** A place in the code that branches go to, placed after them. */
export interface Label {
  at: number | null;
  /** The branches to it, by the word index of their instruction. */
  branches: number[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** Each opcode by name, with its number and length. */
export const OP: Record<string, {code: number; words: number}> =
  Object.fromEntries(
    OPCODES.map(([name, , words], index) => [name, {code: index + 1, words}]),
  );

/** Register cells a function may have: the function record counts them in a byte. */
const MAX_REGISTERS = 255;

/** The register types that take two cells. */
const WIDE: ReadonlySet<RegisterType> = new Set(['int64', 'double', 'node']);

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * One function as it is built. Temporaries go back to a pool by type when released, so a long function
 * doesn't run out of registers.
 *
 * @class FunctionBuilder
 */
export class FunctionBuilder {
  private readonly code: number[] = [];
  /** Each cell's type, as the types table holds it. */
  private readonly cellTypes: number[] = [];
  private readonly pool = new Map<RegisterType, number[]>();
  /** Released runs of consecutive registers, by their types, at their first cell. */
  private readonly runs = new Map<string, number[]>();
  /** The types of each run, by its first cell. */
  private readonly runTypes = new Map<number, string>();
  /** Registers in use, in the order they were taken, for checkpoints. */
  private readonly taken: Register[] = [];

  readonly kind: number;
  private readonly what: string;

  /**
   * @param kind  The function's kind (a FunctionKind or an EREventType).
   * @param what  What the function is, for an error.
   */
  constructor(kind: number, what: string) {
    this.kind = kind;
    this.what = what;
  }

  /** The next instruction's word index. */
  get position(): number {
    return this.code.length;
  }

  /**
   * A register of a type: a released one when there is one, else a new one.
   *
   * @param type  Its type.
   *
   * @returns The register.
   */
  register(type: RegisterType): Register {
    const reused = this.pool.get(type)?.pop();
    const register = {index: reused ?? this.newRegister(type), type};
    this.taken.push(register);
    return register;
  }

  /**
   * A register of a type that is never released, for the rest of the function. Registers start at zero, so
   * one nothing writes keeps that value.
   *
   * @param type  Its type.
   *
   * @returns The register.
   */
  reserved(type: RegisterType): Register {
    return {index: this.newRegister(type), type};
  }

  /**
   * Registers of one-cell types in consecutive cells, as APPEND reads a row: a released run of the same types when
   * there is one, else new ones. A run is released whole, and only ever reused as a run.
   *
   * @param types  Their types.
   *
   * @returns The registers.
   */
  consecutive(types: RegisterType[]): Register[] {
    const key = types.join(' ');
    let first = this.runs.get(key)?.pop();
    if (first === undefined) {
      first = this.cellTypes.length;
      types.forEach(type => this.newRegister(type));
      this.runTypes.set(first, key);
    }
    const registers = types.map((type, index) => ({
      index: first + index,
      type,
    }));
    this.taken.push(registers[0]);
    return registers;
  }

  private newRegister(type: RegisterType): number {
    // A two-cell register starts on an even cell.
    if (WIDE.has(type) && this.cellTypes.length % 2) {
      this.cellTypes.push(ValueType.int);
    }
    const index = this.cellTypes.length;
    this.cellTypes.push(ValueType[type]);
    if (WIDE.has(type)) {
      this.cellTypes.push(ValueType.pair);
    }
    if (this.cellTypes.length > MAX_REGISTERS) {
      throw aotError(
        `AOT: ${this.what} needs more than ${MAX_REGISTERS} registers in the package output`,
        'split it into smaller pieces.',
      );
    }
    return index;
  }

  /** Where the registers taken so far end, for release. */
  checkpoint(): number {
    return this.taken.length;
  }

  /**
   * Releases every register taken since a checkpoint, except those kept.
   *
   * @param checkpoint  The checkpoint.
   * @param keep  Registers still in use.
   */
  release(checkpoint: number, ...keep: Register[]): void {
    const kept = this.taken.splice(checkpoint).filter(register => {
      if (keep.includes(register)) {
        return true;
      }
      const run = this.runTypes.get(register.index);
      if (run !== undefined) {
        this.runs.set(run, [...(this.runs.get(run) ?? []), register.index]);
        return false;
      }
      const free = this.pool.get(register.type) ?? [];
      free.push(register.index);
      this.pool.set(register.type, free);
      return false;
    });
    this.taken.push(...kept);
  }

  /**
   * An instruction with A, B and C.
   *
   * @param name  The opcode.
   * @param a  A.
   * @param b  B.
   * @param c  C.
   * @param extension  The extension word, for an opcode that has one.
   */
  abc(name: string, a = 0, b = 0, c = 0, extension?: number): void {
    this.emit(
      name,
      (a & 0xff) | ((b & 0xff) << 8) | ((c & 0xff) << 16),
      extension,
    );
  }

  /**
   * An instruction with A and a 16-bit Bx.
   *
   * @param name  The opcode.
   * @param a  A.
   * @param bx  Bx, an index or a signed immediate.
   * @param extension  The extension word, for an opcode that has one.
   */
  abx(name: string, a: number, bx: number, extension?: number): void {
    this.emit(name, (a & 0xff) | ((bx & 0xffff) << 8), extension);
  }

  private emit(name: string, operands: number, extension?: number): void {
    const op = OP[name];
    this.code.push((op.code | (operands << 8)) >>> 0);
    if (op.words === 2) {
      this.code.push((extension ?? 0) >>> 0);
    }
  }

  /** A label to branch to, placed later. */
  label(): Label {
    return {at: null, branches: []};
  }

  /**
   * Places a label here, and points the branches to it here.
   *
   * @param label  The label.
   */
  place(label: Label): void {
    label.at = this.position;
    for (const at of label.branches) {
      const offset = label.at - (at + wordsOf(this.code[at]));
      if (offset > 0x7fff) {
        throw aotError(
          `AOT: ${this.what} has a branch too long for the package output`,
          'split it into smaller pieces.',
        );
      }
      this.code[at] = ((this.code[at] & 0xffff) | (offset << 16)) >>> 0;
    }
  }

  /**
   * A forward branch to a label: JMP, or JT / JF on a bool register.
   *
   * @param name  JMP, JT or JF.
   * @param label  Where it goes.
   * @param test  The bool register JT and JF test.
   */
  branch(name: 'JMP' | 'JT' | 'JF', label: Label, test?: Register): void {
    label.branches.push(this.position);
    this.abx(name, test?.index ?? 0, 0);
  }

  /**
   * A branch to a label when any of a dirty word's bits is set in this update pass.
   *
   * @param word  The dirty word.
   * @param mask  Its bits.
   * @param label  Where it goes.
   */
  branchIfDirty(word: number, mask: number, label: Label): void {
    label.branches.push(this.position);
    this.abx('JDIRTY', word, 0, mask);
  }

  /**
   * The finished function, ending in RET.
   *
   * @param params  Parameter cells.
   * @param result  What it returns, for a function that returns a value.
   *
   * @returns The function.
   */
  finish(params = 0, result?: Register): ProgramFunction {
    this.abc('RET', result?.index ?? 0);
    return {
      kind: this.kind,
      params,
      result: result ? ValueType[result.type] : 0,
      types: [...this.cellTypes],
      code: [...this.code],
    };
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * An instruction's length, from its first word.
 *
 * @param word  The word.
 *
 * @returns Its length in words.
 */
function wordsOf(word: number): number {
  return OPCODES[(word & 0xff) - 1][2];
}
