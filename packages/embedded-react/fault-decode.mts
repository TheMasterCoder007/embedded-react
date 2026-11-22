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

// Decodes a fault record (engine/include/er_error_screen.h, ERFaultRecord) on the host: reads the record as
// er_fault_text() writes it, finds each address in the firmware's ELF, and rebuilds the C call chain from the
// stack words the record holds. `embedded-react decode-fault` prints the result.

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** Where the app was when the fault came. */
export interface FaultApp {
  function: number;
  panel: number;
  offset: number;
  buildId: number;
  /** The source file the board's debug table named, or '' when the package has none. */
  file: string;
  line: number;
}

/** A fault record, as the board wrote it. */
export interface FaultRecord {
  core: number;
  reason: string;
  programCounter: number;
  linkRegister: number;
  stackPointer: number;
  address: number;
  status: number[];
  /** Null when the app's code wasn't running. */
  app: FaultApp | null;
  /** The stack from stackPointer up, or the callers' addresses when stackIsCallers. */
  stack: number[];
  stackIsCallers: boolean;
}

/** A function in the firmware. */
export interface ElfFunction {
  name: string;
  address: number;
  /** 0 when the symbol doesn't say. */
  size: number;
  /** Which ELF it came from, by its place in the list mergeElfs() took; 0 for an ELF read alone. */
  image: number;
}

/** What the decoder needs of the firmware's ELF. */
export interface Elf {
  /** The ELF machine: ELF_MACHINE_ARM and so on. */
  machine: number;
  /** By address. */
  functions: ElfFunction[];
  /** The sections that hold code. */
  code: {address: number; bytes: Uint8Array}[];
}

/**
 * How sure a frame of the call chain is:
 * - `fault`: the instruction that faulted.
 * - `sure`: a return address after a call to the frame below it.
 * - `likely`: a return address after a call through a pointer, which can't be checked against the frame below.
 * - `unsure`: a return address after a call to some other function: the frame below was reached by a tail call, or
 *   the word was left on the stack by an earlier call.
 * - `unwinder`: found by the board's own unwinder.
 * - `interrupt`: not a frame; an interrupt was taken here, and the frames above are what it interrupted.
 */
export type FrameKind =
  'fault' | 'sure' | 'likely' | 'unsure' | 'unwinder' | 'interrupt';

/** One frame of the call chain, innermost first. */
export interface FaultFrame {
  kind: FrameKind;
  /** The faulting instruction, or the call instruction this frame was in. */
  address: number;
  /** The function it is in, or null when no ELF has one there. */
  function: string | null;
  /** Which ELF the function is in: see ElfFunction.image. */
  image: number;
  /** How far into the function. */
  offset: number;
  /** Why it is as sure as it is, for a person. */
  note: string;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

export const ELF_MACHINE_ARM = 40;
export const ELF_MACHINE_XTENSA = 94;
export const ELF_MACHINE_RISCV = 243;

export const FAULT_CORE_CORTEX_M = 1;
export const FAULT_CORE_XTENSA = 2;
export const FAULT_CORE_RISCV = 3;

/** "ERFT", as the board's bytes lie. */
const RECORD_MAGIC_HEX = '54465245';
const RECORD_VERSION = 1;
const RECORD_FLAG_APP = 0x01;
const RECORD_FLAG_CALLERS = 0x02;
const RECORD_STATUS_WORDS = 4;
const RECORD_FILE_BYTES = 32;
const RECORD_REASON_BYTES = 48;

/* Offsets in the record. */
const RECORD_VERSION_AT = 4;
const RECORD_CORE_AT = 5;
const RECORD_FLAGS_AT = 6;
const RECORD_STACK_COUNT_AT = 7;
const RECORD_PROGRAM_COUNTER_AT = 8;
const RECORD_LINK_REGISTER_AT = 12;
const RECORD_STACK_POINTER_AT = 16;
const RECORD_ADDRESS_AT = 20;
const RECORD_STATUS_AT = 24;
const RECORD_APP_OFFSET_AT = 40;
const RECORD_APP_BUILD_ID_AT = 44;
const RECORD_APP_LINE_AT = 48;
const RECORD_APP_FUNCTION_AT = 52;
const RECORD_APP_PANEL_AT = 54;
const RECORD_APP_FILE_AT = 56;
const RECORD_REASON_AT = RECORD_APP_FILE_AT + RECORD_FILE_BYTES;
const RECORD_STACK_AT = RECORD_REASON_AT + RECORD_REASON_BYTES;
const WORD_BYTES = 4;

/* The record's check word: FNV-1a over every byte before it. */
const HASH_SEED = 2166136261;
const HASH_PRIME = 16777619;

/* ELF32, little-endian. */
const ELF_MAGIC = [0x7f, 0x45, 0x4c, 0x46];
const ELF_CLASS_32 = 1;
const ELF_DATA_LITTLE = 1;
const ELF_HEADER_BYTES = 52;
const ELF_MACHINE_AT = 18;
const ELF_SECTIONS_AT = 32;
const ELF_SECTION_BYTES_AT = 46;
const ELF_SECTION_COUNT_AT = 48;
const SECTION_TYPE_AT = 4;
const SECTION_FLAGS_AT = 8;
const SECTION_ADDRESS_AT = 12;
const SECTION_OFFSET_AT = 16;
const SECTION_SIZE_AT = 20;
const SECTION_LINK_AT = 24;
const SECTION_TYPE_PROGBITS = 1;
const SECTION_TYPE_SYMBOLS = 2;
const SECTION_FLAG_ALLOCATED = 0x2;
const SECTION_FLAG_CODE = 0x4;
const SYMBOL_BYTES = 16;
const SYMBOL_VALUE_AT = 4;
const SYMBOL_SIZE_AT = 8;
const SYMBOL_INFO_AT = 12;
const SYMBOL_SECTION_AT = 14;
const SYMBOL_TYPE_MASK = 0xf;
const SYMBOL_TYPE_NONE = 0;
const SYMBOL_TYPE_FUNCTION = 2;

/* Thumb: a return address has bit 0 set, and follows a BL (two halfwords) or a BLX to a register (one). */
const THUMB_BIT = 1;
const THUMB_BL_BYTES = 4;
const THUMB_BLX_BYTES = 2;

/** An EXC_RETURN value, which an interrupt handler holds in place of a return address. */
const isExceptionReturn = (word: number): boolean =>
  word >>> 8 === 0xffffff && (word & 0x1) === 1 && (word & 0xe0) === 0xe0;

/** The CFSR's bits, lowest first: the MemManage byte, the BusFault byte, the UsageFault half. */
const CFSR_BITS: [number, string, string][] = [
  [0, 'IACCVIOL', 'ran code at an address it may not'],
  [1, 'DACCVIOL', 'read or wrote an address it may not'],
  [3, 'MUNSTKERR', 'memory fault while unstacking an exception'],
  [4, 'MSTKERR', 'memory fault while stacking an exception: a stack overflow'],
  [5, 'MLSPERR', 'memory fault while saving float registers'],
  [7, 'MMARVALID', 'MMFAR holds the address'],
  [8, 'IBUSERR', 'bus error fetching an instruction'],
  [9, 'PRECISERR', 'bus error at the faulting instruction'],
  [10, 'IMPRECISERR', 'bus error some instructions earlier: pc is past it'],
  [11, 'UNSTKERR', 'bus error while unstacking an exception'],
  [12, 'STKERR', 'bus error while stacking an exception: a stack overflow'],
  [13, 'LSPERR', 'bus error while saving float registers'],
  [15, 'BFARVALID', 'BFAR holds the address'],
  [16, 'UNDEFINSTR', 'undefined instruction'],
  [17, 'INVSTATE', 'a jump to an address without the Thumb bit'],
  [18, 'INVPC', 'bad exception return'],
  [19, 'NOCP', 'used the FPU while it is off'],
  [20, 'STKOF', 'stack overflow'],
  [24, 'UNALIGNED', 'unaligned access'],
  [25, 'DIVBYZERO', 'divide by zero'],
];

const CFSR_MMARVALID = 1 << 7;
const CFSR_BFARVALID = 1 << 15;

const HFSR_BITS: [number, string, string][] = [
  [1, 'VECTTBL', 'bus error reading the vector table'],
  [30, 'FORCED', 'another fault, raised to a hard fault'],
];

const XTENSA_CAUSES: Record<number, string> = {
  0: 'IllegalInstruction',
  2: 'InstructionFetchError',
  3: 'LoadStoreError',
  6: 'IntegerDivideByZero',
  9: 'LoadStoreAlignment',
  20: 'InstFetchProhibited',
  28: 'LoadProhibited',
  29: 'StoreProhibited',
};

/*----------------------------------------------------------------------------------------------------------------------
 - The record
 ---------------------------------------------------------------------------------------------------------------------*/

const hex = (value: number): string =>
  `0x${(value >>> 0).toString(16).padStart(8, '0')}`;

/** The text of a NUL-padded field. */
function fieldText(bytes: Uint8Array, at: number, length: number): string {
  const field = bytes.subarray(at, at + length);
  const end = field.indexOf(0);
  return new TextDecoder().decode(end < 0 ? field : field.subarray(0, end));
}

/**
 * Reads a fault record.
 *
 * @param input  The line er_fault_text() wrote, alone or inside other text such as a log, or the record's bytes.
 *
 * @returns The record.
 * @throws When there is no record, or it was cut short or changed.
 */
export function parseFaultRecord(input: string | Uint8Array): FaultRecord {
  let text: string;
  if (typeof input === 'string') {
    text = input;
  } else {
    const magicBytes = Buffer.from(RECORD_MAGIC_HEX, 'hex');
    text = magicBytes.every((byte, index) => input[index] === byte)
      ? Buffer.from(input).toString('hex')
      : new TextDecoder().decode(input);
  }
  const found = new RegExp(`${RECORD_MAGIC_HEX}(?:[0-9a-f]{2})+`, 'i').exec(
    text,
  );
  if (!found) {
    throw new Error(
      'no fault record here: pass the hex after "fault record:" in the board\'s log',
    );
  }
  const bytes = new Uint8Array(Buffer.from(found[0], 'hex'));
  const view = new DataView(bytes.buffer);
  const stackCapacity =
    (bytes.length - RECORD_STACK_AT - WORD_BYTES) / WORD_BYTES;
  if (!Number.isInteger(stackCapacity) || stackCapacity < 0) {
    throw new Error('the fault record is cut short: copy the whole line');
  }
  const checkAt = bytes.length - WORD_BYTES;
  let hash = HASH_SEED;
  for (let index = 0; index < checkAt; index++) {
    hash = Math.imul(hash ^ bytes[index], HASH_PRIME) >>> 0;
  }
  if (hash !== view.getUint32(checkAt, true)) {
    throw new Error(
      'the fault record is cut short or changed (its check word does not match): copy the whole line',
    );
  }
  if (bytes[RECORD_VERSION_AT] !== RECORD_VERSION) {
    throw new Error(
      `a fault record of version ${bytes[RECORD_VERSION_AT]}, but this version reads version ${RECORD_VERSION}: ` +
        "use the embedded-react version the firmware's engine came from",
    );
  }
  const flags = bytes[RECORD_FLAGS_AT];
  const stackCount = Math.min(bytes[RECORD_STACK_COUNT_AT], stackCapacity);
  const word = (at: number): number => view.getUint32(at, true);
  return {
    core: bytes[RECORD_CORE_AT],
    reason: fieldText(bytes, RECORD_REASON_AT, RECORD_REASON_BYTES),
    programCounter: word(RECORD_PROGRAM_COUNTER_AT),
    linkRegister: word(RECORD_LINK_REGISTER_AT),
    stackPointer: word(RECORD_STACK_POINTER_AT),
    address: word(RECORD_ADDRESS_AT),
    status: Array.from({length: RECORD_STATUS_WORDS}, (_, index) =>
      word(RECORD_STATUS_AT + index * WORD_BYTES),
    ),
    app:
      flags & RECORD_FLAG_APP
        ? {
            function: view.getUint16(RECORD_APP_FUNCTION_AT, true),
            panel: view.getUint16(RECORD_APP_PANEL_AT, true),
            offset: word(RECORD_APP_OFFSET_AT),
            buildId: word(RECORD_APP_BUILD_ID_AT),
            file: fieldText(bytes, RECORD_APP_FILE_AT, RECORD_FILE_BYTES),
            line: word(RECORD_APP_LINE_AT),
          }
        : null,
    stack: Array.from({length: stackCount}, (_, index) =>
      word(RECORD_STACK_AT + index * WORD_BYTES),
    ),
    stackIsCallers: (flags & RECORD_FLAG_CALLERS) !== 0,
  };
}

/*----------------------------------------------------------------------------------------------------------------------
 - The ELF
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Reads the functions and the code of a firmware ELF.
 *
 * @param bytes  The file.
 *
 * @returns Its machine, its functions by address and its code sections.
 * @throws When it isn't a 32-bit little-endian ELF, or it has no symbols.
 */
export function readElf(bytes: Uint8Array): Elf {
  if (
    bytes.length < ELF_HEADER_BYTES ||
    !ELF_MAGIC.every((byte, index) => bytes[index] === byte) ||
    bytes[4] !== ELF_CLASS_32 ||
    bytes[5] !== ELF_DATA_LITTLE
  ) {
    throw new Error(
      "not a 32-bit little-endian ELF: pass the firmware's .elf, not its .bin, .hex or .uf2",
    );
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const sectionsAt = view.getUint32(ELF_SECTIONS_AT, true);
  const sectionBytes = view.getUint16(ELF_SECTION_BYTES_AT, true);
  const sectionCount = view.getUint16(ELF_SECTION_COUNT_AT, true);
  const sections = Array.from({length: sectionCount}, (_, index) => {
    const at = sectionsAt + index * sectionBytes;
    return {
      type: view.getUint32(at + SECTION_TYPE_AT, true),
      flags: view.getUint32(at + SECTION_FLAGS_AT, true),
      address: view.getUint32(at + SECTION_ADDRESS_AT, true),
      offset: view.getUint32(at + SECTION_OFFSET_AT, true),
      size: view.getUint32(at + SECTION_SIZE_AT, true),
      link: view.getUint32(at + SECTION_LINK_AT, true),
    };
  });
  const isCode = (section: (typeof sections)[number] | undefined): boolean =>
    section !== undefined &&
    (section.flags & SECTION_FLAG_CODE) !== 0 &&
    (section.flags & SECTION_FLAG_ALLOCATED) !== 0;

  const machine = view.getUint16(ELF_MACHINE_AT, true);
  const functions: ElfFunction[] = [];
  for (const table of sections) {
    if (table.type !== SECTION_TYPE_SYMBOLS) {
      continue;
    }
    const names = sections[table.link];
    for (
      let at = table.offset;
      at < table.offset + table.size;
      at += SYMBOL_BYTES
    ) {
      const type = bytes[at + SYMBOL_INFO_AT] & SYMBOL_TYPE_MASK;
      const section = sections[view.getUint16(at + SYMBOL_SECTION_AT, true)];
      // Assembly routines often carry no type; the `$t` and `$d` marks an ARM assembler leaves aren't functions.
      if (
        type !== SYMBOL_TYPE_FUNCTION &&
        !(type === SYMBOL_TYPE_NONE && isCode(section))
      ) {
        continue;
      }
      const nameAt = names.offset + view.getUint32(at, true);
      const name = new TextDecoder().decode(
        bytes.subarray(nameAt, bytes.indexOf(0, nameAt)),
      );
      if (!name || name.startsWith('$')) {
        continue;
      }
      const value = view.getUint32(at + SYMBOL_VALUE_AT, true);
      functions.push({
        name,
        address:
          machine === ELF_MACHINE_ARM ? (value & ~THUMB_BIT) >>> 0 : value,
        size: view.getUint32(at + SYMBOL_SIZE_AT, true),
        image: 0,
      });
    }
  }
  if (!functions.length) {
    throw new Error(
      'the ELF has no symbols: pass the firmware as it was linked, before it was stripped',
    );
  }
  // A sized symbol ahead of an unsized one at the same address, so a lookup prefers it.
  functions.sort(
    (first, second) =>
      first.address - second.address || second.size - first.size,
  );
  return {
    machine,
    functions,
    code: sections
      .filter(
        section => section.type === SECTION_TYPE_PROGBITS && isCode(section),
      )
      .map(section => ({
        address: section.address,
        bytes: bytes.subarray(section.offset, section.offset + section.size),
      })),
  };
}

/**
 * Joins the ELFs of a firmware built as several images (a bootloader and an application, a base firmware and a
 * module it loads), so a fault's addresses are found whichever image they are in.
 *
 * @param elfs  Each image, from readElf().
 *
 * @returns One Elf with every image's functions and code. A function's `image` is its ELF's place in @p elfs.
 * @throws When there are none, or they are for different processors.
 */
export function mergeElfs(elfs: Elf[]): Elf {
  if (!elfs.length) {
    throw new Error('no ELF given');
  }
  if (elfs.some(elf => elf.machine !== elfs[0].machine)) {
    throw new Error(
      'the ELFs are for different processors: pass the images of one firmware',
    );
  }
  return {
    machine: elfs[0].machine,
    functions: elfs
      .flatMap((elf, image) => elf.functions.map(each => ({...each, image})))
      .sort(
        (first, second) =>
          first.address - second.address || second.size - first.size,
      ),
    code: elfs.flatMap(elf => elf.code),
  };
}

/**
 * Finds the function an address is in.
 *
 * @param elf  The firmware.
 * @param address  The address.
 *
 * @returns The function, or null when none holds the address.
 */
export function functionAt(elf: Elf, address: number): ElfFunction | null {
  let low = 0;
  let high = elf.functions.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (elf.functions[middle].address <= address) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  // Back over unsized symbols to a sized one that holds the address.
  for (let index = low - 1; index >= 0; index--) {
    const candidate = elf.functions[index];
    if (candidate.size > 0) {
      if (address < candidate.address + candidate.size) {
        return candidate;
      }
      break;
    }
  }
  const nearest = low > 0 ? elf.functions[low - 1] : null;
  const inCode =
    nearest !== null &&
    elf.code.some(
      section =>
        nearest.address >= section.address &&
        address >= section.address &&
        address < section.address + section.bytes.length,
    );
  return nearest !== null && nearest.size === 0 && inCode ? nearest : null;
}

/** A 16-bit word of the firmware's code, or null where the ELF has none. */
function codeHalfword(elf: Elf, address: number): number | null {
  for (const section of elf.code) {
    const at = address - section.address;
    if (at >= 0 && at + 2 <= section.bytes.length) {
      return section.bytes[at] | (section.bytes[at + 1] << 8);
    }
  }
  return null;
}

/**
 * The Thumb call that returns to an address.
 *
 * @returns Where the call is, and its target (null for a call through a register); null when no call is there.
 */
function thumbCallBefore(
  elf: Elf,
  returnAddress: number,
): {at: number; target: number | null} | null {
  const first = codeHalfword(elf, returnAddress - THUMB_BL_BYTES);
  const second = codeHalfword(elf, returnAddress - THUMB_BLX_BYTES);
  if (second === null) {
    return null;
  }
  if (
    first !== null &&
    (first & 0xf800) === 0xf000 &&
    (second & 0xd000) === 0xd000
  ) {
    const sign = (first >> 10) & 1;
    const upper = first & 0x3ff;
    const lower = second & 0x7ff;
    const bit23 = ((second >> 13) & 1) ^ sign ^ 1;
    const bit22 = ((second >> 11) & 1) ^ sign ^ 1;
    let distance =
      (sign << 24) |
      (bit23 << 23) |
      (bit22 << 22) |
      (upper << 12) |
      (lower << 1);
    if (sign) {
      distance -= 1 << 25;
    }
    return {
      at: returnAddress - THUMB_BL_BYTES,
      target: (returnAddress + distance) >>> 0,
    };
  }
  if ((second & 0xff87) === 0x4780) {
    return {at: returnAddress - THUMB_BLX_BYTES, target: null};
  }
  return null;
}

/** A function's name without the stub a linker puts in front of a far call. */
const plainName = (name: string): string =>
  name.replace(/^__(.+?)_(?:veneer|from_thumb|from_arm)$/, '$1');

/*----------------------------------------------------------------------------------------------------------------------
 - The call chain
 ---------------------------------------------------------------------------------------------------------------------*/

function frameAt(
  elf: Elf,
  kind: FrameKind,
  address: number,
  note: string,
): FaultFrame {
  const holder = functionAt(elf, address);
  return {
    kind,
    address,
    function: holder ? holder.name : null,
    image: holder ? holder.image : 0,
    offset: holder ? address - holder.address : 0,
    note,
  };
}

/** The chain on a Cortex-M core: the link register and each stack word that returns to a call. */
function thumbChain(record: FaultRecord, elf: Elf): FaultFrame[] {
  const faulting = record.programCounter & ~THUMB_BIT;
  const frames: FaultFrame[] = [
    frameAt(
      elf,
      'fault',
      faulting,
      functionAt(elf, faulting)
        ? 'the faulting instruction'
        : 'not in the code of any ELF given: a call through a bad pointer, or code in another image',
    ),
  ];
  // The function the next real caller must have called, and one an unsure frame offers in its place.
  let callee = frames[0].function;
  let otherCallee: string | null = null;
  let fromLinkRegister: number | null = null;

  const words = [record.linkRegister, ...record.stack];
  words.forEach((word, index) => {
    const isLinkRegister = index === 0;
    if (!isLinkRegister && isExceptionReturn(word)) {
      frames.push({
        kind: 'interrupt',
        address: word,
        function: null,
        image: 0,
        offset: 0,
        note: 'an interrupt was taken here: the frames below it are its handler',
      });
      callee = null;
      otherCallee = null;
      return;
    }
    if ((word & THUMB_BIT) === 0) {
      return;
    }
    const call = thumbCallBefore(elf, (word & ~THUMB_BIT) >>> 0);
    const caller = call ? functionAt(elf, call.at) : null;
    if (!call || !caller) {
      return;
    }
    const targetFunction =
      call.target === null ? null : functionAt(elf, call.target);
    const target = targetFunction ? plainName(targetFunction.name) : null;
    const calls = (name: string | null): boolean =>
      name !== null && target === plainName(name);

    if (isLinkRegister) {
      // The link register is the caller only until the function saves it or calls on: after that it is stale.
      const stale =
        call.target === null ? caller.name === callee : !calls(callee);
      if (stale && callee !== null) {
        return;
      }
      fromLinkRegister = call.at;
    } else if (call.at === fromLinkRegister) {
      // The function saved the link register it still holds.
      fromLinkRegister = null;
      return;
    }

    let kind: FrameKind;
    let note: string;
    if (call.target === null) {
      kind = 'likely';
      note = 'a call through a pointer';
    } else if (calls(callee)) {
      kind = 'sure';
      note = `calls ${callee}`;
    } else if (callee === null || calls(otherCallee)) {
      kind = 'likely';
      note = `calls ${target ?? hex(call.target)}`;
    } else {
      kind = 'unsure';
      note = `calls ${target ?? hex(call.target)}, not ${callee}: a tail call, or a word an earlier call left`;
    }
    const last = frames[frames.length - 1];
    if (last.kind === kind && last.address === call.at) {
      // The same word twice over: a register saved beside the return address.
      return;
    }
    frames.push(frameAt(elf, kind, call.at, note));
    if (kind === 'unsure') {
      otherCallee = caller.name;
    } else {
      callee = caller.name;
      otherCallee = null;
    }
  });
  return frames;
}

/**
 * Rebuilds the C call chain of a fault, innermost first.
 *
 * On a Cortex-M core it comes from the stack words the record holds: each word that is a return address after a
 * call instruction is a frame, and FaultFrame.kind says how well it fits the frame below. A record that holds the
 * callers the board's own unwinder found is taken as it is.
 *
 * @param record  The record.
 * @param elf  The firmware it came from.
 *
 * @returns The frames.
 */
export function callChain(record: FaultRecord, elf: Elf): FaultFrame[] {
  if (record.stackIsCallers) {
    return [
      frameAt(elf, 'fault', record.programCounter, 'the faulting instruction'),
      ...record.stack.map(address =>
        frameAt(elf, 'unwinder', address, "from the board's unwinder"),
      ),
    ];
  }
  if (elf.machine === ELF_MACHINE_ARM && record.core === FAULT_CORE_CORTEX_M) {
    return thumbChain(record, elf);
  }
  const frames = [
    frameAt(elf, 'fault', record.programCounter, 'the faulting instruction'),
  ];
  if (record.linkRegister && functionAt(elf, record.linkRegister)) {
    frames.push(
      frameAt(elf, 'likely', record.linkRegister, 'the link register'),
    );
  }
  return frames;
}

/*----------------------------------------------------------------------------------------------------------------------
 - The report
 ---------------------------------------------------------------------------------------------------------------------*/

/** The set bits of a status register, named. */
function statusBits(value: number, bits: [number, string, string][]): string[] {
  return bits
    .filter(([bit]) => (value >>> bit) & 1)
    .map(([, name, meaning]) => `${name}: ${meaning}`);
}

/**
 * Explains a record's fault status registers.
 *
 * @param record  The record.
 *
 * @returns A line for each register that says something.
 */
export function statusLines(record: FaultRecord): string[] {
  const lines: string[] = [];
  if (record.core === FAULT_CORE_CORTEX_M) {
    const [cfsr, hfsr] = record.status;
    if (cfsr) {
      lines.push(
        `CFSR ${hex(cfsr)}`,
        ...statusBits(cfsr, CFSR_BITS).map(bit => `  ${bit}`),
      );
    }
    if (hfsr) {
      lines.push(
        `HFSR ${hex(hfsr)}`,
        ...statusBits(hfsr, HFSR_BITS).map(bit => `  ${bit}`),
      );
    }
  } else if (record.core === FAULT_CORE_XTENSA) {
    const [cause] = record.status;
    lines.push(
      `EXCCAUSE ${cause}${XTENSA_CAUSES[cause] ? `: ${XTENSA_CAUSES[cause]}` : ''}`,
    );
  } else if (record.core === FAULT_CORE_RISCV) {
    lines.push(`mcause ${hex(record.status[0])}`);
  }
  return lines;
}

/** True when the record's address is one the fault touched: 0 is an address too, when the core says it is. */
function hasAddress(record: FaultRecord): boolean {
  if (record.core === FAULT_CORE_CORTEX_M) {
    return (record.status[0] & (CFSR_BFARVALID | CFSR_MMARVALID)) !== 0;
  }
  return record.core === FAULT_CORE_XTENSA || record.address !== 0;
}

/**
 * Writes the report `embedded-react decode-fault` prints.
 *
 * @param record  The record.
 * @param frames  Its call chain, from callChain().
 * @param sourceLines  Each frame's `file:line` from the ELF's debug info, by the frame's address; a frame without
 *                     one prints its function alone.
 * @param appLine  The app's source line, or null when the app wasn't running.
 *
 * @returns The lines.
 */
export function faultReport(
  record: FaultRecord,
  frames: FaultFrame[],
  sourceLines: Map<number, string>,
  appLine: string | null,
): string[] {
  const lines = [record.reason || 'fault'];
  lines.push(`  pc      ${hex(record.programCounter)}`);
  if (hasAddress(record)) {
    lines.push(`  address ${hex(record.address)}`);
  }
  lines.push(`  lr      ${hex(record.linkRegister)}`);
  lines.push(`  sp      ${hex(record.stackPointer)}`);
  lines.push(...statusLines(record).map(line => `  ${line}`));

  const fromStack =
    !record.stackIsCallers && record.core === FAULT_CORE_CORTEX_M;
  lines.push(
    '',
    fromStack
      ? `C call chain, rebuilt from ${record.stack.length} saved stack words:`
      : 'C call chain:',
  );
  const places = frames.map(frame =>
    frame.kind === 'interrupt'
      ? ''
      : frame.function
        ? `${frame.function}+0x${frame.offset.toString(16)}`
        : '??',
  );
  const sources = frames.map(frame => sourceLines.get(frame.address) ?? '');
  const placeWidth = Math.max(...places.map(place => place.length));
  const sourceWidth = Math.max(...sources.map(source => source.length));
  let depth = 0;
  frames.forEach((frame, index) => {
    if (frame.kind === 'interrupt') {
      lines.push(`      -- ${frame.note} --`);
      return;
    }
    const sureness =
      frame.kind === 'fault' || frame.kind === 'unwinder'
        ? frame.note
        : `${frame.kind}: ${frame.note}`;
    lines.push(
      `  #${String(depth++).padEnd(2)} ${hex(frame.address)}  ${places[index].padEnd(placeWidth)}  ` +
        `${sourceWidth ? `${sources[index].padEnd(sourceWidth)}  ` : ''}[${sureness}]`,
    );
  });
  if (fromStack && record.stack.length > 0) {
    lines.push(
      `  The chain stops ${record.stack.length * WORD_BYTES} bytes up the stack: callers past that aren't in the record.`,
    );
  }
  lines.push('', appLine ? `App: ${appLine}` : "App: its code wasn't running");
  return lines;
}
