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
//   npm create embedded-react@latest my-app
//   npx create-embedded-react my-app
//
// Copies template/ into <my-app>, substituting the project name and pinning the embedded-react dependency
// to this scaffolder's version (lockstep). Then: cd my-app && npm install && npm run dev.

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

// Separate the project name from flags. --ts/--typescript selects the TypeScript template.
const argv = process.argv.slice(2);
const flags = argv.filter(a => a.startsWith('-'));
const positional = argv.filter(a => !a.startsWith('-'));
const wantsTs = flags.some(a => a === '--ts' || a === '--typescript');
const wantsHelp = flags.some(a => a === '-h' || a === '--help');

if (!wantsHelp) {
  const KNOWN_FLAGS = new Set(['--ts', '--typescript']);
  const unknown = flags.filter(
    f => f !== '-h' && f !== '--help' && !KNOWN_FLAGS.has(f),
  );
  if (unknown.length) {
    console.error(`Unknown option(s): ${unknown.join(', ')}`);
    process.exit(1);
  }
  if (positional.length > 1) {
    console.error(
      `Expected a single <project-name>, got: ${positional.join(', ')}`,
    );
    process.exit(1);
  }
}

const arg = positional[0];
if (!arg || wantsHelp) {
  console.log('Create a new embedded-react app:\n');
  console.log('  npm create embedded-react@latest <project-name>');
  console.log('  npx create-embedded-react <project-name>\n');
  console.log('Options:');
  console.log('  --ts, --typescript   scaffold a TypeScript app\n');
  process.exit(arg ? 0 : 1);
}

const TEMPLATE = resolve(
  PKG_ROOT,
  wantsTs ? 'template-typescript' : 'template',
);

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
    // npm strips a published .gitignore, so the template ships it as `gitignore`; restore the dot here.
    const dp = join(dst, name === 'gitignore' ? '.gitignore' : name);
    if (statSync(sp).isDirectory()) copyDir(sp, dp);
    else if (TEXT.test(name) || name === 'gitignore')
      writeFileSync(dp, subst(readFileSync(sp, 'utf8')));
    else copyFileSync(sp, dp); // binary asset (png, ttf, …)
  }
}

copyDir(TEMPLATE, destDir);

const where = relative(process.cwd(), destDir) || '.';
console.log(`\n✓ Created "${appName}" at ${where}\n`);
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
