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

// create-embedded-react — scaffold a fresh embedded-react app.
//
//   npm create embedded-react@latest my-app                       # minimal starter
//   npm create embedded-react@latest my-app -- --ts               # TypeScript starter
//   npm create embedded-react@latest my-app -- --template thermostat   # start from a demo
//   npm create embedded-react@latest -- --list                    # list available templates
//   npx create-embedded-react my-app [--ts] [--template <name>]
//
// Copies the chosen template into <my-app>: the built-in JS/TS starters ship in this package; the demo
// templates (thermostat, music-player, …) are staged from the repo's demos/ into templates/ at publish
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
import {dirname, resolve, join, relative, basename} from 'node:path';

const PKG_ROOT = dirname(fileURLToPath(import.meta.url));
const erVersion = JSON.parse(
  readFileSync(resolve(PKG_ROOT, 'package.json'), 'utf8'),
).version;

// The built-in starters ship inside this package. --ts is shorthand for the `starter-ts` template.
const BUILTINS = [
  {
    name: 'starter',
    lang: 'js',
    dir: resolve(PKG_ROOT, 'template'),
    description:
      'Minimal starter — a styled card with a pulsing logo and a counter (default).',
  },
  {
    name: 'starter-ts',
    lang: 'ts',
    dir: resolve(PKG_ROOT, 'template-typescript'),
    description: 'The minimal starter in TypeScript (same as --ts).',
  },
];

/**
 * Where the demo templates live. Published: templates/ (staged by prepack). Source checkout before that
 * ran: fall back to the repo's demos/ directly, so the scaffolder works straight from a clone.
 */
function demoTemplatesRoot() {
  const staged = resolve(PKG_ROOT, 'templates');
  if (existsSync(staged)) return staged;
  const repoDemos = resolve(PKG_ROOT, '..', 'demos');
  if (existsSync(repoDemos)) return repoDemos;
  return null;
}

/** Discover the demo templates (any <root>/<name>/index.jsx), reading each package.json for its blurb. */
function demoTemplates() {
  const root = demoTemplatesRoot();
  if (!root) return [];
  return readdirSync(root, {withFileTypes: true})
    .filter(d => d.isDirectory() && existsSync(join(root, d.name, 'index.jsx')))
    .map(d => {
      let description = '';
      try {
        description =
          JSON.parse(readFileSync(join(root, d.name, 'package.json'), 'utf8'))
            .description || '';
      } catch {
        /* a demo without a package.json still scaffolds; just no blurb */
      }
      return {name: d.name, lang: 'js', dir: join(root, d.name), description};
    })
    .sort((a, b) => a.name.localeCompare(b.name));
}

/** The full template registry: built-in starters + discovered demos, in listing order. */
function allTemplates() {
  return [...BUILTINS, ...demoTemplates()];
}

function printTemplates() {
  console.log('Available templates (--template <name>):\n');
  const rows = allTemplates();
  const w = Math.max(...rows.map(t => t.name.length));
  for (const t of rows) {
    console.log(`  ${t.name.padEnd(w)}  ${t.description}`);
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
  console.log('  --list                  list available templates and exit');
  console.log('  -h, --help              show this help\n');
}

// --- Argument parsing ------------------------------------------------------------------------------------
// Separate flags from the positional <project-name>. --template/-t takes the next token as its value.
const argv = process.argv.slice(2);
const KNOWN_VALUE_FLAGS = new Set(['--template', '-t']);

let templateArg = null;
let wantsTs = false;
let wantsList = false;
let wantsHelp = false;
const positional = [];
const unknown = [];

for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (KNOWN_VALUE_FLAGS.has(a)) {
    const value = argv[i + 1];
    if (!value || value.startsWith('-')) {
      console.error(`Missing value for ${a}\n`);
      printHelp();
      process.exit(1);
    }
    templateArg = value;
    i++; // consume the value
  } else if (a === '--ts' || a === '--typescript') {
    wantsTs = true;
  } else if (a === '--list') {
    wantsList = true;
  } else if (a === '-h' || a === '--help') {
    wantsHelp = true;
  } else if (a.startsWith('-')) {
    unknown.push(a);
  } else {
    positional.push(a);
  }
}

if (wantsHelp) {
  printHelp();
  printTemplates();
  process.exit(0);
}
if (wantsList) {
  printTemplates();
  process.exit(0);
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
const template = templates.find(t => t.name === templateName);
if (!template) {
  console.error(`Unknown template: ${templateName}\n`);
  printTemplates();
  process.exit(1);
}

const arg = positional[0];
if (!arg) {
  printHelp();
  printTemplates();
  process.exit(1);
}

const destDir = resolve(process.cwd(), arg);
const appName = basename(destDir);
const pkgName =
  appName
    .toLowerCase()
    .replace(/[^a-z0-9-~.]/g, '-')
    .replace(/^[-.]+/, '') || 'embedded-react-app';

if (existsSync(destDir) && readdirSync(destDir).length) {
  console.error(`Target directory "${arg}" already exists and is not empty.`);
  process.exit(1);
}

const TEXT = /\.(jsx?|tsx?|json|md)$/;
const subst = s =>
  s
    .replaceAll('__APP_NAME__', pkgName)
    .replaceAll('__ER_VERSION__', `^${erVersion}`);

/** Copy template → dest, substituting placeholders in text files and restoring the dotfile name. */
function copyDir(src, dst) {
  mkdirSync(dst, {recursive: true});
  for (const name of readdirSync(src)) {
    const sp = join(src, name);
    // npm strips a published .gitignore, so templates ship it as `gitignore`; restore the dot here.
    const dp = join(dst, name === 'gitignore' ? '.gitignore' : name);
    if (statSync(sp).isDirectory()) copyDir(sp, dp);
    else if (TEXT.test(name) || name === 'gitignore')
      writeFileSync(dp, subst(readFileSync(sp, 'utf8')));
    else copyFileSync(sp, dp); // binary asset (png, ttf, …)
  }
}

/**
 * Normalize the scaffolded package.json: adopt the project name, reset the version, mark it private, and
 * pin embedded-react to this scaffolder's version (lockstep). This is what makes a demo — a real project
 * with its own name and a concrete dependency version — into a fresh <my-app> starter.
 */
function finalizePackageJson(dir) {
  const p = join(dir, 'package.json');
  if (!existsSync(p)) return;
  const pkg = JSON.parse(readFileSync(p, 'utf8'));
  pkg.name = pkgName;
  pkg.private = true;
  pkg.version = '0.0.0';
  if (pkg.dependencies?.['embedded-react'])
    pkg.dependencies['embedded-react'] = `^${erVersion}`;
  writeFileSync(p, JSON.stringify(pkg, null, 2) + '\n');
}

copyDir(template.dir, destDir);
finalizePackageJson(destDir);

const where = relative(process.cwd(), destDir) || '.';
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
