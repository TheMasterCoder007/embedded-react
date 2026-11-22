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

import {describe, expect, it} from 'vitest';
import {
  ELF_MACHINE_ARM,
  FAULT_CORE_CORTEX_M,
  FAULT_CORE_XTENSA,
  callChain,
  faultReport,
  functionAt,
  mergeElfs,
  parseFaultRecord,
  readElf,
  statusLines,
} from '../fault-decode.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - A fault record, built as engine/rendering/error_screen.c builds one
 ---------------------------------------------------------------------------------------------------------------------*/

interface RecordSpec {
  version?: number;
  core?: number;
  reason?: string;
  programCounter?: number;
  linkRegister?: number;
  address?: number;
  status?: number[];
  app?: {
    function: number;
    panel: number;
    offset: number;
    buildId: number;
    file: string;
    line: number;
  };
  stack?: number[];
  callers?: boolean;
  capacity?: number;
}

const STACK_AT = 136;

function recordBytes(spec: RecordSpec): Buffer {
  const stack = spec.stack ?? [];
  const bytes = Buffer.alloc(STACK_AT + 4 * (spec.capacity ?? 32) + 4);
  bytes.write('TFRE', 0, 'latin1');
  bytes[4] = spec.version ?? 1;
  bytes[5] = spec.core ?? FAULT_CORE_CORTEX_M;
  bytes[6] = (spec.app ? 1 : 0) | (spec.callers ? 2 : 0);
  bytes[7] = stack.length;
  bytes.writeUInt32LE(spec.programCounter ?? 0, 8);
  bytes.writeUInt32LE(spec.linkRegister ?? 0, 12);
  bytes.writeUInt32LE(0x20001000, 16);
  bytes.writeUInt32LE(spec.address ?? 0, 20);
  (spec.status ?? []).forEach((word, index) =>
    bytes.writeUInt32LE(word, 24 + 4 * index),
  );
  if (spec.app) {
    bytes.writeUInt32LE(spec.app.offset, 40);
    bytes.writeUInt32LE(spec.app.buildId, 44);
    bytes.writeUInt32LE(spec.app.line, 48);
    bytes.writeUInt16LE(spec.app.function, 52);
    bytes.writeUInt16LE(spec.app.panel, 54);
    bytes.write(spec.app.file, 56, 'latin1');
  }
  bytes.write(spec.reason ?? 'hard fault', 88, 'latin1');
  stack.forEach((word, index) =>
    bytes.writeUInt32LE(word, STACK_AT + 4 * index),
  );
  let hash = 2166136261;
  for (let index = 0; index < bytes.length - 4; index++) {
    hash = Math.imul(hash ^ bytes[index], 16777619) >>> 0;
  }
  bytes.writeUInt32LE(hash, bytes.length - 4);
  return bytes;
}

const recordText = (spec: RecordSpec): string =>
  recordBytes(spec).toString('hex');

/*----------------------------------------------------------------------------------------------------------------------
 - A small Thumb firmware: leaf <- middle <- top, with a call through a register in top
 ---------------------------------------------------------------------------------------------------------------------*/

const CODE_BASE = 0x08000000;
const NOP = 0xbf00;
const BLX_R3 = 0x4798;

const LEAF = CODE_BASE;
const MIDDLE = CODE_BASE + 0x10;
const TOP = CODE_BASE + 0x20;
const OTHER = CODE_BASE + 0x40;
const MIDDLE_CALLS_LEAF = MIDDLE + 4;
const TOP_CALLS_MIDDLE = TOP + 4;
const TOP_CALLS_POINTER = TOP + 10;
const OTHER_CALLS_TOP = OTHER + 4;
const CODE_BYTES = 0x50;

/** The two halfwords of a Thumb BL at `at` to `target`. */
function branchLink(at: number, target: number): [number, number] {
  const distance = target - (at + 4);
  const sign = distance < 0 ? 1 : 0;
  const bit23 = (distance >> 23) & 1;
  const bit22 = (distance >> 22) & 1;
  return [
    0xf000 | (sign << 10) | ((distance >> 12) & 0x3ff),
    0xd000 |
      ((bit23 ^ sign ^ 1) << 13) |
      ((bit22 ^ sign ^ 1) << 11) |
      ((distance >> 1) & 0x7ff),
  ];
}

/** A Thumb ELF: its code at `base`, and its functions as [name, address, size]. */
function thumbElf(
  base: number,
  code: Buffer,
  functions: [string, number, number][],
): Buffer {
  let names = '\0';
  const symbols = Buffer.alloc(16 * (functions.length + 2));
  functions.forEach(([name, address, size], index) => {
    const at = 16 * (index + 1);
    symbols.writeUInt32LE(names.length, at);
    symbols.writeUInt32LE(address | 1, at + 4);
    symbols.writeUInt32LE(size, at + 8);
    symbols[at + 12] = 0x12; // a global function
    symbols.writeUInt16LE(1, at + 14);
    names += `${name}\0`;
  });
  // A mapping symbol, which is not a function.
  const mappingAt = 16 * (functions.length + 1);
  symbols.writeUInt32LE(names.length, mappingAt);
  symbols.writeUInt32LE(base, mappingAt + 4);
  symbols.writeUInt16LE(1, mappingAt + 14);
  names += '$t\0';
  const strings = Buffer.from(names, 'latin1');

  const HEADER_BYTES = 52;
  const SECTION_BYTES = 40;
  const codeAt = HEADER_BYTES;
  const symbolsAt = codeAt + code.length;
  const stringsAt = symbolsAt + symbols.length;
  const sectionsAt = stringsAt + strings.length;
  const header = Buffer.alloc(HEADER_BYTES);
  header.set([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1]);
  header.writeUInt16LE(2, 16);
  header.writeUInt16LE(ELF_MACHINE_ARM, 18);
  header.writeUInt32LE(sectionsAt, 32);
  header.writeUInt16LE(SECTION_BYTES, 46);
  header.writeUInt16LE(4, 48);
  const section = (
    type: number,
    flags: number,
    address: number,
    offset: number,
    size: number,
    link: number,
  ): Buffer => {
    const bytes = Buffer.alloc(SECTION_BYTES);
    bytes.writeUInt32LE(type, 4);
    bytes.writeUInt32LE(flags, 8);
    bytes.writeUInt32LE(address, 12);
    bytes.writeUInt32LE(offset, 16);
    bytes.writeUInt32LE(size, 20);
    bytes.writeUInt32LE(link, 24);
    return bytes;
  };
  return Buffer.concat([
    header,
    code,
    symbols,
    strings,
    Buffer.alloc(SECTION_BYTES),
    section(1, 0x6, base, codeAt, code.length, 0),
    section(2, 0, 0, symbolsAt, symbols.length, 3),
    section(3, 0, 0, stringsAt, strings.length, 0),
  ]);
}

/** Code of `bytes` no-ops at `base`, with a BL at each [at, target]. */
function thumbCode(
  base: number,
  bytes: number,
  calls: [number, number][],
): Buffer {
  const code = Buffer.alloc(bytes);
  for (let at = 0; at < bytes; at += 2) {
    code.writeUInt16LE(NOP, at);
  }
  for (const [at, target] of calls) {
    const [first, second] = branchLink(at, target);
    code.writeUInt16LE(first, at - base);
    code.writeUInt16LE(second, at - base + 2);
  }
  return code;
}

function firmwareElf(): Buffer {
  const code = thumbCode(CODE_BASE, CODE_BYTES, [
    [MIDDLE_CALLS_LEAF, LEAF],
    [TOP_CALLS_MIDDLE, MIDDLE],
    [OTHER_CALLS_TOP, TOP],
  ]);
  code.writeUInt16LE(BLX_R3, TOP_CALLS_POINTER - CODE_BASE);
  return thumbElf(CODE_BASE, code, [
    ['leaf', LEAF, 0x10],
    ['middle', MIDDLE, 0x10],
    ['top', TOP, 0x20],
    ['other', OTHER, 0x10],
  ]);
}

/* A second image, as a base firmware is to a module it loads: `driver` is called through a pointer, and `task`
 * calls the first image's `other`. */
const BASE_IMAGE = CODE_BASE + 0x20000;
const DRIVER = BASE_IMAGE;
const TASK = BASE_IMAGE + 0x10;
const TASK_CALLS_OTHER = TASK + 4;

const baseImageElf = (): Buffer =>
  thumbElf(
    BASE_IMAGE,
    thumbCode(BASE_IMAGE, 0x20, [[TASK_CALLS_OTHER, OTHER]]),
    [
      ['driver', DRIVER, 0x10],
      ['task', TASK, 0x10],
    ],
  );

const elf = readElf(firmwareElf());

/** The return address a call leaves: past the call, with the Thumb bit. */
const afterCall = (at: number): number => (at + 4) | 1;
const afterPointerCall = (at: number): number => (at + 2) | 1;

const chain = (spec: RecordSpec) =>
  callChain(parseFaultRecord(recordText(spec)), elf).map(
    frame => `${frame.kind} ${frame.function ?? '??'}`,
  );

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

describe('parseFaultRecord', () => {
  const spec: RecordSpec = {
    reason: 'hard fault: bus error',
    programCounter: 0x08000124,
    linkRegister: 0x08000301,
    address: 0x24,
    status: [0x8200, 0x40000000, 0, 0x24],
    app: {
      function: 7,
      panel: 1,
      offset: 42,
      buildId: 0x1a2b3c4d,
      file: 'App.tsx',
      line: 41,
    },
    stack: [1, 2, 0xfffffff9],
  };

  it('reads every field', () => {
    expect(parseFaultRecord(recordText(spec))).toEqual({
      core: FAULT_CORE_CORTEX_M,
      reason: 'hard fault: bus error',
      programCounter: 0x08000124,
      linkRegister: 0x08000301,
      stackPointer: 0x20001000,
      address: 0x24,
      status: [0x8200, 0x40000000, 0, 0x24],
      app: spec.app,
      stack: [1, 2, 0xfffffff9],
      stackIsCallers: false,
    });
  });

  it('finds the record in a log line, in either case', () => {
    const line = `E (3012) embedded-react: fault record: ${recordText(spec).toUpperCase()}\n`;
    expect(parseFaultRecord(line).programCounter).toBe(0x08000124);
  });

  it("reads the record's bytes, and a text file's", () => {
    expect(parseFaultRecord(recordBytes(spec)).address).toBe(0x24);
    expect(
      parseFaultRecord(Buffer.from(`fault record: ${recordText(spec)}`))
        .address,
    ).toBe(0x24);
  });

  it('reads a record with another stack size', () => {
    const small = parseFaultRecord(
      recordText({stack: [5, 6], capacity: 8, callers: true}),
    );
    expect(small.stack).toEqual([5, 6]);
    expect(small.stackIsCallers).toBe(true);
    expect(small.app).toBeNull();
  });

  it('refuses text with no record, and a cut or changed one', () => {
    expect(() => parseFaultRecord('restarted after a fault')).toThrow(
      /no fault record/,
    );
    const text = recordText(spec);
    expect(() => parseFaultRecord(text.slice(0, -8))).toThrow(/cut short/);
    expect(() => parseFaultRecord(text.slice(0, -6))).toThrow(/cut short/);
    const changed =
      text.slice(0, 20) + (text[20] === '0' ? '1' : '0') + text.slice(21);
    expect(() => parseFaultRecord(changed)).toThrow(/check word/);
  });

  it('refuses another version', () => {
    expect(() => parseFaultRecord(recordText({version: 2}))).toThrow(
      /version 2/,
    );
  });
});

describe('readElf', () => {
  it('reads the functions, without the Thumb bit or mapping symbols', () => {
    expect(elf.machine).toBe(ELF_MACHINE_ARM);
    expect(elf.functions.map(each => each.name)).toEqual([
      'leaf',
      'middle',
      'top',
      'other',
    ]);
    expect(functionAt(elf, MIDDLE + 6)?.name).toBe('middle');
    expect(functionAt(elf, CODE_BASE + 0x1000)).toBeNull();
    expect(functionAt(elf, 0)).toBeNull();
  });

  it('refuses what is not a firmware ELF', () => {
    expect(() => readElf(Buffer.from('UF2\n'.padEnd(64)))).toThrow(
      /not a 32-bit little-endian ELF/,
    );
    const stripped = firmwareElf();
    stripped.writeUInt16LE(2, 48); // only the null and code sections
    expect(() => readElf(stripped)).toThrow(/no symbols/);
  });
});

describe('callChain', () => {
  it('follows return addresses that call the frame below', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [
          0x20000100,
          afterCall(MIDDLE_CALLS_LEAF),
          0,
          afterCall(TOP_CALLS_MIDDLE),
          afterCall(OTHER_CALLS_TOP),
        ],
      }),
    ).toEqual(['fault leaf', 'sure middle', 'sure top', 'sure other']);
  });

  it('takes the caller from the link register of a leaf', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        linkRegister: afterCall(MIDDLE_CALLS_LEAF),
        stack: [afterCall(TOP_CALLS_MIDDLE)],
      }),
    ).toEqual(['fault leaf', 'sure middle', 'sure top']);
  });

  it('counts a link register the function also saved once', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        linkRegister: afterCall(MIDDLE_CALLS_LEAF),
        stack: [afterCall(MIDDLE_CALLS_LEAF), afterCall(TOP_CALLS_MIDDLE)],
      }),
    ).toEqual(['fault leaf', 'sure middle', 'sure top']);
  });

  it('drops a stale link register', () => {
    // Left by a call the faulting function made itself.
    expect(
      chain({
        programCounter: TOP + 0x10,
        linkRegister: afterCall(TOP_CALLS_MIDDLE),
        stack: [afterCall(OTHER_CALLS_TOP)],
      }),
    ).toEqual(['fault top', 'sure other']);
    expect(
      chain({
        programCounter: TOP + 0x10,
        linkRegister: afterPointerCall(TOP_CALLS_POINTER),
      }),
    ).toEqual(['fault top']);
  });

  it('marks a call through a pointer as likely', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [
          afterPointerCall(TOP_CALLS_POINTER),
          afterCall(OTHER_CALLS_TOP),
        ],
      }),
    ).toEqual(['fault leaf', 'likely top', 'sure other']);
  });

  it('marks a call to another function as unsure, and goes on from either', () => {
    // top calls middle, but the frame below is leaf: middle jumped to it, or the word is old.
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [afterCall(TOP_CALLS_MIDDLE), afterCall(OTHER_CALLS_TOP)],
      }),
    ).toEqual(['fault leaf', 'unsure top', 'likely other']);
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [afterCall(TOP_CALLS_MIDDLE), afterCall(MIDDLE_CALLS_LEAF)],
      }),
    ).toEqual(['fault leaf', 'unsure top', 'sure middle']);
  });

  it('skips words that are not return addresses', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [
          MIDDLE_CALLS_LEAF + 4, // no Thumb bit
          (MIDDLE + 12) | 1, // not after a call
          0x20000001,
          afterCall(MIDDLE_CALLS_LEAF),
        ],
      }),
    ).toEqual(['fault leaf', 'sure middle']);
  });

  it('marks where an interrupt was taken', () => {
    expect(
      chain({
        programCounter: LEAF + 4,
        stack: [
          afterCall(MIDDLE_CALLS_LEAF),
          0xfffffff9,
          afterCall(OTHER_CALLS_TOP),
        ],
      }),
    ).toEqual(['fault leaf', 'sure middle', 'interrupt ??', 'likely other']);
  });

  it('starts from the callers when the pc is not in the code', () => {
    const frames = callChain(
      parseFaultRecord(
        recordText({
          programCounter: 0,
          linkRegister: afterPointerCall(TOP_CALLS_POINTER),
          stack: [afterCall(OTHER_CALLS_TOP)],
        }),
      ),
      elf,
    );
    expect(
      frames.map(frame => `${frame.kind} ${frame.function ?? '??'}`),
    ).toEqual(['fault ??', 'likely top', 'sure other']);
    expect(frames[0].note).toMatch(/bad pointer/);
  });

  it("takes the board's own callers as they are", () => {
    expect(
      chain({
        core: FAULT_CORE_XTENSA,
        programCounter: LEAF + 4,
        stack: [MIDDLE + 4, TOP + 4],
        callers: true,
      }),
    ).toEqual(['fault leaf', 'unwinder middle', 'unwinder top']);
  });
});

describe('mergeElfs', () => {
  const both = mergeElfs([elf, readElf(baseImageElf())]);
  const framesOf = (spec: RecordSpec, images: typeof both) =>
    callChain(parseFaultRecord(recordText(spec)), images).map(
      frame => `${frame.kind} ${frame.function ?? '??'} ${frame.image}`,
    );

  it('finds a function in whichever image holds it', () => {
    expect(functionAt(both, MIDDLE + 2)).toMatchObject({
      name: 'middle',
      image: 0,
    });
    expect(functionAt(both, TASK + 2)).toMatchObject({name: 'task', image: 1});
    expect(both.functions.map(each => each.address)).toEqual(
      [...both.functions.map(each => each.address)].sort(
        (first, second) => first - second,
      ),
    );
  });

  it('follows a chain from one image into the other', () => {
    // A fault in the base image's driver, called through a pointer from the module.
    const fault: RecordSpec = {
      programCounter: DRIVER + 4,
      linkRegister: afterPointerCall(TOP_CALLS_POINTER),
      stack: [afterCall(OTHER_CALLS_TOP), afterCall(TASK_CALLS_OTHER)],
    };
    expect(framesOf(fault, both)).toEqual([
      'fault driver 1',
      'likely top 0',
      'sure other 0',
      'sure task 1',
    ]);
    // With the module's ELF alone, the other image's frames can't be named.
    const alone = callChain(parseFaultRecord(recordText(fault)), elf);
    expect(
      alone.map(frame => `${frame.kind} ${frame.function ?? '??'}`),
    ).toEqual(['fault ??', 'likely top', 'sure other']);
    expect(alone[0].note).toMatch(/another image/);
  });

  it('refuses no ELF, and ELFs for different processors', () => {
    expect(() => mergeElfs([])).toThrow(/no ELF/);
    expect(() => mergeElfs([elf, {...elf, machine: 94}])).toThrow(
      /different processors/,
    );
  });
});

describe('faultReport', () => {
  it('explains the status registers', () => {
    const lines = statusLines(
      parseFaultRecord(recordText({status: [0x8200, 0x40000000, 0, 0x24]})),
    );
    expect(lines.join('\n')).toMatch(
      /CFSR 0x00008200\n {2}PRECISERR.*\n {2}BFARVALID/,
    );
    expect(lines.join('\n')).toMatch(/HFSR 0x40000000\n {2}FORCED/);
    expect(
      statusLines(
        parseFaultRecord(
          recordText({core: FAULT_CORE_XTENSA, status: [28, 0]}),
        ),
      ),
    ).toEqual(['EXCCAUSE 28: LoadProhibited']);
    expect(statusLines(parseFaultRecord(recordText({})))).toEqual([]);
  });

  it('prints the chain with how sure each frame is, and the app line', () => {
    const record = parseFaultRecord(
      recordText({
        reason: 'hard fault: bus error',
        programCounter: LEAF + 4,
        address: 0x24,
        status: [0x8200],
        stack: [
          afterCall(MIDDLE_CALLS_LEAF),
          afterPointerCall(TOP_CALLS_POINTER),
        ],
      }),
    );
    const report = faultReport(
      record,
      callChain(record, elf),
      new Map([[LEAF + 4, 'sensor.c:88']]),
      'App.tsx:41:7  onPress  (build 1a2b3c4d)',
    );
    expect(report).toEqual([
      'hard fault: bus error',
      '  pc      0x08000004',
      '  address 0x00000024',
      '  lr      0x00000000',
      '  sp      0x20001000',
      '  CFSR 0x00008200',
      '    PRECISERR: bus error at the faulting instruction',
      '    BFARVALID: BFAR holds the address',
      '',
      'C call chain, rebuilt from 2 saved stack words:',
      '  #0  0x08000004  leaf+0x4    sensor.c:88  [the faulting instruction]',
      '  #1  0x08000014  middle+0x4               [sure: calls leaf]',
      '  #2  0x0800002a  top+0xa                  [likely: a call through a pointer]',
      "  The chain stops 8 bytes up the stack: callers past that aren't in the record.",
      '',
      'App: App.tsx:41:7  onPress  (build 1a2b3c4d)',
    ]);
  });

  it('shows a null address the core says it touched, and no address it does not', () => {
    const nullRead = parseFaultRecord(
      recordText({core: FAULT_CORE_XTENSA, status: [28, 0], callers: true}),
    );
    expect(
      faultReport(nullRead, callChain(nullRead, elf), new Map(), null),
    ).toContain('  address 0x00000000');
    const noAddress = parseFaultRecord(recordText({programCounter: LEAF}));
    expect(
      faultReport(noAddress, callChain(noAddress, elf), new Map(), null).join(),
    ).not.toMatch(/address/);
  });

  it("says when the app wasn't running", () => {
    const record = parseFaultRecord(recordText({programCounter: LEAF}));
    expect(
      faultReport(record, callChain(record, elf), new Map(), null).at(-1),
    ).toBe("App: its code wasn't running");
  });
});
