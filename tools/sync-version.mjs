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

// sync-version.mjs — propagate the repo-root VERSION (the single source of truth) into every artifact that
// carries a version, so the LOCKSTEP ship (engine + npm + ESP-IDF + PlatformIO) never drifts.
//
//   node tools/sync-version.mjs           # write VERSION into all targets
//   node tools/sync-version.mjs --check   # verify all targets already match VERSION (CI gate; non-zero on drift)
//
// Targets: bridges/quickjs/js/package.json, idf_component.yml, library.json (if present), engine/include/er_version.h.
// To release: edit VERSION, run this, commit, tag vX.Y.Z.

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const check = process.argv.includes('--check');

const version = readFileSync(resolve(ROOT, 'VERSION'), 'utf8').trim();
const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(version);
if (!m) {
  console.error(`VERSION must be "MAJOR.MINOR.PATCH" (got "${version}")`);
  process.exit(2);
}
const [, major, minor, patch] = m;

const drift = []; // { file, found } when --check finds a mismatch
const wrote = [];

/** Reads a file, applies `transform(text) → newText`, then writes (or, in --check, records any drift). */
function apply(relPath, transform, currentVersionOf) {
  const path = resolve(ROOT, relPath);
  if (!existsSync(path)) return; // optional target (e.g. library.json before Phase 2)
  const text = readFileSync(path, 'utf8');
  const found = currentVersionOf(text);
  if (check) {
    if (found !== version) drift.push({ file: relPath, found });
    return;
  }
  const next = transform(text);
  if (next !== text) {
    writeFileSync(path, next);
    wrote.push(relPath);
  }
}

// package.json / library.json — JSON "version" field (preserve 2-space formatting + trailing newline).
const jsonVersion = (text) => JSON.parse(text).version;
const setJsonVersion = (text) => {
  const obj = JSON.parse(text);
  obj.version = version;
  return JSON.stringify(obj, null, 2) + '\n';
};

apply('bridges/quickjs/js/package.json', setJsonVersion, jsonVersion);
apply('library.json', setJsonVersion, jsonVersion);

// idf_component.yml — a `version: "x"` line (regex, no YAML dependency).
const YAML_VER = /^version:\s*["']?([^"'\n]+)["']?\s*$/m;
apply(
  'idf_component.yml',
  (text) => text.replace(YAML_VER, `version: "${version}"`),
  (text) => YAML_VER.exec(text)?.[1]?.trim(),
);

// engine/include/er_version.h — regenerate the macro block (the C single-source-of-truth header).
const H_VER = /#define ER_VERSION_STRING "([^"]+)"/;
apply(
  'engine/include/er_version.h',
  (text) =>
    text
      .replace(/#define ER_VERSION_MAJOR \d+/, `#define ER_VERSION_MAJOR ${major}`)
      .replace(/#define ER_VERSION_MINOR \d+/, `#define ER_VERSION_MINOR ${minor}`)
      .replace(/#define ER_VERSION_PATCH \d+/, `#define ER_VERSION_PATCH ${patch}`)
      .replace(/#define ER_VERSION_STRING "[^"]*"/, `#define ER_VERSION_STRING "${version}"`),
  (text) => H_VER.exec(text)?.[1],
);

if (check) {
  if (drift.length) {
    console.error(`Version drift — these don't match VERSION (${version}):`);
    for (const d of drift) console.error(`  ${d.file}: ${d.found}`);
    console.error('Run `node tools/sync-version.mjs` and commit.');
    process.exit(1);
  }
  console.log(`✓ all artifacts at ${version}`);
} else {
  console.log(`Synced to ${version}${wrote.length ? ' → ' + wrote.join(', ') : ' (all already current)'}`);
}
