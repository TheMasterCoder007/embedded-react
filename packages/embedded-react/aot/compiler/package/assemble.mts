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
 * The package output's file assembly: the program section from the IR, the container around it, and the
 * resource report the build prints.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {
  SECTION_ASSET_PACK,
  SECTION_DEBUG,
  SECTION_PROGRAM,
  emitPackageContainer,
} from '../../../assets/emit-container.mjs';
import {lowerApp} from '../lower.mts';
import {FLAG_DEBUG, Resource, Table} from './format.mts';
import {lowerPackage} from './lower.mts';
import {writeDebug, writeProgram} from './writer.mts';
import type {ResolvedOptions} from '../options.mts';
import type {IrSlotStore} from '../ir/source.mts';
import type {SlotWidths} from '../ir/types.mts';
import type {AssetImport} from '../types.mts';
import type {Program, ProgramPanel, TableSize} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One panel's program, and the images it shows. */
export interface PanelProgram {
  program: Program;
  images: AssetImport[];
}

/** What one package compile produces before the assets are baked. */
export interface PackageResult {
  /** The program section. */
  program: Uint8Array;
  buildId: number;
  /** The debug section: in the package, or beside a release one as app.erdbg. */
  debug: Uint8Array;
  /** A release build: the debug flag clear, and no debug section in the package. */
  release: boolean;
  panels: ProgramPanel[];
  /** Each table's size in the program section. */
  tables: TableSize[];
  /** The image imports the package's asset pack has to hold. */
  images: AssetImport[];
}

/** A finished package. */
export interface PackageFile {
  bytes: Uint8Array;
  /** Each section's type and size. */
  sections: {type: number; bytes: number}[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

const SECTION_NAMES: Record<number, string> = {
  [SECTION_ASSET_PACK]: 'assets',
  [SECTION_PROGRAM]: 'program',
  [SECTION_DEBUG]: 'debug',
};

const TABLE_NAMES = Object.fromEntries(
  Object.entries(Table).map(([name, id]) => [id, name]),
);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Compiles a Flow B app's source to one panel's program. Pure, like compileSourceImpl; compilePackage
 * (compile.mts) is the public entry around it.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param opts  compilePackage's options, resolved (see resolveOptions), with the panel as the screen.
 * @param wide  State/ref slots to declare 64-bit.
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The panel's program, and its images.
 */
export function compilePanel(
  src: string,
  opts: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): PanelProgram {
  const app = lowerApp(src, opts, wide, stores);
  return {program: lowerPackage(app, opts), images: app.images};
}

/**
 * Writes the program section for every panel, and its debug section. The asset pack holds each image any
 * panel shows, so each panel needs room to register all of them.
 *
 * @param panels  Each panel's program, in the order the package lists them.
 * @param file  The source file, as the debug table names it.
 * @param release  Build for release: the debug flag clear.
 *
 * @returns The program section, and what the package holds.
 */
export function assemblePackage(
  panels: PanelProgram[],
  file: string,
  release = false,
): PackageResult {
  const images = [
    ...new Map(
      panels.flatMap(panel => panel.images).map(image => [image.name, image]),
    ).values(),
  ];
  const programs = panels.map(({program}) => ({
    ...program,
    flags: release ? program.flags & ~FLAG_DEBUG : program.flags,
    needs: program.needs.map(need =>
      need.resource === Resource.images
        ? {...need, amount: images.length}
        : need,
    ),
  }));
  const written = writeProgram(programs);
  return {
    program: written.bytes,
    buildId: written.buildId,
    debug: writeDebug(programs, written.buildId, file),
    release,
    panels: programs.map(program => program.panels[0]),
    tables: written.tables,
    images,
  };
}

/**
 * Puts a program section, its asset pack and, unless it is a release build, its debug section in a container.
 *
 * @param result  The compiled program.
 * @param assetPack  The baked asset pack, or nothing when the app has no assets.
 *
 * @returns The package.
 */
export function packPackage(
  result: PackageResult,
  assetPack?: Uint8Array,
): PackageFile {
  const {bytes, sections} = emitPackageContainer({
    program: result.program,
    assetPack,
    debug: result.release ? undefined : result.debug,
  });
  return {
    bytes,
    sections: sections.map(section => ({
      type: section.type,
      bytes: section.bytes,
    })),
  };
}

/**
 * The resource report: what each panel needs of the firmware's RAM and nodes, and the flash each section takes.
 *
 * @param result  The compiled program.
 * @param file  The finished package.
 *
 * @returns The report's lines.
 */
export function resourceReport(
  result: PackageResult,
  file: PackageFile,
): string[] {
  const panels = result.panels.map(
    panel =>
      `  panel ${panel.width}x${panel.height}: arena ${panel.arenaBytes} B, ${panel.nodeCount} nodes`,
  );
  const sections = file.sections.map(
    section =>
      `${SECTION_NAMES[section.type] ?? `section ${section.type}`} ${section.bytes} B`,
  );
  // A table stored once per panel lists each copy's size.
  const tables = result.tables
    .filter(table => table.bytes.some(bytes => bytes > 0))
    .map(table => `${TABLE_NAMES[table.id]} ${table.bytes.join('+')}`);
  return [
    ...panels,
    `  flash: ${file.bytes.length} B (${sections.join(', ')})`,
    `  program tables (B): ${tables.join(', ')}`,
  ];
}

/**
 * A package as C, for a board with no spare partition to link into its firmware: a const array of words, so it
 * is 4-aligned with any compiler, and its length in bytes.
 *
 * @param bytes  The package.
 *
 * @returns app.erpkg.c's text.
 */
export function bakePackage(bytes: Uint8Array): string {
  const padded = new Uint8Array(Math.ceil(bytes.length / 4) * 4);
  padded.set(bytes);
  const view = new DataView(padded.buffer);
  const words = Array.from(
    {length: padded.length / 4},
    (_, index) =>
      `0x${view
        .getUint32(index * 4, true)
        .toString(16)
        .padStart(8, '0')}u`,
  );
  const lines: string[] = [];
  for (let index = 0; index < words.length; index += 8) {
    lines.push(`    ${words.slice(index, index + 8).join(', ')},`);
  }
  return [
    '/* Generated by `embedded-react build --aot --bake`. Do not edit. */',
    '',
    '/*',
    ' * A Flow B package, linked into the firmware. Load it with',
    ' *   extern const uint32_t app_erpkg[];',
    ' *   extern const size_t app_erpkg_bytes;',
    ' *   erb_load(app_erpkg, app_erpkg_bytes, &config, &error);',
    ' */',
    '',
    '#include <stddef.h>',
    '#include <stdint.h>',
    '',
    `const uint32_t app_erpkg[${words.length}] = {`,
    ...lines,
    '};',
    '',
    `const size_t app_erpkg_bytes = ${bytes.length}u;`,
    '',
  ].join('\n');
}
