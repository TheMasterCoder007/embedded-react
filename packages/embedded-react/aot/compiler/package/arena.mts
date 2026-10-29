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

import {
  ANIM_ENTRY_SIZE,
  EVENT_SIZE,
  FRAME_CELLS,
  LIST_HEADER_SIZE,
  Region,
  SlotFlag,
  TIMER_SIZE,
  ValueType,
} from './format.mts';
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
  /** A string's most bytes, without its NUL; a list's rows; a handle array's handles. */
  cap: number;
  /** An enum's set, or a list's row layout. */
  aux: number;
  /** A list's row size. */
  rowBytes: number;
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
  private readonly lists: Slot[] = [];
  private readonly handles: Slot[] = [];
  private dirtyBits = 0;
  private timers = 0;
  private anims = 0;
  private events = 0;
  private tapeFloats = 0;
  private laidOut: ProgramRegion[] | null = null;

  /**
   * Plans a slot.
   *
   * @param type  Its ValueType.
   * @param init  Its initial value: an immediate, or a constant or string index; none starts at zero.
   * @param cap  A string's most bytes, or a handle array's handles.
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
      rowBytes: 0,
      index: -1,
      offset: -1,
    };
    (type === ValueType.string
      ? this.strings
      : type === ValueType.list
        ? this.lists
        : type === ValueType.node
          ? this.handles
          : this.values
    ).push(slot);
    return slot;
  }

  /**
   * Plans a list's pool: its row count, then `cap` rows, empty at load.
   *
   * @param layout  Its row layout.
   * @param rowBytes  A row's size.
   * @param cap  Rows it holds.
   *
   * @returns The slot.
   */
  list(layout: number, rowBytes: number, cap: number): Slot {
    return Object.assign(this.slot(ValueType.list, 0, cap), {
      aux: layout,
      rowBytes,
    });
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
   * Gives the arena a timer table.
   *
   * @param count  Timers that run at once.
   */
  timerTable(count: number): void {
    this.timers = count;
  }

  /**
   * Gives the arena a running-animation table, which the trees ASTART starts are laid out in.
   *
   * @param count  Entries it holds.
   */
  animTable(count: number): void {
    this.anims = count;
  }

  /**
   * Gives the arena's event queue room for more events: the effects an update pass queues, or the animation
   * completions that arrive while a function runs.
   *
   * @param count  Events it holds besides.
   */
  eventQueue(count: number): void {
    this.events += count;
  }

  /**
   * Gives the arena a tape scratch, as large as the largest tape code builds. An empty tape still needs one.
   *
   * @param floats  The tape's floats.
   */
  tapeScratch(floats: number): void {
    this.tapeFloats = Math.max(this.tapeFloats, floats, 1);
  }

  /**
   * Lays the slots out: values, the dirty bits, strings, list pools, node handles, then the timer and animation
   * tables. Code may name slots from here on.
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
    region(
      Region.lists,
      this.lists,
      slot => LIST_HEADER_SIZE + slot.cap * slot.rowBytes,
    );
    region(Region.handles, this.handles, slot => 8 * Math.max(slot.cap, 1));
    if (this.timers) {
      const bytes = this.timers * TIMER_SIZE;
      regions.push({kind: Region.timers, count: this.timers, offset, bytes});
      offset += bytes;
    }
    if (this.anims) {
      const bytes = this.anims * ANIM_ENTRY_SIZE;
      regions.push({kind: Region.anims, count: this.anims, offset, bytes});
      offset += bytes;
    }

    [...this.values, ...this.strings, ...this.lists, ...this.handles].forEach(
      (slot, index) => {
        slot.index = index;
      },
    );
    this.laidOut = regions;
  }

  /** The slot table, in arena order. */
  get slots(): ProgramSlot[] {
    return [
      ...this.values,
      ...this.strings,
      ...this.lists,
      ...this.handles,
    ].map(slot => ({
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
   * The arena's regions, with the register stack after the slots, then the tape scratch, and the event queue.
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
    const regions = [
      ...this.laidOut,
      {kind: Region.stack, count: cells, offset: end, bytes: cells * 4},
    ];
    let bytes = end + cells * 4;
    if (this.tapeFloats) {
      regions.push({
        kind: Region.tape,
        count: this.tapeFloats,
        offset: bytes,
        bytes: this.tapeFloats * 4,
      });
      bytes += this.tapeFloats * 4;
    }
    if (this.events) {
      regions.push({
        kind: Region.events,
        count: this.events,
        offset: bytes,
        bytes: this.events * EVENT_SIZE,
      });
      bytes += this.events * EVENT_SIZE;
    }
    return {regions, bytes};
  }
}
