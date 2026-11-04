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
// A slot holds a header in an erase sector of its own, then the package. The firmware
// (runtime/erb_package_slots.c) runs the valid slot with the newest sequence and falls back to the other, so a
// new package always goes in the slot that isn't the newest, with the next sequence. `flash` reads the board's
// headers and writes the package, then the header, so its own write cut short never leaves the newest sequence on
// a partial package. `pack` writes the image to a file instead, for a factory line or a drag-and-drop UF2, which
// can't promise that order, so `flash` also checks the newest slot's package before it writes over the other.

import {spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {crc32} from './assets/emit-container.mjs';

export const SLOT_HEADER_BYTES = 4096; // bytes before the package: one erase sector
const SLOT_HEADER_PAGE_BYTES = 256; // the part of it that is written
const SEQUENCE_MAX = 0xffffffff;
const ERASED_BYTE = 0xff;
const SLOT_MAGIC = 0x53425245; // "ERBS", little-endian
const SLOT_MAGIC_AT = 0;
const SLOT_SEQUENCE_AT = 4;
const SLOT_PACKAGE_LENGTH_AT = 8;
const SLOT_HEADER_CHECKED_BYTES = 12;
const SLOT_HEADER_CRC_BYTES = 4;
const SLOT_NAMES = ['a', 'b'];

const CONTAINER_MAGIC = 'ERCF';
const CONTAINER_MAGIC_BYTES = 4;
const CONTAINER_CRC_AT = 8;
const CONTAINER_CRC_END = 12;
const CONTAINER_TAG_AT = 12;
const CONTAINER_TAG_LENGTH_BYTES = 2;
const PACKAGE_TAG = 'erb';

const ESP_PARTITION_TABLE_AT = 0x8000;
const ESP_PARTITION_TABLE_BYTES = 0xc00;
const ESP_PARTITION_ENTRY_BYTES = 32;
const ESP_PARTITION_MAGIC = 0x50aa;
const ESP_PARTITION_OFFSET_AT = 4;
const ESP_PARTITION_SIZE_AT = 8;
const ESP_PARTITION_LABEL_AT = 12;
const ESP_PARTITION_LABEL_BYTES = 16;
const ESP_SLOT_LABELS = ['erb_a', 'erb_b'];

const PICO_REBOOT_TIMEOUT_MS = 10000;
const PICO_READY_TRIES = 40;
const PICO_READY_WAIT_MS = 250;

const UF2_BLOCK_BYTES = 512;
const UF2_PAYLOAD_BYTES = 256;
const UF2_MAGIC_START_0 = 0x0a324655;
const UF2_MAGIC_START_1 = 0x9e5d5157;
const UF2_MAGIC_END = 0x0ab16f30;
const UF2_FLAG_FAMILY = 0x2000;
const UF2_FAMILY_RP2040 = 0xe48bff56;
const UF2_MAGIC_START_1_AT = 4;
const UF2_FLAGS_AT = 8;
const UF2_ADDRESS_AT = 12;
const UF2_PAYLOAD_SIZE_AT = 16;
const UF2_BLOCK_NUMBER_AT = 20;
const UF2_BLOCK_COUNT_AT = 24;
const UF2_FAMILY_AT = 28;
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
    bytes.length < CONTAINER_TAG_AT + CONTAINER_TAG_LENGTH_BYTES ||
    bytes.toString('latin1', 0, CONTAINER_MAGIC_BYTES) !== CONTAINER_MAGIC
  ) {
    throw new Error('Not a package: no ERCF container in this file.');
  }
  if (
    bytes.readUInt32LE(CONTAINER_CRC_AT) !==
    crc32(bytes.subarray(CONTAINER_CRC_END))
  ) {
    throw new Error('This package is corrupt: its CRC does not match.');
  }
  const tagBytes = bytes.readUInt16LE(CONTAINER_TAG_AT);
  const tag = bytes.toString(
    'latin1',
    CONTAINER_TAG_AT + CONTAINER_TAG_LENGTH_BYTES,
    CONTAINER_TAG_AT + CONTAINER_TAG_LENGTH_BYTES + tagBytes,
  );
  if (tag !== PACKAGE_TAG) {
    throw new Error(
      `This is a Flow A container (built for ${tag}), not a Flow B package. Build one with: embedded-react build --aot`,
    );
  }
}

/** Whether the bytes are a whole Flow B package. */
function isWholePackage(bytes) {
  try {
    checkPackage(bytes);
    return true;
  } catch {
    return false;
  }
}

/** A slot's header page. */
export function slotHeader(packageLength, sequence) {
  const header = Buffer.alloc(SLOT_HEADER_PAGE_BYTES, ERASED_BYTE);
  header.writeUInt32LE(SLOT_MAGIC, SLOT_MAGIC_AT);
  header.writeUInt32LE(sequence, SLOT_SEQUENCE_AT);
  header.writeUInt32LE(packageLength, SLOT_PACKAGE_LENGTH_AT);
  header.writeUInt32LE(
    crc32(header.subarray(0, SLOT_HEADER_CHECKED_BYTES)),
    SLOT_HEADER_CHECKED_BYTES,
  );
  return header;
}

/** A slot's whole image: the header in its sector, then the package. */
export function slotImage(packageBytes, sequence) {
  const headerSector = Buffer.alloc(SLOT_HEADER_BYTES, ERASED_BYTE);
  slotHeader(packageBytes.length, sequence).copy(headerSector);
  return Buffer.concat([headerSector, packageBytes]);
}

/** What a slot's first bytes say: its sequence and package length, or null when it has no valid header. */
export function readSlotHeader(bytes, slotBytes) {
  if (
    bytes.length < SLOT_HEADER_CHECKED_BYTES + SLOT_HEADER_CRC_BYTES ||
    bytes.readUInt32LE(SLOT_MAGIC_AT) !== SLOT_MAGIC ||
    bytes.readUInt32LE(SLOT_HEADER_CHECKED_BYTES) !==
      crc32(bytes.subarray(0, SLOT_HEADER_CHECKED_BYTES))
  ) {
    return null;
  }
  const packageBytes = bytes.readUInt32LE(SLOT_PACKAGE_LENGTH_AT);
  if (packageBytes === 0 || packageBytes > slotBytes - SLOT_HEADER_BYTES) {
    return null;
  }
  return {sequence: bytes.readUInt32LE(SLOT_SEQUENCE_AT), packageBytes};
}

/** A slot's sequence from its first bytes, or null when it has no valid header. */
export function readSlotSequence(bytes, slotBytes) {
  return readSlotHeader(bytes, slotBytes)?.sequence ?? null;
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

/**
 * Runs of bytes as UF2 blocks, in the order given, for a drag-and-drop onto an RP2040 in BOOTSEL mode.
 *
 * @param {{address: number, bytes: Buffer}[]} runs  What to write where.
 */
export function uf2(runs) {
  const pages = runs.flatMap(({address, bytes}) =>
    Array.from(
      {length: Math.ceil(bytes.length / UF2_PAYLOAD_BYTES)},
      (_, page) => ({
        address: address + page * UF2_PAYLOAD_BYTES,
        bytes: bytes.subarray(
          page * UF2_PAYLOAD_BYTES,
          (page + 1) * UF2_PAYLOAD_BYTES,
        ),
      }),
    ),
  );
  const blockCount = pages.length;
  const blocks = Buffer.alloc(blockCount * UF2_BLOCK_BYTES);
  for (let block = 0; block < blockCount; block++) {
    const at = block * UF2_BLOCK_BYTES;
    blocks.writeUInt32LE(UF2_MAGIC_START_0, at);
    blocks.writeUInt32LE(UF2_MAGIC_START_1, at + UF2_MAGIC_START_1_AT);
    blocks.writeUInt32LE(UF2_FLAG_FAMILY, at + UF2_FLAGS_AT);
    blocks.writeUInt32LE(pages[block].address, at + UF2_ADDRESS_AT);
    blocks.writeUInt32LE(UF2_PAYLOAD_BYTES, at + UF2_PAYLOAD_SIZE_AT);
    blocks.writeUInt32LE(block, at + UF2_BLOCK_NUMBER_AT);
    blocks.writeUInt32LE(blockCount, at + UF2_BLOCK_COUNT_AT);
    blocks.writeUInt32LE(UF2_FAMILY_RP2040, at + UF2_FAMILY_AT);
    // A short last page is padded as erased flash, since the bootrom programs whole pages.
    blocks.fill(
      ERASED_BYTE,
      at + UF2_PAYLOAD_AT,
      at + UF2_PAYLOAD_AT + UF2_PAYLOAD_BYTES,
    );
    pages[block].bytes.copy(blocks, at + UF2_PAYLOAD_AT);
    blocks.writeUInt32LE(UF2_MAGIC_END, at + UF2_MAGIC_END_AT);
  }
  return blocks;
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
        address: table.readUInt32LE(at + ESP_PARTITION_OFFSET_AT),
        bytes: table.readUInt32LE(at + ESP_PARTITION_SIZE_AT),
      };
    }
  }
  return slots.every(Boolean) ? slots : null;
}

/** Runs the first of a tool's names that exists; throws with its output when it fails. */
function runTool(names, args, {run, install, inherit = false, timeoutMs}) {
  for (const name of names) {
    const result = run(name, args, inherit, timeoutMs);
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

const sleepMs = ms =>
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

const defaultRun = (command, args, inherit, timeoutMs) =>
  spawnSync(command, args, {
    encoding: 'utf8',
    stdio: inherit ? 'inherit' : 'pipe',
    timeout: timeoutMs,
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
    begin: () => {},
    finish: () => {}, // esptool resets the board itself after each command
  };
};

const picoAccess = ({board, run, sleep}) => {
  const names = process.env.PICOTOOL ? [process.env.PICOTOOL] : ['picotool'];
  const options = {
    run,
    install:
      "Install one with USB support, such as Homebrew's, or set PICOTOOL. The copy a Pico SDK build fetches has none.",
  };
  // libusb on macOS can deadlock as the board detaches, leaving picotool stuck after the reboot worked. What comes
  // next shows whether it did, so a reboot that outlives the timeout isn't a failure.
  const reboot = args => {
    try {
      runTool(names, args, {...options, timeoutMs: PICO_REBOOT_TIMEOUT_MS});
    } catch (error) {
      if (error.code !== 'ETIMEDOUT') throw error;
    }
  };
  return {
    // The board stays in BOOTSEL for the whole write: a command straight after a -f one finds it still rebooting.
    // -f takes a running board there through its USB serial, and a board already there stays.
    begin: () => {
      reboot(['reboot', '-u', '-f']);
      for (let tries = 1; ; tries++) {
        try {
          runTool(names, ['info'], {
            ...options,
            timeoutMs: PICO_REBOOT_TIMEOUT_MS,
          });
          return;
        } catch (error) {
          if (tries === PICO_READY_TRIES) throw error;
          sleep(PICO_READY_WAIT_MS);
        }
      }
    },
    // Addresses are memory addresses for both commands: load's -o defaults to 0x10000000, the start of flash.
    read: (address, bytes, file) =>
      runTool(
        names,
        ['save', '-r', hex(address), hex(address + bytes), file, '-t', 'bin'],
        options,
      ),
    slots: () => board.slots,
    write: (address, file) =>
      runTool(names, ['load', file, '-t', 'bin', '-o', hex(address)], {
        ...options,
        inherit: true,
      }),
    finish: () => reboot(['reboot']),
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
  // 0 is valid: it is what comes after the sequence wraps.
  if (!Number.isInteger(sequence) || sequence < 0 || sequence > SEQUENCE_MAX) {
    throw new Error(
      `--sequence takes a whole number from 0 to ${SEQUENCE_MAX}, got: ${sequence}`,
    );
  }
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
  writeFileSync(
    file,
    isUf2
      ? uf2([
          {address: address + SLOT_HEADER_BYTES, bytes: packageBytes},
          {address, bytes: slotHeader(packageBytes.length, sequence)},
        ])
      : image,
  );
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
  sleep = sleepMs,
}) {
  checkPackage(packageBytes);
  const access =
    board.tool === 'picotool'
      ? picoAccess({board, run, sleep})
      : espAccess({chip: board.chip, port, run});
  const scratchDir = mkdtempSync(join(tmpdir(), 'er-flash-'));
  let begun = false;
  try {
    access.begin();
    begun = true;
    const slots = access.slots(scratchDir);
    const headers = slots.map((slot, index) => {
      const file = join(scratchDir, `header-${index}.bin`);
      access.read(slot.address, SLOT_HEADER_PAGE_BYTES, file);
      return readSlotHeader(readFileSync(file), slot.bytes);
    });
    let {slot, sequence, lastGood} = pickSlot(
      headers.map(header => header?.sequence ?? null),
    );
    // The newest slot is what this write leaves as the fallback, so it has to be whole. A write cut short can leave
    // a header over part of a package, and no boot may have cleared it yet.
    if (lastGood !== null) {
      const file = join(scratchDir, 'newest.bin');
      access.read(
        slots[lastGood].address + SLOT_HEADER_BYTES,
        headers[lastGood].packageBytes,
        file,
      );
      if (!isWholePackage(readFileSync(file))) {
        log(
          `Slot ${SLOT_NAMES[lastGood]} holds a partial package, so it goes first`,
        );
        const other = 1 - lastGood;
        slot = lastGood;
        lastGood = headers[other] ? other : null;
      }
    }
    const {address, bytes} = slots[slot];
    const slotBytesUsed = SLOT_HEADER_BYTES + packageBytes.length;
    if (slotBytesUsed > bytes) {
      throw new Error(
        `The package takes ${slotBytesUsed} B with its header, and this board's slots hold ${bytes} B.`,
      );
    }
    const packageFile = join(scratchDir, 'package.bin');
    const headerFile = join(scratchDir, 'header.bin');
    writeFileSync(packageFile, packageBytes);
    writeFileSync(headerFile, slotHeader(packageBytes.length, sequence));
    log(
      `Writing slot ${SLOT_NAMES[slot]} at ${hex(address)}, sequence ${sequence} (${slotBytesUsed} B)`,
    );
    // The header last. Until then the slot still has its older sequence, or none, so it stays the one to write
    // over if this is cut short, and the other slot stays the newest.
    access.write(address + SLOT_HEADER_BYTES, packageFile);
    access.write(address, headerFile);
    begun = false;
    access.finish();
    return {
      address,
      sequence,
      slot: SLOT_NAMES[slot],
      lastGood: lastGood === null ? null : SLOT_NAMES[lastGood],
    };
  } catch (error) {
    // Out of BOOTSEL if it can, without hiding what went wrong.
    if (begun) {
      try {
        access.finish();
      } catch {}
    }
    throw error;
  } finally {
    rmSync(scratchDir, {recursive: true, force: true});
  }
}
