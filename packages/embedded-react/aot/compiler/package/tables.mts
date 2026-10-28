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
 * The tables a program's pieces share: strings, constants and prop sets, each entry stored once.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {NONE} from './format.mts';
import type {ProgramPropEntry} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The shared tables, filled as the program is lowered.
 *
 * @class ProgramTables
 */
export class ProgramTables {
  readonly strings: string[] = [];
  readonly const32: number[] = [];
  readonly const64: bigint[] = [];
  readonly propSets: ProgramPropEntry[][] = [];
  private readonly stringIndex = new Map<string, number>();
  private readonly const32Index = new Map<number, number>();
  private readonly const64Index = new Map<bigint, number>();
  private readonly propSetIndex = new Map<string, number>();

  /**
   * A string's index, adding it the first time.
   *
   * @param text  The string.
   *
   * @returns Its index.
   */
  string(text: string): number {
    return intern(this.stringIndex, this.strings, text);
  }

  /**
   * A 32-bit constant's index, adding it the first time.
   *
   * @param bits  Its bits, as an unsigned number.
   *
   * @returns Its index.
   */
  constant32(bits: number): number {
    return intern(this.const32Index, this.const32, bits >>> 0);
  }

  /**
   * A 64-bit constant's index, adding it the first time.
   *
   * @param bits  Its bits.
   *
   * @returns Its index.
   */
  constant64(bits: bigint): number {
    return intern(this.const64Index, this.const64, BigInt.asUintN(64, bits));
  }

  /**
   * A prop set's index, adding it the first time.
   *
   * @param entries  The set's entries, applied in order.
   *
   * @returns Its index, or NONE for an empty set.
   */
  propSet(entries: ProgramPropEntry[]): number {
    return entries.length
      ? intern(
          this.propSetIndex,
          this.propSets,
          entries,
          JSON.stringify(entries),
        )
      : NONE;
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A value's index in a table, adding it the first time.
 *
 * @param index  The table's index, by key.
 * @param table  The table.
 * @param value  The value.
 * @param key  Its key, when it isn't the value itself.
 *
 * @returns Its index.
 */
function intern<Key, Value>(
  index: Map<Key, number>,
  table: Value[],
  value: Value,
  key: Key = value as unknown as Key,
): number {
  let at = index.get(key);
  if (at === undefined) {
    at = table.push(value) - 1;
    index.set(key, at);
  }

  return at;
}
