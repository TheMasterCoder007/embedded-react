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

// release.mjs — cut a lockstep release in one step: bump VERSION → sync every artifact (manifests +
// README pins) → stamp the CHANGELOG → commit → tag.
//
//   node tools/release.mjs 0.3.0             bump, sync, stamp changelog, commit "release: v0.3.0", tag v0.3.0
//   node tools/release.mjs 0.3.0 --dry-run   print the plan; change nothing (no file writes, no git)
//
// Then:  git push --follow-tags     → the release workflow gates (tag == VERSION) and publishes all channels.
//
// Day to day you just add notes under "## [Unreleased]" in CHANGELOG.md; the release promotes that section
// to the dated version automatically. The `git add` is restricted to the version-bearing files + the
// changelog, so unrelated working-tree changes are never swept into the release commit.

import {execFileSync} from 'node:child_process';
import {readFileSync, writeFileSync, existsSync} from 'node:fs';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve} from 'node:path';
import {versionedFiles} from './sync-version.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

/**
 * @brief Promotes CHANGELOG.md's "## [Unreleased]" section to a dated release heading, leaving a fresh
 *        empty [Unreleased] on top, and updates the compare-link refs (Unreleased → new tag, plus a
 *        [version] link). Idempotent: a no-op if "## [version]" is already present (e.g., re-tagging the
 *        current commit), so re-running release on the same version won't double-stamp.
 *
 * @param[in] version        X.Y.Z being released.
 * @param[in] date           Release date (YYYY-MM-DD).
 * @param[in] changelogPath  CHANGELOG path (defaults to the repo root; overridable for tests).
 *
 * @return true if the file was changed.
 */
export function stampChangelog(
  version,
  date,
  changelogPath = resolve(ROOT, 'CHANGELOG.md'),
) {
  if (!existsSync(changelogPath)) return false;
  let text = readFileSync(changelogPath, 'utf8');
  const verEsc = version.replace(/\./g, '\\.');
  if (new RegExp(`^## \\[${verEsc}\\]`, 'm').test(text)) return false; // already stamped

  if (!/^## \[Unreleased\]\s*$/m.test(text)) {
    throw new Error('CHANGELOG.md has no "## [Unreleased]" heading to promote');
  }
  // Promote the Unreleased section to the dated version, keeping a fresh empty [Unreleased] for next time.
  text = text.replace(
    /^## \[Unreleased\]\s*$/m,
    `## [Unreleased]\n\n## [${version}] - ${date}`,
  );

  // Update the link refs at the bottom: repoint [Unreleased] and add a [version] compare link.
  const link = text.match(
    /^\[Unreleased\]:\s*(.*?)\/compare\/v(\d+\.\d+\.\d+)\.\.\.HEAD\s*$/m,
  );
  if (link) {
    const [, base, prev] = link;
    text = text.replace(
      /^\[Unreleased\]:.*$/m,
      `[Unreleased]: ${base}/compare/v${version}...HEAD\n[${version}]: ${base}/compare/v${prev}...v${version}`,
    );
  }
  writeFileSync(changelogPath, text);
  return true;
}

async function main() {
  const args = process.argv.slice(2);
  const dryRun = args.includes('--dry-run');
  const version = args.find(a => /^\d+\.\d+\.\d+$/.test(a));
  if (!version) {
    console.error(
      'usage: node tools/release.mjs <MAJOR.MINOR.PATCH> [--dry-run]',
    );
    process.exit(2);
  }
  const tag = `v${version}`;
  const date = new Date().toISOString().slice(0, 10);

  // The files the version bump touches (sync-version.mjs writes these) + the changelog. Added explicitly
  // (never `git add -A`) so unrelated working-tree changes are never swept into the release commit.
  const FILES = versionedFiles();

  const git = (...a) => execFileSync('git', a, {cwd: ROOT, stdio: 'inherit'});
  const gitOut = (...a) =>
    execFileSync('git', a, {cwd: ROOT, encoding: 'utf8'}).trim();
  const gitTry = (...a) => {
    try {
      return gitOut(...a);
    } catch {
      return '';
    }
  };
  const node = (...a) =>
    execFileSync(process.execPath, a, {cwd: ROOT, stdio: 'inherit'});

  // Preconditions (checked even in dry-run so the preview is honest).

  // Release only from the default branch. A tag cut on a feature branch still triggers the publish
  // workflow (it fires on the tag, not the branch), so you'd ship off-branch code — and once the branch
  // is squash/rebase-merged the tag dangles off master's history. Override with --allow-branch.
  const branch = gitTry('rev-parse', '--abbrev-ref', 'HEAD');
  const defaultBranch =
    gitTry('symbolic-ref', '--quiet', 'refs/remotes/origin/HEAD').replace(
      /^refs\/remotes\/origin\//,
      '',
    ) || 'master';
  if (!args.includes('--allow-branch') && branch && branch !== defaultBranch) {
    console.error(
      `release must be cut from the default branch (${defaultBranch}); you are on "${branch}".`,
    );
    console.error(
      `merge your work into ${defaultBranch} and run it there, or pass --allow-branch to override.`,
    );
    process.exit(1);
  }

  if (gitOut('tag', '--list', tag)) {
    console.error(
      `tag ${tag} already exists — pick a new version or delete the tag first`,
    );
    process.exit(1);
  }
  if (gitOut('diff', '--cached', '--name-only')) {
    console.error(
      'you have staged changes — commit or unstage them before releasing',
    );
    process.exit(1);
  }

  if (dryRun) {
    console.log(`[dry-run] release ${tag}:`);
    console.log(
      `  1. set VERSION=${version} and sync all artifacts (manifests + README install pins)`,
    );
    console.log(
      `  2. stamp CHANGELOG: "## [Unreleased]" → "## [${version}] - ${date}" (+ compare link)`,
    );
    console.log(`  3. git add ${FILES.length} version files + CHANGELOG.md`);
    console.log(`  4. git commit -m "release: ${tag}"`);
    console.log(`  5. git tag ${tag}`);
    console.log(
      `  then: git push --follow-tags   (triggers the release workflow)`,
    );
    process.exit(0);
  }

  node(resolve(ROOT, 'tools/sync-version.mjs'), '--set', version); // 1. bump + sync (VERSION, manifests, README)
  const stamped = stampChangelog(version, date); // 2. promote the changelog's Unreleased section
  git('add', ...FILES, ...(stamped ? ['CHANGELOG.md'] : [])); // 3. stage only the version files (+ changelog)
  // 4. Commit the bump — unless nothing is staged (re-tagging the already-released current commit).
  if (gitOut('diff', '--cached', '--name-only')) {
    git('commit', '-m', `release: ${tag}`);
  } else {
    console.log(
      `VERSION is already ${version} and committed — tagging the current commit (no bump needed).`,
    );
  }
  git('tag', '-a', tag, '-m', `release: ${tag}`); // 5. annotated tag (carries tagger/date + works with --follow-tags)
  console.log(`\n✓ tagged ${tag}. Push to release:  git push --follow-tags`);
}

// Run only when invoked directly (`node tools/release.mjs ...`), not when imported (e.g. by a test).
if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href
) {
  await main();
}
