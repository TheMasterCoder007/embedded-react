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

// flash — writes a Flow B package into one of a board's two package slots.
//
// A slot holds a 256-byte header, then the package. The firmware (runtime/erb_package_slots.c) runs the valid
// slot with the newest sequence and falls back to the other, so a new package always goes in the slot that
// isn't the newest, with the next sequence. `pack` writes that image to a file, for a factory line or a
// drag-and-drop UF2. `flash` reads the board's headers and writes it.

import {spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {crc32} from './assets/emit-container.mjs';

export const SLOT_HEADER_BYTES = 256;
const SLOT_MAGIC = 0x53425245; // "ERBS", little-endian
const SLOT_HEADER_CHECKED_BYTES = 12;
const SLOT_NAMES = ['a', 'b'];

const CONTAINER_MAGIC = 'ERCF';
const CONTAINER_CRC_END = 12;
const CONTAINER_TAG_AT = 12;
const PACKAGE_TAG = 'erb';

const ESP_PARTITION_TABLE_AT = 0x8000;
const ESP_PARTITION_TABLE_BYTES = 0xc00;
const ESP_PARTITION_ENTRY_BYTES = 32;
const ESP_PARTITION_MAGIC = 0x50aa;
const ESP_PARTITION_LABEL_AT = 12;
const ESP_PARTITION_LABEL_BYTES = 16;
const ESP_SLOT_LABELS = ['erb_a', 'erb_b'];

const UF2_BLOCK_BYTES = 512;
const UF2_PAYLOAD_BYTES = 256;
const UF2_MAGIC_START_0 = 0x0a324655;
const UF2_MAGIC_START_1 = 0x9e5d5157;
const UF2_MAGIC_END = 0x0ab16f30;
const UF2_FLAG_FAMILY = 0x2000;
const UF2_FAMILY_RP2040 = 0xe48bff56;
const UF2_PAYLOAD_AT = 32;
const UF2_MAGIC_END_AT = 508;

/**
 * Where each board example keeps its slots. An ESP32 board's `flash` reads them from the partition table on the
 * board instead, so this is what `pack` prints for it. Keep it in step with the example's partitions.csv or
 * CMakeLists.txt: flash.unit.test.mjs checks that it is.
 */
export const BOARDS = {
  'esp32-2432s028r': {
    tool: 'esptool',
    chip: 'esp32',
    slots: [
      {address: 0x180000, bytes: 0x140000},
      {address: 0x2c0000, bytes: 0x140000},
    ],
  },
  'esp32-s3': {
    tool: 'esptool',
    chip: 'esp32s3',
    slots: [
      {address: 0x510000, bytes: 0x200000},
      {address: 0x710000, bytes: 0x200000},
    ],
  },
  'rp2040-touch-lcd-1.69': {
    tool: 'picotool',
    slots: [
      {address: 0x10100000, bytes: 0x80000},
      {address: 0x10180000, bytes: 0x80000},
    ],
  },
};

const hex = value => `0x${value.toString(16)}`;

/** The board for a --board value; throws naming the ones there are. */
export function boardByName(name) {
  const board = BOARDS[name];
  if (!board) {
    throw new Error(
      `${name ? `Unknown board "${name}"` : 'No board given'}: pass --board with one of ${Object.keys(BOARDS).join(', ')}.`,
    );
  }
  return board;
}

/** Checks the bytes are a whole Flow B package, so a stale or Flow A file never reaches a slot. */
export function checkPackage(bytes) {
  if (
    bytes.length < CONTAINER_TAG_AT + 2 ||
    bytes.toString('latin1', 0, 4) !== CONTAINER_MAGIC
  ) {
    throw new Error('Not a package: no ERCF container in this file.');
  }
  if (bytes.readUInt32LE(8) !== crc32(bytes.subarray(CONTAINER_CRC_END))) {
    throw new Error('This package is corrupt: its CRC does not match.');
  }
  const tagBytes = bytes.readUInt16LE(CONTAINER_TAG_AT);
  const tag = bytes.toString(
    'latin1',
    CONTAINER_TAG_AT + 2,
    CONTAINER_TAG_AT + 2 + tagBytes,
  );
  if (tag !== PACKAGE_TAG) {
    throw new Error(
      `This is a Flow A container (built for ${tag}), not a Flow B package. Build one with: embedded-react build --aot`,
    );
  }
}

/** A slot's image: the header, then the package. */
export function slotImage(packageBytes, sequence) {
  const header = Buffer.alloc(SLOT_HEADER_BYTES, 0xff);
  header.writeUInt32LE(SLOT_MAGIC, 0);
  header.writeUInt32LE(sequence >>> 0, 4);
  header.writeUInt32LE(packageBytes.length, 8);
  header.writeUInt32LE(
    crc32(header.subarray(0, SLOT_HEADER_CHECKED_BYTES)),
    SLOT_HEADER_CHECKED_BYTES,
  );
  return Buffer.concat([header, packageBytes]);
}

/** A slot's sequence from the first bytes of its flash, or null when it has no valid header. */
export function readSlotSequence(bytes, slotBytes) {
  if (
    bytes.length < SLOT_HEADER_CHECKED_BYTES + 4 ||
    bytes.readUInt32LE(0) !== SLOT_MAGIC ||
    bytes.readUInt32LE(SLOT_HEADER_CHECKED_BYTES) !==
      crc32(bytes.subarray(0, SLOT_HEADER_CHECKED_BYTES))
  ) {
    return null;
  }
  const packageBytes = bytes.readUInt32LE(8);
  if (packageBytes === 0 || packageBytes > slotBytes - SLOT_HEADER_BYTES) {
    return null;
  }
  return bytes.readUInt32LE(4);
}

/**
 * Picks the slot for a new package from the two slots' sequences (null for none): the one that isn't the
 * newest, with the next sequence. The firmware makes the same choice (erb_package_slots_boot).
 */
export function pickSlot([first, second]) {
  // A signed difference, so the order survives the sequence wrapping.
  const secondIsNewer =
    second !== null && (first === null || (second - first) << 0 > 0);
  const newest = secondIsNewer ? 1 : 0;
  const newestSequence = secondIsNewer ? second : first;
  if (newestSequence === null) return {slot: 0, sequence: 1, lastGood: null};
  return {
    slot: 1 - newest,
    sequence: (newestSequence + 1) >>> 0,
    lastGood: newest,
  };
}

/** An image as UF2 blocks, for a drag-and-drop onto an RP2040 in BOOTSEL mode. */
export function uf2(image, address) {
  const blockCount = Math.ceil(image.length / UF2_PAYLOAD_BYTES);
  const out = Buffer.alloc(blockCount * UF2_BLOCK_BYTES);
  for (let block = 0; block < blockCount; block++) {
    const at = block * UF2_BLOCK_BYTES;
    out.writeUInt32LE(UF2_MAGIC_START_0, at);
    out.writeUInt32LE(UF2_MAGIC_START_1, at + 4);
    out.writeUInt32LE(UF2_FLAG_FAMILY, at + 8);
    out.writeUInt32LE(address + block * UF2_PAYLOAD_BYTES, at + 12);
    out.writeUInt32LE(UF2_PAYLOAD_BYTES, at + 16);
    out.writeUInt32LE(block, at + 20);
    out.writeUInt32LE(blockCount, at + 24);
    out.writeUInt32LE(UF2_FAMILY_RP2040, at + 28);
    // A short last page is padded as erased flash, since the bootrom programs whole pages.
    out.fill(
      0xff,
      at + UF2_PAYLOAD_AT,
      at + UF2_PAYLOAD_AT + UF2_PAYLOAD_BYTES,
    );
    image.copy(
      out,
      at + UF2_PAYLOAD_AT,
      block * UF2_PAYLOAD_BYTES,
      (block + 1) * UF2_PAYLOAD_BYTES,
    );
    out.writeUInt32LE(UF2_MAGIC_END, at + UF2_MAGIC_END_AT);
  }
  return out;
}

/** The package slots in an ESP32 partition table's bytes, or null when the firmware on the board has none. */
export function slotsFromPartitionTable(table) {
  const slots = [null, null];
  for (
    let at = 0;
    at + ESP_PARTITION_ENTRY_BYTES <= table.length;
    at += ESP_PARTITION_ENTRY_BYTES
  ) {
    if (table.readUInt16LE(at) !== ESP_PARTITION_MAGIC) break;
    const labelEnd = at + ESP_PARTITION_LABEL_AT + ESP_PARTITION_LABEL_BYTES;
    const label = table
      .toString('latin1', at + ESP_PARTITION_LABEL_AT, labelEnd)
      .replace(/\0.*$/, '');
    const index = ESP_SLOT_LABELS.indexOf(label);
    if (index >= 0) {
      slots[index] = {
        address: table.readUInt32LE(at + 4),
        bytes: table.readUInt32LE(at + 8),
      };
    }
  }
  return slots.every(Boolean) ? slots : null;
}

/** Runs the first of a tool's names that exists; throws with its output when it fails. */
function runTool(names, args, {run, install, inherit = false}) {
  for (const name of names) {
    const result = run(name, args, inherit);
    if (result.error?.code === 'ENOENT') continue;
    if (result.error) throw result.error;
    if (result.status !== 0) {
      throw new Error(
        `${name} ${args.join(' ')} failed:\n${result.stderr ?? ''}${result.stdout ?? ''}`.trimEnd(),
      );
    }
    return;
  }
  throw new Error(`${names[0]} wasn't found. ${install}`);
}

const defaultRun = (command, args, inherit) =>
  spawnSync(command, args, {
    encoding: 'utf8',
    stdio: inherit ? 'inherit' : 'pipe',
  });

const espAccess = ({chip, port, run}) => {
  const names = process.env.ESPTOOL
    ? [process.env.ESPTOOL]
    : ['esptool', 'esptool.py'];
  const common = ['--chip', chip, ...(port ? ['--port', port] : [])];
  const options = {
    run,
    install: 'Export ESP-IDF first (. $IDF_PATH/export.sh), or set ESPTOOL.',
  };
  // The underscored commands: esptool 4 takes only these, and esptool 5 still does.
  const read = (address, bytes, file) =>
    runTool(
      names,
      [...common, 'read_flash', hex(address), hex(bytes), file],
      options,
    );
  return {
    read,
    slots: dir => {
      const file = join(dir, 'partitions.bin');
      read(ESP_PARTITION_TABLE_AT, ESP_PARTITION_TABLE_BYTES, file);
      const slots = slotsFromPartitionTable(readFileSync(file));
      if (!slots) {
        throw new Error(
          "The board's partition table has no erb_a and erb_b partitions. Flash this example's Flow B firmware first.",
        );
      }
      return slots;
    },
    write: (address, file) =>
      runTool(names, [...common, 'write_flash', hex(address), file], {
        ...options,
        inherit: true,
      }),
    finish: () => {}, // esptool resets the board itself after each command
  };
};

const picoAccess = ({board, run}) => {
  const names = process.env.PICOTOOL ? [process.env.PICOTOOL] : ['picotool'];
  const options = {
    run,
    install:
      'Install it (a build of the Pico SDK fetches one, without USB unless libusb is found), or set PICOTOOL.',
  };
  return {
    // -f takes a running board into BOOTSEL through its USB serial, and back afterwards.
    read: (address, bytes, file) =>
      runTool(
        names,
        [
          'save',
          '-r',
          hex(address),
          hex(address + bytes),
          file,
          '-t',
          'bin',
          '-f',
        ],
        options,
      ),
    slots: () => board.slots,
    write: (address, file) =>
      runTool(names, ['load', file, '-t', 'bin', '-o', hex(address), '-f'], {
        ...options,
        inherit: true,
      }),
    finish: () => runTool(names, ['reboot', '-f'], options),
  };
};

/**
 * Writes a package's slot image to a file, for a line or a drag-and-drop. Nothing here knows what the board holds,
 * so it is for an empty board, or a slot and sequence you already know.
 *
 * @returns {{file: string, address: number, sequence: number, slot: string}}
 */
export function packSlot({
  packageBytes,
  board,
  boardName,
  slotName = 'a',
  sequence = 1,
  outDir,
}) {
  const slot = SLOT_NAMES.indexOf(slotName);
  if (slot < 0) throw new Error(`--slot takes a or b, got: ${slotName}`);
  checkPackage(packageBytes);
  const {address, bytes} = board.slots[slot];
  const image = slotImage(packageBytes, sequence);
  if (image.length > bytes) {
    throw new Error(
      `The package takes ${image.length} B with its header, and ${boardName}'s slots hold ${bytes} B.`,
    );
  }
  const isUf2 = board.tool === 'picotool';
  const file = join(outDir, `app.slot-${slotName}.${isUf2 ? 'uf2' : 'bin'}`);
  writeFileSync(file, isUf2 ? uf2(image, address) : image);
  return {file, address, sequence, slot: slotName};
}

/**
 * Writes a package to a board: reads its slots, and writes the one that isn't the newest.
 *
 * @returns {{address: number, sequence: number, slot: string, lastGood: string|null}}
 */
export function flashSlot({
  packageBytes,
  board,
  port,
  run = defaultRun,
  log = () => {},
}) {
  checkPackage(packageBytes);
  const access =
    board.tool === 'picotool'
      ? picoAccess({board, run})
      : espAccess({chip: board.chip, port, run});
  const dir = mkdtempSync(join(tmpdir(), 'er-flash-'));
  try {
    const slots = access.slots(dir);
    const sequences = slots.map((slot, index) => {
      const file = join(dir, `header-${index}.bin`);
      access.read(slot.address, SLOT_HEADER_BYTES, file);
      return readSlotSequence(readFileSync(file), slot.bytes);
    });
    const {slot, sequence, lastGood} = pickSlot(sequences);
    const {address, bytes} = slots[slot];
    const image = slotImage(packageBytes, sequence);
    if (image.length > bytes) {
      throw new Error(
        `The package takes ${image.length} B with its header, and this board's slots hold ${bytes} B.`,
      );
    }
    const file = join(dir, 'slot.bin');
    writeFileSync(file, image);
    log(
      `Writing slot ${SLOT_NAMES[slot]} at ${hex(address)}, sequence ${sequence} (${image.length} B)`,
    );
    access.write(address, file);
    access.finish();
    return {
      address,
      sequence,
      slot: SLOT_NAMES[slot],
      lastGood: lastGood === null ? null : SLOT_NAMES[lastGood],
    };
  } finally {
    rmSync(dir, {recursive: true, force: true});
  }
}
