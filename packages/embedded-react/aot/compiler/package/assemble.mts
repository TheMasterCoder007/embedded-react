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
  SECTION_PROGRAM,
  emitPackageContainer,
} from '../../../assets/emit-container.mjs';
import {lowerApp} from '../lower.mts';
import {Table} from './format.mts';
import {lowerPackage} from './lower.mts';
import {writeProgram} from './writer.mts';
import type {ResolvedOptions} from '../options.mts';
import type {IrSlotStore} from '../ir/source.mts';
import type {SlotWidths} from '../ir/types.mts';
import type {AssetImport} from '../types.mts';
import type {ProgramPanel, TableSize} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** What one package compile produces before the assets are baked. */
export interface PackageResult {
  /** The program section. */
  program: Uint8Array;
  buildId: number;
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
};

const TABLE_NAMES = Object.fromEntries(
  Object.entries(Table).map(([name, id]) => [id, name]),
);

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Compiles a Flow B app's source to a package's program section. Pure, like compileSourceImpl; compilePackage
 * (compile.mts) is the public entry around it.
 *
 * @param src  The App.jsx/App.tsx source text.
 * @param opts  compilePackage's options, resolved (see resolveOptions).
 * @param wide  State/ref slots to declare 64-bit.
 * @param stores  Collects every value stored into an int slot.
 *
 * @returns The program section, and what the package holds.
 */
export function compilePackageImpl(
  src: string,
  opts: ResolvedOptions,
  wide: SlotWidths,
  stores: IrSlotStore[],
): PackageResult {
  const app = lowerApp(src, opts, wide, stores);
  const program = lowerPackage(app, opts);
  const written = writeProgram(program);
  return {
    program: written.bytes,
    buildId: written.buildId,
    panels: program.panels,
    tables: written.tables,
    images: app.images,
  };
}

/**
 * Puts a program section and its asset pack in a container.
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
  const tables = result.tables
    .filter(table => table.bytes > 0)
    .map(table => `${TABLE_NAMES[table.id]} ${table.bytes}`);
  return [
    ...panels,
    `  flash: ${file.bytes.length} B (${sections.join(', ')})`,
    `  program tables (B): ${tables.join(', ')}`,
  ];
}
