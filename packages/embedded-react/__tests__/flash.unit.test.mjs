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
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {
  emitContainer,
  emitPackageContainer,
} from '../assets/emit-container.mjs';
import {
  BOARDS,
  SLOT_HEADER_BYTES,
  boardByName,
  checkPackage,
  flashSlot,
  packSlot,
  pickSlot,
  readSlotSequence,
  slotImage,
  slotsFromPartitionTable,
  uf2,
} from '../flash.mjs';

const examples = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../../../examples',
);

// A program section's contents don't matter here: only the container around it is read.
const packageBytes = () =>
  Buffer.from(emitPackageContainer({program: Buffer.alloc(32, 0x5a)}).bytes);

const SLOT_BYTES = 0x1000;

/** The same 16 bytes as runtime/tests/erb_package_slots_test.c, so the writer and the reader can't drift apart. */
const GOLDEN_HEADER = Buffer.from([
  0x45, 0x52, 0x42, 0x53, 0x07, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0xb5,
  0xd9, 0x79, 0x48,
]);

/** A partition table with the given (label, address, bytes) entries, closed by an erased entry. */
function partitionTable(entries) {
  const table = Buffer.alloc(0xc00, 0xff);
  entries.forEach(([label, address, bytes], index) => {
    const at = index * 32;
    table.writeUInt16LE(0x50aa, at);
    table.writeUInt8(1, at + 2);
    table.writeUInt8(0x41, at + 3);
    table.writeUInt32LE(address, at + 4);
    table.writeUInt32LE(bytes, at + 8);
    table.fill(0, at + 12, at + 28);
    table.write(label, at + 12, 'latin1');
    table.writeUInt32LE(0, at + 28);
  });
  return table;
}

/**
 * A board's flash behind a fake esptool or picotool: reads hand back what the map holds, and writes are recorded.
 */
function fakeBoard(flash) {
  const writes = [];
  const calls = [];
  const run = (command, args) => {
    calls.push([command, ...args]);
    const read = args.indexOf('read_flash');
    if (read >= 0) {
      const [address, , file] = args.slice(read + 1);
      writeFileSync(file, flash[Number(address)] ?? Buffer.alloc(0x100, 0xff));
    }
    if (args[0] === 'save') {
      writeFileSync(
        args[4],
        flash[Number(args[2])] ?? Buffer.alloc(0x100, 0xff),
      );
    }
    const write = args.indexOf('write_flash');
    if (write >= 0) {
      writes.push({
        address: Number(args[write + 1]),
        image: readFileSync(args[write + 2]),
      });
    }
    if (args[0] === 'load') {
      writes.push({address: Number(args[5]), image: readFileSync(args[1])});
    }
    return {status: 0, stdout: '', stderr: ''};
  };
  return {run, writes, calls};
}

describe('slot images', () => {
  it('writes the header the firmware reads', () => {
    const image = slotImage(Buffer.alloc(0x1234, 0xab), 7);
    expect(image.subarray(0, 16)).toEqual(GOLDEN_HEADER);
    expect(image.length).toBe(SLOT_HEADER_BYTES + 0x1234);
    expect(image.subarray(16, SLOT_HEADER_BYTES).every(b => b === 0xff)).toBe(
      true,
    );
    expect(image[SLOT_HEADER_BYTES]).toBe(0xab);
  });

  it('reads a sequence back, and only from a valid header', () => {
    const image = slotImage(Buffer.alloc(16), 42);
    expect(readSlotSequence(image, SLOT_BYTES)).toBe(42);
    expect(readSlotSequence(Buffer.alloc(256, 0xff), SLOT_BYTES)).toBeNull();
    expect(readSlotSequence(Buffer.alloc(256, 0), SLOT_BYTES)).toBeNull();
    const damaged = Buffer.from(image);
    damaged[4] ^= 1;
    expect(readSlotSequence(damaged, SLOT_BYTES)).toBeNull();
    const big = slotImage(Buffer.alloc(SLOT_BYTES), 1);
    expect(readSlotSequence(big, SLOT_BYTES)).toBeNull();
  });
});

describe('picking a slot', () => {
  it('starts in the first slot', () => {
    expect(pickSlot([null, null])).toEqual({
      slot: 0,
      sequence: 1,
      lastGood: null,
    });
  });

  it('writes over the older slot, and never the newest', () => {
    expect(pickSlot([5, null])).toEqual({slot: 1, sequence: 6, lastGood: 0});
    expect(pickSlot([null, 5])).toEqual({slot: 0, sequence: 6, lastGood: 1});
    expect(pickSlot([5, 6])).toEqual({slot: 0, sequence: 7, lastGood: 1});
    expect(pickSlot([6, 5])).toEqual({slot: 1, sequence: 7, lastGood: 0});
  });

  it('keeps the order past the sequence wrapping, as the firmware does', () => {
    expect(pickSlot([0xffffffff, 1])).toEqual({
      slot: 0,
      sequence: 2,
      lastGood: 1,
    });
    expect(pickSlot([0xffffffff, null])).toEqual({
      slot: 1,
      sequence: 0,
      lastGood: 0,
    });
  });
});

describe('checking a package', () => {
  it('takes a Flow B package', () => {
    expect(() => checkPackage(packageBytes())).not.toThrow();
  });

  it('refuses anything else', () => {
    expect(() => checkPackage(Buffer.from('not a package'))).toThrow(
      /Not a package/,
    );
    const flowA = Buffer.from(
      emitContainer({bytecode: Buffer.alloc(8), qjsTag: 'v0.15.0'}),
    );
    expect(() => checkPackage(flowA)).toThrow(
      /Flow A container \(built for v0\.15\.0\)/,
    );
    const corrupt = packageBytes();
    corrupt[corrupt.length - 1] ^= 1;
    expect(() => checkPackage(corrupt)).toThrow(/corrupt/);
  });
});

describe('UF2', () => {
  it('writes whole pages from the address up, with the RP2040 family', () => {
    const image = Buffer.alloc(300, 0xcd);
    const file = uf2(image, 0x10100000);
    expect(file.length).toBe(1024);
    expect(file.readUInt32LE(0)).toBe(0x0a324655);
    expect(file.readUInt32LE(4)).toBe(0x9e5d5157);
    expect(file.readUInt32LE(12)).toBe(0x10100000);
    expect(file.readUInt32LE(16)).toBe(256);
    expect(file.readUInt32LE(24)).toBe(2);
    expect(file.readUInt32LE(28)).toBe(0xe48bff56);
    expect(file.readUInt32LE(508)).toBe(0x0ab16f30);
    expect(file.readUInt32LE(512 + 12)).toBe(0x10100100);
    expect(file.readUInt32LE(512 + 20)).toBe(1);
    expect(file[512 + 32 + 43]).toBe(0xcd);
    expect(file[512 + 32 + 44]).toBe(0xff); // the rest of the last page stays erased
  });
});

describe('the partition table', () => {
  it('finds the slots', () => {
    const table = partitionTable([
      ['nvs', 0x9000, 0x6000],
      ['erb_b', 0x2c0000, 0x140000],
      ['erb_a', 0x180000, 0x140000],
    ]);
    expect(slotsFromPartitionTable(table)).toEqual([
      {address: 0x180000, bytes: 0x140000},
      {address: 0x2c0000, bytes: 0x140000},
    ]);
  });

  it('says when the firmware on the board has none', () => {
    expect(
      slotsFromPartitionTable(partitionTable([['config', 0x310000, 0x200000]])),
    ).toBeNull();
  });
});

describe('flashing', () => {
  const espSlots = [
    ['erb_a', 0x180000, 0x140000],
    ['erb_b', 0x2c0000, 0x140000],
  ];

  it('writes the first package to the first slot', () => {
    const board = fakeBoard({0x8000: partitionTable(espSlots)});
    const written = flashSlot({
      packageBytes: packageBytes(),
      board: BOARDS['esp32-2432s028r'],
      port: '/dev/cu.test',
      run: board.run,
    });
    expect(written).toEqual({
      address: 0x180000,
      sequence: 1,
      slot: 'a',
      lastGood: null,
    });
    expect(board.writes).toHaveLength(1);
    expect(board.writes[0].address).toBe(0x180000);
    expect(board.writes[0].image.subarray(SLOT_HEADER_BYTES)).toEqual(
      packageBytes(),
    );
    expect(board.calls[0]).toEqual(
      expect.arrayContaining(['--chip', 'esp32', '--port', '/dev/cu.test']),
    );
  });

  it('leaves the last good package alone', () => {
    const board = fakeBoard({
      0x8000: partitionTable(espSlots),
      0x180000: slotImage(Buffer.alloc(16), 9).subarray(0, 256),
      0x2c0000: slotImage(Buffer.alloc(16), 8).subarray(0, 256),
    });
    const written = flashSlot({
      packageBytes: packageBytes(),
      board: BOARDS['esp32-2432s028r'],
      run: board.run,
    });
    expect(written).toEqual({
      address: 0x2c0000,
      sequence: 10,
      slot: 'b',
      lastGood: 'a',
    });
  });

  it("uses the board's own partition table over the example's", () => {
    const board = fakeBoard({
      0x8000: partitionTable([
        ['erb_a', 0x200000, 0x100000],
        ['erb_b', 0x300000, 0x100000],
      ]),
    });
    const written = flashSlot({
      packageBytes: packageBytes(),
      board: BOARDS['esp32-2432s028r'],
      run: board.run,
    });
    expect(written.address).toBe(0x200000);
  });

  it('refuses a board with no slots, and a package too big for one', () => {
    const noSlots = fakeBoard({
      0x8000: partitionTable([['config', 0x310000, 0x200000]]),
    });
    expect(() =>
      flashSlot({
        packageBytes: packageBytes(),
        board: BOARDS['esp32-s3'],
        run: noSlots.run,
      }),
    ).toThrow(/Flash this example's Flow B firmware first/);
    expect(noSlots.writes).toHaveLength(0);

    const small = fakeBoard({
      0x8000: partitionTable([
        ['erb_a', 0x200000, 0x100],
        ['erb_b', 0x300000, 0x100],
      ]),
    });
    expect(() =>
      flashSlot({
        packageBytes: packageBytes(),
        board: BOARDS['esp32-s3'],
        run: small.run,
      }),
    ).toThrow(/slots hold 256 B/);
    expect(small.writes).toHaveLength(0);
  });

  it('flashes an RP2040 through picotool, and reboots it', () => {
    const board = fakeBoard({
      0x10100000: slotImage(Buffer.alloc(16), 3).subarray(0, 256),
    });
    const written = flashSlot({
      packageBytes: packageBytes(),
      board: BOARDS['rp2040-touch-lcd-1.69'],
      run: board.run,
    });
    expect(written).toEqual({
      address: 0x10180000,
      sequence: 4,
      slot: 'b',
      lastGood: 'a',
    });
    expect(board.calls[0].slice(0, 4)).toEqual([
      'picotool',
      'save',
      '-r',
      '0x10100000',
    ]);
    expect(board.calls.at(-1)).toEqual(['picotool', 'reboot', '-f']);
  });

  it('says how to get a tool that is missing, and shows a failing one', () => {
    const missing = () => ({error: {code: 'ENOENT'}});
    expect(() =>
      flashSlot({
        packageBytes: packageBytes(),
        board: BOARDS['esp32-s3'],
        run: missing,
      }),
    ).toThrow(/esptool wasn't found.*export\.sh/);
    const failing = () => ({status: 2, stderr: 'no serial port', stdout: ''});
    expect(() =>
      flashSlot({
        packageBytes: packageBytes(),
        board: BOARDS['esp32-s3'],
        run: failing,
      }),
    ).toThrow(/no serial port/);
  });
});

describe('packing', () => {
  const inDir = fn => {
    const outDir = mkdtempSync(join(tmpdir(), 'er-pack-'));
    try {
      fn(outDir);
    } finally {
      rmSync(outDir, {recursive: true, force: true});
    }
  };

  it('writes an ESP32 slot image as a raw file', () =>
    inDir(outDir => {
      const packed = packSlot({
        packageBytes: packageBytes(),
        board: BOARDS['esp32-s3'],
        boardName: 'esp32-s3',
        slotName: 'b',
        sequence: 3,
        outDir,
      });
      expect(packed.address).toBe(0x710000);
      expect(packed.file.endsWith('app.slot-b.bin')).toBe(true);
      expect(readSlotSequence(readFileSync(packed.file), 0x200000)).toBe(3);
    }));

  it('writes an RP2040 slot image as a UF2, at the slot', () =>
    inDir(outDir => {
      const packed = packSlot({
        packageBytes: packageBytes(),
        board: BOARDS['rp2040-touch-lcd-1.69'],
        boardName: 'rp2040-touch-lcd-1.69',
        outDir,
      });
      expect(packed.file.endsWith('app.slot-a.uf2')).toBe(true);
      expect(readFileSync(packed.file).readUInt32LE(12)).toBe(0x10100000);
    }));

  it('refuses a slot that does not exist and a package that does not fit', () =>
    inDir(outDir => {
      const board = BOARDS['esp32-s3'];
      expect(() =>
        packSlot({packageBytes: packageBytes(), board, slotName: 'c', outDir}),
      ).toThrow(/--slot takes a or b/);
      const tiny = {
        ...board,
        slots: [
          {address: 0, bytes: 16},
          {address: 16, bytes: 16},
        ],
      };
      expect(() =>
        packSlot({
          packageBytes: packageBytes(),
          board: tiny,
          boardName: 'tiny',
          outDir,
        }),
      ).toThrow(/tiny's slots hold 16 B/);
    }));
});

describe('boards', () => {
  it('names the boards there are when one is wrong', () => {
    expect(() => boardByName('nope')).toThrow(/Unknown board "nope".*esp32-s3/);
    expect(() => boardByName(undefined)).toThrow(/No board given/);
  });

  it.each([
    ['esp32-2432s028r', 'esp32/esp32-2432s028r/partitions.csv'],
    ['esp32-s3', 'esp32/esp32-s3/partitions.csv'],
  ])('%s matches %s', (name, csv) => {
    const lines = readFileSync(resolve(examples, csv), 'utf8').split('\n');
    const slot = label => {
      const [, , , address, bytes] = lines
        .find(line => line.startsWith(label + ','))
        .split(',')
        .map(field => field.trim());
      return {address: Number(address), bytes: Number(bytes)};
    };
    expect(BOARDS[name].slots).toEqual([slot('erb_a'), slot('erb_b')]);
  });

  it('rp2040-touch-lcd-1.69 matches its CMakeLists.txt', () => {
    const cmake = readFileSync(
      resolve(examples, 'rp2040/rp2040-touch-lcd-1.69/CMakeLists.txt'),
      'utf8',
    );
    const value = name =>
      Number(cmake.match(new RegExp(`set\\(${name} (0x[0-9a-f]+)\\)`))[1]);
    const XIP_BASE = 0x10000000;
    expect(BOARDS['rp2040-touch-lcd-1.69'].slots).toEqual([
      {
        address: XIP_BASE + value('ER_PACKAGE_SLOT_A_OFFSET'),
        bytes: value('ER_PACKAGE_SLOT_BYTES'),
      },
      {
        address: XIP_BASE + value('ER_PACKAGE_SLOT_B_OFFSET'),
        bytes: value('ER_PACKAGE_SLOT_BYTES'),
      },
    ]);
  });
});
