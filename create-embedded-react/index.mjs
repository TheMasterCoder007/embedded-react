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

import { readdirSync, statSync, mkdirSync, readFileSync, writeFileSync, copyFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, join, relative, basename } from 'node:path';

const PKG_ROOT = dirname(fileURLToPath(import.meta.url));
const TEMPLATE = resolve(PKG_ROOT, 'template');
const erVersion = JSON.parse(readFileSync(resolve(PKG_ROOT, 'package.json'), 'utf8')).version;

const arg = process.argv[2];
if (!arg || arg.startsWith('-')) {
  console.log('Create a new embedded-react app:\n');
  console.log('  npm create embedded-react@latest <project-name>');
  console.log('  npx create-embedded-react <project-name>\n');
  process.exit(arg ? 0 : 1);
}

const destDir = resolve(process.cwd(), arg);
const appName = basename(destDir);
const pkgName = appName.toLowerCase().replace(/[^a-z0-9-~.]/g, '-').replace(/^[-.]+/, '') || 'embedded-react-app';

if (existsSync(destDir) && readdirSync(destDir).length) {
  console.error(`Target directory "${arg}" already exists and is not empty.`);
  process.exit(1);
}

const TEXT = /\.(jsx?|tsx?|json|md)$/;
const subst = (s) => s.replaceAll('__APP_NAME__', pkgName).replaceAll('__ER_VERSION__', `^${erVersion}`);

/** Copy template → dest, substituting placeholders in text files and restoring the dotfile name. */
function copyDir(src, dst) {
  mkdirSync(dst, { recursive: true });
  for (const name of readdirSync(src)) {
    const sp = join(src, name);
    // npm strips a published .gitignore, so the template ships it as `gitignore`; restore the dot here.
    const dp = join(dst, name === 'gitignore' ? '.gitignore' : name);
    if (statSync(sp).isDirectory()) copyDir(sp, dp);
    else if (TEXT.test(name) || name === 'gitignore') writeFileSync(dp, subst(readFileSync(sp, 'utf8')));
    else copyFileSync(sp, dp); // binary asset (png, ttf, …)
  }
}

copyDir(TEMPLATE, destDir);

const where = relative(process.cwd(), destDir) || '.';
console.log(`\n✓ Created "${appName}" at ${where}\n`);
console.log('Next steps:');
if (where !== '.') console.log(`  cd ${where}`);
console.log('  npm install');
console.log('  npm run dev        # WASM simulator with hot reload → http://localhost:3333\n');
