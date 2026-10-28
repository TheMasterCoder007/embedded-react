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
 * The arena's layout: every slot the program reads or writes, the dirty bits, and the regions that hold them.
 * Slots are planned first and laid out once, before any code is written, since code names them by index and
 * the slot table lists them in arena order.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {FRAME_CELLS, Region, SlotFlag, ValueType} from './format.mts';
import type {ProgramRegion, ProgramSlot} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A planned slot. `index` is set when the arena is laid out. */
export interface Slot {
  type: number;
  flags: number;
  dirtyBit: number;
  /** An immediate, or a constant or string index, by type. */
  init: number;
  /** A string's most bytes, without its NUL. */
  cap: number;
  /** An enum's set. */
  aux: number;
  index: number;
  offset: number;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The init of a 64-bit or string slot that starts at zero. */
const NO_INIT = 0xffffffff;

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The arena as it is planned.
 *
 * @class Arena
 */
export class Arena {
  private readonly values: Slot[] = [];
  private readonly strings: Slot[] = [];
  private readonly handles: Slot[] = [];
  private dirtyBits = 0;
  private laidOut: ProgramRegion[] | null = null;

  /**
   * Plans a slot.
   *
   * @param type  Its ValueType.
   * @param init  Its initial value: an immediate, or a constant or string index; none starts at zero.
   * @param cap  A string's most bytes.
   *
   * @returns The slot.
   */
  slot(type: number, init?: number, cap = 0): Slot {
    if (this.laidOut) {
      throw new Error(
        'AOT internal: a slot planned after the arena was laid out',
      );
    }
    const wide = type === ValueType.int64 || type === ValueType.double;
    const slot: Slot = {
      type,
      flags: 0,
      dirtyBit: 0,
      init: init ?? (wide || type === ValueType.string ? NO_INIT : 0),
      cap,
      aux: 0,
      index: -1,
      offset: -1,
    };
    (type === ValueType.string
      ? this.strings
      : type === ValueType.node
        ? this.handles
        : this.values
    ).push(slot);
    return slot;
  }

  /**
   * Gives a slot a dirty bit, set when its value changes.
   *
   * @param slot  The slot.
   *
   * @returns The slot.
   */
  track(slot: Slot): Slot {
    if (this.laidOut) {
      throw new Error(
        'AOT internal: a dirty bit given after the arena was laid out',
      );
    }
    slot.flags |= SlotFlag.tracked;
    slot.dirtyBit = this.dirtyBits++;
    return slot;
  }

  /**
   * Lays the slots out: values, the dirty bits, strings, then node handles. Code may name slots from here on.
   */
  layOut(): void {
    const regions: ProgramRegion[] = [];
    let offset = 0;
    const region = (
      kind: number,
      slots: Slot[],
      bytesOf: (slot: Slot) => number,
    ): void => {
      const start = offset;
      for (const slot of slots) {
        slot.offset = offset;
        offset += bytesOf(slot);
      }
      if (offset > start) {
        regions.push({kind, count: 0, offset: start, bytes: offset - start});
      }
    };

    region(Region.slots, this.values, slot =>
      slot.type === ValueType.int64 || slot.type === ValueType.double ? 8 : 4,
    );
    if (this.dirtyBits) {
      // Kept twice: live, and the update pass's copy.
      const bytes = 2 * Math.ceil(this.dirtyBits / 32) * 4;
      regions.push({kind: Region.dirty, count: this.dirtyBits, offset, bytes});
      offset += bytes;
    }
    region(Region.strings, this.strings, slot => (slot.cap + 1 + 3) & ~3);
    region(Region.handles, this.handles, () => 8);

    [...this.values, ...this.strings, ...this.handles].forEach(
      (slot, index) => {
        slot.index = index;
      },
    );
    this.laidOut = regions;
  }

  /** The slot table, in arena order. */
  get slots(): ProgramSlot[] {
    return [...this.values, ...this.strings, ...this.handles].map(slot => ({
      type: slot.type,
      flags: slot.flags,
      dirtyBit: slot.dirtyBit,
      offset: slot.offset,
      init: slot.init,
      cap: slot.cap,
      aux: slot.aux,
    }));
  }

  /** Whether any slot has a dirty bit. */
  get hasDirtyBits(): boolean {
    return this.dirtyBits > 0;
  }

  /**
   * The arena's regions, with the register stack after the slots.
   *
   * @param stackCells  Cells the deepest function needs, frame included.
   *
   * @returns The regions and the arena's size.
   */
  regions(stackCells: number): {regions: ProgramRegion[]; bytes: number} {
    if (!this.laidOut) {
      throw new Error('AOT internal: the arena was not laid out');
    }
    const end = this.laidOut.reduce(
      (last, region) => Math.max(last, region.offset + region.bytes),
      0,
    );
    const cells = Math.max(stackCells, FRAME_CELLS);
    return {
      regions: [
        ...this.laidOut,
        {kind: Region.stack, count: cells, offset: end, bytes: cells * 4},
      ],
      bytes: end + cells * 4,
    };
  }
}
