#!/usr/bin/env node
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

// @ts-check

// create-embedded-react — scaffold a fresh embedded-react app.
//
//   npm create embedded-react@latest my-app                       # minimal starter
//   npm create embedded-react@latest my-app -- --ts               # TypeScript starter
//   npm create embedded-react@latest my-app -- --template thermostat   # start from a demo
//   npm create embedded-react@latest -- --list                    # list available templates
//   npm create embedded-react@latest my-app -- --board esp32-s3   # with VS Code's files to debug it on a board
//   npx create-embedded-react my-app [--ts] [--template <name>]
//
// Copies the chosen template into <my-app>: the built-in JS/TS starters ship in this package; the demo
// templates (thermostat, watch-face, …) are staged from the repo's demos/ into templates/ at publish
// time (see sync-templates.mjs). The scaffolder sets the project name, pins the embedded-react dependency
// to this scaffolder's version (lockstep), and restores the .gitignore. Then: cd my-app && npm install
// && npm run dev.

import {
  readdirSync,
  statSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
  copyFileSync,
  existsSync,
} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {
  dirname,
  resolve,
  join,
  relative,
  basename,
  isAbsolute,
} from 'node:path';

const PACKAGE_ROOT = dirname(fileURLToPath(import.meta.url));
const embeddedReactVersion = JSON.parse(
  readFileSync(resolve(PACKAGE_ROOT, 'package.json'), 'utf8'),
).version;

// The built-in starters ship inside this package. --ts is shorthand for the `starter-ts` template.
const BUILTINS = [
  {
    name: 'starter',
    language: 'js',
    dir: resolve(PACKAGE_ROOT, 'template'),
    description:
      'The responsive starter — a card with a pulsing logo and a counter (default).',
  },
  {
    name: 'starter-ts',
    language: 'ts',
    dir: resolve(PACKAGE_ROOT, 'template-typescript'),
    description: 'The minimal starter in TypeScript (same as --ts).',
  },
];

const OPENOCD_READY = 'Listening on port 3333 for gdb connections';

/**
 * The boards `--board` writes VS Code's files for. `flash` is the board's name for `embedded-react flash`, where it
 * writes that board's packages. `debug` is what its probe needs: the GDB, the debug server, and where the board
 * example leaves the firmware's ELF. Keep these in step with the examples' own .vscode/launch.json.
 *
 * @type {Record<string, {description: string, flash: string | null, debug: {name: string, gdb: string, gdbOnMac?: string, server: string[], elf: string} | null}>}
 */
const BOARDS = {
  'esp32-s3': {
    description: 'ESP32-S3, debugged over its built-in JTAG',
    flash: 'esp32-s3',
    debug: {
      name: 'ESP32-S3',
      gdb: 'xtensa-esp32s3-elf-gdb',
      // The reset lets the server read the flash mapping, whatever session came before.
      server: [
        'openocd',
        '-f',
        'board/esp32s3-builtin.cfg',
        '-c',
        'init; reset halt',
      ],
      elf: 'build-flow-b-debug/embedded-react-esp32.elf',
    },
  },
  'esp32-2432s028r': {
    description:
      'ESP32 "Cheap Yellow Display": build and flash only, it has no probe',
    flash: 'esp32-2432s028r',
    debug: null,
  },
  'rp2040-touch-lcd-1.69': {
    description: 'RP2040, debugged through a CMSIS-DAP probe',
    flash: 'rp2040-touch-lcd-1.69',
    debug: {
      name: 'RP2040',
      gdb: 'gdb-multiarch',
      gdbOnMac: 'gdb',
      server: [
        'openocd',
        '-f',
        'interface/cmsis-dap.cfg',
        '-f',
        'target/rp2040.cfg',
        '-c',
        'adapter speed 5000',
      ],
      elf: 'build-debug/embedded-react-watch.elf',
    },
  },
  stm32f7: {
    description: 'STM32F7, debugged through an ST-Link',
    flash: null,
    debug: stLink('STM32F7', ['-f', 'target/stm32f7x.cfg']),
  },
  stm32h7: {
    description: 'STM32H7, debugged through an ST-Link',
    flash: null,
    debug: stLink('STM32H7', ['-f', 'target/stm32h7x.cfg']),
  },
  'stm32h7-dual-bank': {
    description: 'STM32H7 with two flash banks, debugged through an ST-Link',
    flash: null,
    // Without both banks in the server's memory map, a breakpoint in the second never fires.
    debug: stLink('STM32H7', [
      '-c',
      'set DUAL_BANK 1',
      '-f',
      'target/stm32h7x.cfg',
    ]),
  },
};

/**
 * What an STM32 behind an ST-Link needs.
 *
 * @param {string} name
 * @param {string[]} target
 */
function stLink(name, target) {
  return {
    name,
    gdb: 'gdb-multiarch',
    gdbOnMac: 'gdb',
    server: ['openocd', '-f', 'interface/stlink.cfg', ...target],
    elf: 'build/firmware.elf',
  };
}

/**
 * VS Code's launch configurations and tasks for an app on a board: F5 builds the app, flashes it when it changed,
 * and attaches through the board's probe.
 *
 * @param {string} boardName
 * @param {string | null} firmware  The firmware's ELF from the project's folder, or null for where the board
 *   example leaves it.
 */
function editorFiles(boardName, firmware) {
  const board = BOARDS[boardName];
  /** @type {Record<string, unknown>[]} */
  const tasks = [
    {
      label: 'App: build',
      type: 'shell',
      command: 'npx',
      args: ['embedded-react', 'build', '--aot'],
      problemMatcher: [],
    },
  ];
  if (board.flash) {
    tasks.push(
      {
        label: 'App: flash',
        detail:
          "Writes the package when it isn't the one the last flash wrote.",
        type: 'shell',
        command: 'npx',
        args: ['embedded-react', 'flash', '--board', board.flash, '--changed'],
        problemMatcher: [],
      },
      {
        label: 'Build and flash',
        dependsOrder: 'sequence',
        dependsOn: ['App: build', 'App: flash'],
        problemMatcher: [],
      },
    );
  }
  /** @type {Record<string, unknown>[]} */
  const configurations = [
    {
      type: 'embedded-react',
      request: 'launch',
      name: 'Debug the app in the simulator',
      simulator: true,
      cwd: '${workspaceFolder}',
    },
  ];
  if (board.debug) {
    const {name, gdb, gdbOnMac, server, elf} = board.debug;
    configurations.push({
      type: 'embedded-react',
      request: 'attach',
      name: `Debug the app on the ${name}`,
      preLaunchTask: board.flash ? 'Build and flash' : 'App: build',
      program:
        firmware && isAbsolute(firmware)
          ? firmware
          : `\${workspaceFolder}/${firmware ?? elf}`,
      debugFile: '${workspaceFolder}/dist/app.erdbg',
      gdb,
      ...(gdbOnMac ? {osx: {gdb: gdbOnMac}} : {}),
      server,
      serverReady: OPENOCD_READY,
      target: 'localhost:3333',
      disconnectCommands: ['monitor resume'],
    });
  }
  return {
    'launch.json': {version: '0.2.0', configurations},
    'tasks.json': {version: '2.0.0', tasks},
  };
}

/**
 * Where the demo templates live. Published: templates/ (staged by prepack). Source checkout before that
 * ran: fall back to the repo's demos/ directly, so the scaffolder works straight from a clone.
 */
function demoTemplatesRoot() {
  const staged = resolve(PACKAGE_ROOT, 'templates');
  if (existsSync(staged)) return staged;
  const repoDemos = resolve(PACKAGE_ROOT, '..', 'demos');
  if (existsSync(repoDemos)) return repoDemos;
  return null;
}

/** Discover the demo templates (any <root>/<name>/index.jsx or index.tsx), reading each package.json for its blurb. */
function demoTemplates() {
  const root = demoTemplatesRoot();
  if (!root) return [];
  return readdirSync(root, {withFileTypes: true})
    .filter(
      entry =>
        entry.isDirectory() &&
        (existsSync(join(root, entry.name, 'index.jsx')) ||
          existsSync(join(root, entry.name, 'index.tsx'))),
    )
    .map(entry => {
      let description = '';
      try {
        description =
          JSON.parse(
            readFileSync(join(root, entry.name, 'package.json'), 'utf8'),
          ).description || '';
      } catch {
        /* a demo without a package.json still scaffolds; just no blurb */
      }
      return {
        name: entry.name,
        language: existsSync(join(root, entry.name, 'index.tsx')) ? 'ts' : 'js',
        dir: join(root, entry.name),
        description,
      };
    })
    .sort((first, second) => first.name.localeCompare(second.name));
}

/** The full template registry: built-in starters + discovered demos, in listing order. */
function allTemplates() {
  return [...BUILTINS, ...demoTemplates()];
}

function printTemplates() {
  console.log('Available templates (--template <name>):\n');
  const rows = allTemplates();
  const nameWidth = Math.max(...rows.map(template => template.name.length));
  for (const template of rows) {
    console.log(
      `  ${template.name.padEnd(nameWidth)}  ${template.description}`,
    );
  }
  console.log('');
}

function printBoards() {
  console.log('Boards (--board <name>):\n');
  const nameWidth = Math.max(...Object.keys(BOARDS).map(name => name.length));
  for (const [name, board] of Object.entries(BOARDS)) {
    console.log(`  ${name.padEnd(nameWidth)}  ${board.description}`);
  }
  console.log('');
}

function printHelp() {
  console.log('Create a new embedded-react app:\n');
  console.log('  npm create embedded-react@latest <project-name>');
  console.log('  npx create-embedded-react <project-name>\n');
  console.log('Options:');
  console.log(
    '  --template, -t <name>   start from a template or demo (default: starter)',
  );
  console.log(
    '  --ts, --typescript      scaffold the TypeScript starter (= --template starter-ts)',
  );
  console.log(
    "  --board, -b <name>      add VS Code's files to build, flash and debug the app on a board",
  );
  console.log(
    "  --firmware <elf>        the firmware's ELF for --board, when it isn't where the board example leaves it",
  );
  console.log('  --list                  list available templates and exit');
  console.log('  -h, --help              show this help\n');
}

// --- Argument parsing ------------------------------------------------------------------------------------
// Separate flags from the positional <project-name>. --template/-t takes the next token as its value.
const argv = process.argv.slice(2);
const KNOWN_VALUE_FLAGS = new Set([
  '--template',
  '-t',
  '--board',
  '-b',
  '--firmware',
]);

let templateArg = null;
let boardArg = null;
let firmwareArg = null;
let wantsTs = false;
let wantsList = false;
let wantsHelp = false;
const positional = [];
const unknown = [];

for (let argIndex = 0; argIndex < argv.length; argIndex++) {
  const arg = argv[argIndex];
  if (KNOWN_VALUE_FLAGS.has(arg)) {
    const value = argv[argIndex + 1];
    if (!value || value.startsWith('-')) {
      console.error(`Missing value for ${arg}\n`);
      printHelp();
      process.exit(1);
    }
    if (arg === '--board' || arg === '-b') boardArg = value;
    else if (arg === '--firmware') firmwareArg = value;
    else templateArg = value;
    argIndex++; // consume the value
  } else if (arg === '--ts' || arg === '--typescript') {
    wantsTs = true;
  } else if (arg === '--list') {
    wantsList = true;
  } else if (arg === '-h' || arg === '--help') {
    wantsHelp = true;
  } else if (arg.startsWith('-')) {
    unknown.push(arg);
  } else {
    positional.push(arg);
  }
}

if (wantsHelp) {
  printHelp();
  printTemplates();
  printBoards();
  process.exit(0);
}
if (wantsList) {
  printTemplates();
  printBoards();
  process.exit(0);
}
if (boardArg && !BOARDS[boardArg]) {
  console.error(`Unknown board: ${boardArg}\n`);
  printBoards();
  process.exit(1);
}
if (firmwareArg && !BOARDS[boardArg ?? '']?.debug) {
  console.error('--firmware goes with a --board that has a probe.\n');
  printBoards();
  process.exit(1);
}
if (unknown.length) {
  console.error(`Unknown option(s): ${unknown.join(', ')}\n`);
  printHelp();
  process.exit(1);
}
if (positional.length > 1) {
  console.error(
    `Expected a single <project-name>, got: ${positional.join(', ')}`,
  );
  process.exit(1);
}

// --- Resolve the chosen template ------------------------------------------------------------------------
// --ts is shorthand for `starter-ts`; it may only combine with the default/`starter` template (no demo has
// a TypeScript variant yet).
let templateName = templateArg;
if (wantsTs) {
  if (templateName == null || templateName === 'starter') {
    templateName = 'starter-ts';
  } else if (templateName !== 'starter-ts') {
    console.error(
      `--ts can't be combined with --template ${templateName} (no TypeScript variant). ` +
        'Use --template starter-ts, or drop --ts.',
    );
    process.exit(1);
  }
}
if (templateName == null) templateName = 'starter';

const templates = allTemplates();
const template = templates.find(candidate => candidate.name === templateName);
if (!template) {
  console.error(`Unknown template: ${templateName}\n`);
  printTemplates();
  process.exit(1);
}

const projectArg = positional[0];
if (!projectArg) {
  printHelp();
  printTemplates();
  process.exit(1);
}

const destinationDir = resolve(process.cwd(), projectArg);
const appName = basename(destinationDir);
const packageName =
  appName
    .toLowerCase()
    .replace(/[^a-z0-9-~.]/g, '-')
    .replace(/^[-.]+/, '') || 'embedded-react-app';

if (existsSync(destinationDir) && readdirSync(destinationDir).length) {
  console.error(
    `Target directory "${projectArg}" already exists and is not empty.`,
  );
  process.exit(1);
}

const TEXT_FILE = /\.(jsx?|tsx?|json|md)$/;
/** @param {string} text */
const subst = text =>
  text
    .replaceAll('__APP_NAME__', packageName)
    .replaceAll('__ER_VERSION__', `^${embeddedReactVersion}`);

/**
 * Copy template → dest, substituting placeholders in text files and restoring the dotfile name.
 *
 * @param {string} source
 * @param {string} destination
 */
function copyDir(source, destination) {
  mkdirSync(destination, {recursive: true});
  for (const name of readdirSync(source)) {
    const sourcePath = join(source, name);
    // npm strips a published .gitignore, so templates ship it as `gitignore`; restore the dot here.
    const destinationPath = join(
      destination,
      name === 'gitignore' ? '.gitignore' : name,
    );
    if (statSync(sourcePath).isDirectory())
      copyDir(sourcePath, destinationPath);
    else if (TEXT_FILE.test(name) || name === 'gitignore')
      writeFileSync(destinationPath, subst(readFileSync(sourcePath, 'utf8')));
    else copyFileSync(sourcePath, destinationPath); // binary asset (png, ttf, …)
  }
}

/**
 * Normalize the scaffolded package.json: adopt the project name, reset the version, mark it private, and
 * pin embedded-react to this scaffolder's version (lockstep). This is what makes a demo — a real project
 * with its own name and a concrete dependency version — into a fresh <my-app> starter.
 *
 * @param {string} dir
 */
function finalizePackageJson(dir) {
  const packageJsonPath = join(dir, 'package.json');
  if (!existsSync(packageJsonPath)) return;
  const packageJson = JSON.parse(readFileSync(packageJsonPath, 'utf8'));
  packageJson.name = packageName;
  packageJson.private = true;
  packageJson.version = '0.0.0';
  if (packageJson.dependencies?.['embedded-react'])
    packageJson.dependencies['embedded-react'] = `^${embeddedReactVersion}`;
  writeFileSync(packageJsonPath, JSON.stringify(packageJson, null, 2) + '\n');
}

/**
 * Copies in the packages the template depends on by a `file:` path outside its folder, and points the project at the
 * copies. A published template has them already (sync-templates.mjs); a demo scaffolded straight from a clone doesn't.
 *
 * @param {string} templateDirectory
 * @param {string} projectDirectory
 */
function copyLocalPackages(templateDirectory, projectDirectory) {
  const packageJsonPath = join(projectDirectory, 'package.json');
  if (!existsSync(packageJsonPath)) return;
  const packageJson = JSON.parse(readFileSync(packageJsonPath, 'utf8'));
  let copied = false;
  for (const [name, version] of Object.entries(
    packageJson.dependencies ?? {},
  )) {
    if (!version.startsWith('file:')) continue;
    const packageDirectory = resolve(templateDirectory, version.slice(5));
    if (!relative(templateDirectory, packageDirectory).startsWith('..'))
      continue;
    const folder = basename(packageDirectory);
    copyDir(packageDirectory, join(projectDirectory, folder));
    packageJson.dependencies[name] = `file:./${folder}`;
    copied = true;
  }
  if (copied)
    writeFileSync(packageJsonPath, JSON.stringify(packageJson, null, 2) + '\n');
}

copyDir(template.dir, destinationDir);
copyLocalPackages(template.dir, destinationDir);
finalizePackageJson(destinationDir);
if (boardArg) {
  const editorDirectory = join(destinationDir, '.vscode');
  mkdirSync(editorDirectory, {recursive: true});
  for (const [name, contents] of Object.entries(
    // A relative --firmware is a path as the shell takes it, so it is kept from the new project's folder.
    editorFiles(
      boardArg,
      firmwareArg && !isAbsolute(firmwareArg)
        ? relative(
            destinationDir,
            resolve(process.cwd(), firmwareArg),
          ).replaceAll('\\', '/')
        : firmwareArg,
    ),
  )) {
    writeFileSync(
      join(editorDirectory, name),
      JSON.stringify(contents, null, 2) + '\n',
    );
  }
}

const where = relative(process.cwd(), destinationDir) || '.';
console.log(
  `\n✓ Created "${appName}"${
    template.name === 'starter' ? '' : ` from the "${template.name}" template`
  } at ${where}\n`,
);
console.log('Next steps:');
if (where !== '.') console.log(`  cd ${where}`);
console.log('  npm install');
console.log(
  '  npm run dev         # WASM simulator with hot reload → http://localhost:3333',
);
console.log('  npm run dev:device  # or hot reload on a real board over USB');
console.log(
  '                      #   port auto-detected on ESP32-S3/C3; other boards: npm run dev:device -- <port>\n',
);
if (boardArg) {
  const debug = BOARDS[boardArg].debug;
  console.log(
    `VS Code: .vscode has the tasks to build ${BOARDS[boardArg].flash ? 'and flash ' : ''}the app for ${boardArg}` +
      (debug
        ? ', and F5 debugs it there or in the simulator.'
        : ', and F5 debugs it in the simulator.'),
  );
  if (debug && !firmwareArg) {
    console.log(
      `  It expects the firmware's ELF at ${debug.elf} in the project: set "program" in .vscode/launch.json to yours.`,
    );
  }
  console.log('');
}
