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
// The exact set of files a version bump touches is exported as versionedFiles() so tools/release.mjs can
// stage precisely those — one source of truth, so the writer and the release commit can never disagree.
// To release: edit VERSION, run this, commit, tag vX.Y.Z.

import {readFileSync, writeFileSync, existsSync} from 'node:fs';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {dirname, resolve} from 'node:path';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const VERSION_FILE = resolve(ROOT, 'VERSION');
const SEMVER = /^(\d+)\.(\d+)\.(\d+)$/;

/*----------------------------------------------------------------------------------------------------------------------
 - Targets: the single source of truth for which files carry the version
 ---------------------------------------------------------------------------------------------------------------------*/

// JSON manifests — the "version" field (preserve 2-space formatting + trailing newline).
const jsonRead = text => JSON.parse(text).version;
const jsonWrite = (text, version) => {
  const obj = JSON.parse(text);
  obj.version = version;
  return JSON.stringify(obj, null, 2) + '\n';
};

// npm lockfiles (lockfileVersion 3) carry the package version in TWO places — the top-level `version`
// and `packages[""].version` — and drift if only `npm install` ever touches them. Sync both. (A
// JSON.parse → re-stringify roundtrip of an npm-written lockfile is byte-identical, so the only change
// is the version lines.)
const lockRead = text => JSON.parse(text).version;
const lockWrite = (text, version) => {
  const obj = JSON.parse(text);
  obj.version = version;
  if (obj.packages && obj.packages['']) obj.packages[''].version = version;
  return JSON.stringify(obj, null, 2) + '\n';
};

// idf_component.yml — a `version: "x"` line (regex, no YAML dependency).
const YAML_VER = /^version:\s*["']?([^"'\n]+)["']?\s*$/m;
const yamlRead = text => YAML_VER.exec(text)?.[1]?.trim();
const yamlWrite = (text, version) =>
  text.replace(YAML_VER, `version: "${version}"`);

// er_version.h — the C single-source-of-truth header; regenerate the macro block.
const H_VER = /#define ER_VERSION_STRING "([^"]+)"/;
const headerRead = text => H_VER.exec(text)?.[1];
const headerWrite = (text, version) => {
  const [, major, minor, patch] = SEMVER.exec(version);
  return text
    .replace(
      /#define ER_VERSION_MAJOR \d+/,
      `#define ER_VERSION_MAJOR ${major}`,
    )
    .replace(
      /#define ER_VERSION_MINOR \d+/,
      `#define ER_VERSION_MINOR ${minor}`,
    )
    .replace(
      /#define ER_VERSION_PATCH \d+/,
      `#define ER_VERSION_PATCH ${patch}`,
    )
    .replace(
      /#define ER_VERSION_STRING "[^"]*"/,
      `#define ER_VERSION_STRING "${version}"`,
    );
};

// README.md — the install snippets pin the version (so copy-paste installs match the release):
// CMake FetchContent GIT_TAG, ESP-IDF `^x`, PlatformIO `#vx`. Each regex captures the prefix (group 1)
// and the MAJOR.MINOR.PATCH (group 2). Drift here ships a README that tells users to pin the wrong tag.
const README_PINS = [
  /(GIT_TAG\s+v)(\d+\.\d+\.\d+)/, // CMake FetchContent
  /(embedded-react\^)(\d+\.\d+\.\d+)/, // ESP-IDF add-dependency
  /(embedded-react\.git#v)(\d+\.\d+\.\d+)/, // PlatformIO lib_deps
];
const readmeRead = text => {
  const found = README_PINS.map(re => re.exec(text)?.[2]).filter(Boolean);
  if (!found.length) return undefined;
  return found.every(v => v === found[0])
    ? found[0]
    : `${found[0]} (install pins disagree)`;
};
const readmeWrite = (text, version) =>
  README_PINS.reduce((t, re) => t.replace(re, `$1${version}`), text);

// ESP32 example CMakeLists — the Flow A / Flow B fetch templates pin the embedded-react release they pull
// when copied OUT of the monorepo (`FetchContent ... GIT_TAG vX.Y.Z`). The pin must track the release, or a
// copied-out template re-drifts every bump and fetches the wrong tag. The regex matches only the
// `GIT_TAG <space> vX.Y.Z` form (the embedded_react declare) — NOT QuickJS's `GIT_TAG ${QUICKJS_GIT_TAG}`
// (no literal version) — so the QuickJS pin is left untouched.
const GIT_TAG_PIN = /(GIT_TAG\s+v)(\d+\.\d+\.\d+)/;
const gitTagRead = text => GIT_TAG_PIN.exec(text)?.[2];
const gitTagWrite = (text, version) =>
  text.replace(GIT_TAG_PIN, `$1${version}`);

// Version-bearing manifests — each independently published artifact, the engine's C header, the README
// install pins, and the ESP32 example fetch-template tags.
const MANIFESTS = [
  {path: 'bridges/quickjs/js/package.json', read: jsonRead, write: jsonWrite},
  {
    path: 'bridges/quickjs/js/package-lock.json',
    read: lockRead,
    write: lockWrite,
  },
  {
    path: 'create-embedded-react/package.json',
    read: jsonRead,
    write: jsonWrite,
  },
  {path: 'library.json', read: jsonRead, write: jsonWrite},
  {path: 'engine/idf_component.yml', read: yamlRead, write: yamlWrite},
  {path: 'engine/include/er_version.h', read: headerRead, write: headerWrite},
  {path: 'README.md', read: readmeRead, write: readmeWrite},
  {
    path: 'examples/esp32/esp32-s3/CMakeLists.txt',
    read: gitTagRead,
    write: gitTagWrite,
  },
  {
    path: 'examples/esp32/esp32-2432s028r/CMakeLists.txt',
    read: gitTagRead,
    write: gitTagWrite,
  },
];

// LICENSE + NOTICE: each independently published artifact (the npm packages; the engine as a CMake /
// ESP-IDF / PlatformIO component) is its OWN distribution, so per Apache-2.0 §4 it must carry its own
// LICENSE and NOTICE. The repo-root copies are the source of truth; mirror them into each package dir so
// `npm pack` / `compote upload` / `pio publish` include them (and never drift — --check verifies).
const LEGAL_FILES = ['LICENSE', 'NOTICE'];
const PACKAGE_DIRS = ['bridges/quickjs/js', 'create-embedded-react', 'engine'];
const legalMirrors = () =>
  PACKAGE_DIRS.flatMap(dir => LEGAL_FILES.map(f => `${dir}/${f}`));

/**
 * @brief The complete set of repo-relative files a version bump touches: the VERSION file, every
 *        version-bearing manifest, and each package's mirrored LICENSE/NOTICE. tools/release.mjs imports
 *        this so it stages exactly the files this script writes — the two can never drift apart.
 *
 * @return Array of repo-root-relative paths.
 */
export function versionedFiles() {
  return ['VERSION', ...MANIFESTS.map(t => t.path), ...legalMirrors()];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main: run only when invoked directly (a no-op side-effect-free import otherwise)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Syncs (or, with --check, verifies) every versioned artifact against the repo-root VERSION.
 *
 * @return Does not return; exits the process with a status code (0 ok, 1 drift, 2 bad args).
 */
function main() {
  // Modes:
  //   (default)        write VERSION → every artifact.
  //   --set X.Y.Z      first write X.Y.Z into VERSION, then sync (used by the release helper).
  //   --check          verify every artifact matches the VERSION file.
  //   --check X.Y.Z    verify every artifact AND the VERSION file match X.Y.Z (the release tag gate).
  const args = process.argv.slice(2);
  const check = args.includes('--check');
  const setIdx = args.indexOf('--set');
  const setVersion = setIdx >= 0 ? args[setIdx + 1] : null;
  const versionArg = args.find((a, i) => SEMVER.test(a) && i !== setIdx + 1); // bare X.Y.Z (not the --set value)

  if (setVersion && !SEMVER.test(setVersion)) {
    console.error(
      `--set needs a MAJOR.MINOR.PATCH version (got "${setVersion ?? ''}")`,
    );
    process.exit(2);
  }
  // In --set mode, the new version IS the source of truth — write it into VERSION up front.
  if (setVersion && !check) writeFileSync(VERSION_FILE, setVersion + '\n');

  // The version everything is expected to be: --check <tag> uses the tag; otherwise the VERSION file.
  const version =
    check && versionArg
      ? versionArg
      : readFileSync(VERSION_FILE, 'utf8').trim();
  if (!SEMVER.test(version)) {
    console.error(`VERSION must be "MAJOR.MINOR.PATCH" (got "${version}")`);
    process.exit(2);
  }

  const drift = []; // { file, found } when --check finds a mismatch
  const wrote = [];

  // In `--check <tag>` mode the VERSION file itself must equal the tag (catches the "forgot to bump" case).
  if (check && versionArg) {
    const fileVersion = readFileSync(VERSION_FILE, 'utf8').trim();
    if (fileVersion !== versionArg)
      drift.push({file: 'VERSION', found: fileVersion});
  }

  // Version-bearing manifests.
  for (const t of MANIFESTS) {
    const path = resolve(ROOT, t.path);
    if (!existsSync(path)) continue; // optional target (e.g. library.json before Phase 2)
    const text = readFileSync(path, 'utf8');
    if (check) {
      const found = t.read(text);
      if (found !== version) drift.push({file: t.path, found});
      continue;
    }
    const next = t.write(text, version);
    if (next !== text) {
      writeFileSync(path, next);
      wrote.push(t.path);
    }
  }

  // Per-package LICENSE + NOTICE mirrors.
  for (const dir of PACKAGE_DIRS) {
    for (const f of LEGAL_FILES) {
      const want = readFileSync(resolve(ROOT, f), 'utf8');
      const dst = resolve(ROOT, dir, f);
      const have = existsSync(dst) ? readFileSync(dst, 'utf8') : null;
      if (check) {
        if (have !== want)
          drift.push({
            file: `${dir}/${f}`,
            found: have == null ? '(missing)' : 'differs from root',
          });
      } else if (have !== want) {
        writeFileSync(dst, want);
        wrote.push(`${dir}/${f}`);
      }
    }
  }

  if (check) {
    if (drift.length) {
      console.error(
        `Drift from the repo-root source of truth (VERSION=${version}, LICENSE, NOTICE):`,
      );
      for (const d of drift) console.error(`  ${d.file}: ${d.found}`);
      console.error('Run `node tools/sync-version.mjs` and commit.');
      process.exit(1);
    }
    console.log(`✓ all artifacts at ${version}; LICENSE + NOTICE in sync`);
  } else {
    console.log(
      `Synced to ${version}${wrote.length ? ' → ' + wrote.join(', ') : ' (all already current)'}`,
    );
  }
}

// Run only when executed directly (`node tools/sync-version.mjs ...`), not when imported (release.mjs).
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main();
