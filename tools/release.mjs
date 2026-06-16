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

// release.mjs — cut a lockstep release in one step: bump VERSION → sync every artifact → commit → tag.
//
//   node tools/release.mjs 0.2.0             bump, sync, `git commit "release: v0.2.0"`, `git tag v0.2.0`
//   node tools/release.mjs 0.2.0 --dry-run   print the plan; change nothing (no file writes, no git)
//
// Then:  git push --follow-tags     → the release workflow gates (tag == VERSION) and publishes all channels.
//
// The `git add` is restricted to the version-bearing files, so unrelated working-tree changes are never
// swept into the release commit. Fails early if the tag already exists or the tree has staged changes.

import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { versionedFiles } from './sync-version.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const dryRun = args.includes('--dry-run');
const version = args.find((a) => /^\d+\.\d+\.\d+$/.test(a));

if (!version) {
  console.error('usage: node tools/release.mjs <MAJOR.MINOR.PATCH> [--dry-run]');
  process.exit(2);
}
const tag = `v${version}`;

// The files the version bump touches — committed together, added explicitly (never `git add -A`), so
// unrelated working-tree changes are never swept into the release commit. The list is sourced from
// tools/sync-version.mjs (versionedFiles()) — the same script that WRITES them — so the writer and the
// release commit can't drift apart. (A file synced but not staged would land on disk yet never commit,
// shipping a tag that fails CI's `--check`.)
const FILES = versionedFiles();

const git = (...a) => execFileSync('git', a, { cwd: ROOT, stdio: 'inherit' });
const gitOut = (...a) => execFileSync('git', a, { cwd: ROOT, encoding: 'utf8' }).trim();
const node = (...a) => execFileSync(process.execPath, a, { cwd: ROOT, stdio: 'inherit' });

// Preconditions (checked even in dry-run so the preview is honest).
if (gitOut('tag', '--list', tag)) {
  console.error(`tag ${tag} already exists — pick a new version or delete the tag first`);
  process.exit(1);
}
if (gitOut('diff', '--cached', '--name-only')) {
  console.error('you have staged changes — commit or unstage them before releasing');
  process.exit(1);
}

if (dryRun) {
  console.log(`[dry-run] release ${tag}:`);
  console.log(`  1. set VERSION=${version} and sync all artifacts (tools/sync-version.mjs --set)`);
  console.log(`  2. git add ${FILES.length} version files`);
  console.log(`  3. git commit -m "release: ${tag}"`);
  console.log(`  4. git tag ${tag}`);
  console.log(`  then: git push --follow-tags   (triggers the release workflow)`);
  process.exit(0);
}

node(resolve(ROOT, 'tools/sync-version.mjs'), '--set', version); // 1. bump + sync
git('add', ...FILES); // 2. stage only the version files
// 3. Commit the bump — unless the version is already current (nothing staged), in which case we are simply
// tagging the already-committed version (re-releasing the current commit), so skip the empty commit.
if (gitOut('diff', '--cached', '--name-only')) {
  git('commit', '-m', `release: ${tag}`);
} else {
  console.log(`VERSION is already ${version} and committed — tagging the current commit (no bump needed).`);
}
git('tag', '-a', tag, '-m', `release: ${tag}`); // 4. annotated tag (carries tagger/date + works with --follow-tags)
console.log(`\n✓ tagged ${tag}. Push to release:  git push --follow-tags`);
