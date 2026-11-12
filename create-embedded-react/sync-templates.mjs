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

// sync-templates.mjs — stage the repo's demo apps (../demos) into this package's templates/ so they ship
// as `create-embedded-react --template <name>` starters. npm can only publish files INSIDE the package
// dir, and the demos live at the repo root, so they must be COPIED in. templates/ is generated (gitignored)
// and rebuilt here; the `prepack` lifecycle runs this before every `npm pack`/`npm publish`, so a fresh
// clone that runs the scaffolder against ../demos and a published tarball scaffold the identical app.
//
//   node create-embedded-react/sync-templates.mjs
//
// Each demo is a self-contained consumer project already (package.json → the embedded-react CLI, its own
// .gitignore). We copy it verbatim, minus build/junk dirs, renaming .gitignore → gitignore (npm strips a
// published .gitignore; index.mjs restores the dot when scaffolding). Placeholder substitution and the
// package.json name/version/dependency pin happen at scaffold time in index.mjs, not here.

import {
  readdirSync,
  statSync,
  mkdirSync,
  copyFileSync,
  existsSync,
  rmSync,
} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, resolve, join} from 'node:path';

const PACKAGE_ROOT = dirname(fileURLToPath(import.meta.url));
const DEMOS_DIR = resolve(PACKAGE_ROOT, '..', 'demos');
const OUT_DIR = resolve(PACKAGE_ROOT, 'templates');

// Never copied into a template: dependency/build output and editor/OS cruft.
const SKIP_DIRS = new Set([
  'node_modules',
  'dist',
  'sim-export',
  'build',
  '.git',
  '.idea',
  '.vscode',
]);
const SKIP_FILES = new Set(['.DS_Store', 'Thumbs.db']);

/** Recursively copy src → dst, skipping build/junk and renaming .gitignore → gitignore for publish. */
function copyDir(source, destination) {
  mkdirSync(destination, {recursive: true});
  for (const name of readdirSync(source)) {
    if (SKIP_FILES.has(name)) continue;
    const sourcePath = join(source, name);
    if (statSync(sourcePath).isDirectory()) {
      if (SKIP_DIRS.has(name)) continue;
      copyDir(sourcePath, join(destination, name));
    } else {
      copyFileSync(
        sourcePath,
        join(destination, name === '.gitignore' ? 'gitignore' : name),
      );
    }
  }
}

if (!existsSync(DEMOS_DIR)) {
  console.error(`No demos directory at ${DEMOS_DIR}; nothing to stage.`);
  process.exit(1);
}

// A demo is any ../demos/<name> with an index.jsx or index.tsx entry (the same rule the in-repo build tools use).
const demos = readdirSync(DEMOS_DIR, {withFileTypes: true})
  .filter(
    entry =>
      entry.isDirectory() &&
      (existsSync(join(DEMOS_DIR, entry.name, 'index.jsx')) ||
        existsSync(join(DEMOS_DIR, entry.name, 'index.tsx'))),
  )
  .map(entry => entry.name);

rmSync(OUT_DIR, {recursive: true, force: true});
for (const name of demos) copyDir(join(DEMOS_DIR, name), join(OUT_DIR, name));

// Log to stderr: this runs as a `prepack` lifecycle script, and npm forwards a child's stdout into the
// `npm pack --json` output — logging there would corrupt that JSON (which tooling parses).
console.error(
  `✓ staged ${demos.length} demo template(s) → create-embedded-react/templates/  (${demos.join(', ')})`,
);
