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
 * Checks that a change to the Flow B compiler left its output exactly as it was. The unit tests only
 * assert fragments of the generated C; this compares all of it, and every error message too.
 *
 *   npm run aot:baseline                  record from the working tree
 *   npm run aot:baseline -- --ref HEAD    record from a commit, whatever the working tree holds
 *   npm run aot:same                      compile the recorded inputs again and compare
 *
 * The baseline is every compileSource call the AOT unit tests make, plus both demos at each board size,
 * recorded with what the compiler returned (or threw). It lives in node_modules/.cache/aot-same.
 */
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const JS = resolve(here, '../..'); // bridges/quickjs/js
const REPO = resolve(JS, '../../..');
const CACHE = resolve(JS, 'node_modules/.cache/aot-same');
const CORPUS = join(CACHE, 'corpus.jsonl');
const BASELINE = join(CACHE, 'baseline.json');
const META = join(CACHE, 'meta.json');

const DEMOS = ['thermostat', 'watch-face'];
// The board sizes, plus the rotated 320×240 and 480×800 that reach the thermostat's landscape and stack layouts.
const SIZES = [
  [240, 320],
  [240, 280],
  [320, 480],
  [800, 480],
  [320, 240],
  [480, 800],
];

const readCorpus = path =>
  readFileSync(path, 'utf8').trim().split('\n').map(JSON.parse);

/** The compiler's entry in a package tree: compile.mts, or compile.mjs in a commit from before it moved. */
function compilerEntry(packageDir) {
  const mtsEntry = join(packageDir, 'aot/compile.mts');
  return existsSync(mtsEntry) ? mtsEntry : join(packageDir, 'aot/compile.mjs');
}

/** Compiles corpus records with one compiler. Each env group runs in its own process with that env set,
 *  so a compiler that reads ER_AOT_* at import sees the same values as one that reads them per call. */
function replay(corpusPath, compilerPath) {
  const groups = new Map();
  readCorpus(corpusPath).forEach((record, recordIndex) => {
    const key = JSON.stringify(record.env);
    if (!groups.has(key)) {
      groups.set(key, []);
    }
    groups.get(key).push(recordIndex);
  });
  const work = mkdtempSync(join(tmpdir(), 'aot-same-'));
  const results = {};
  try {
    let groupIndex = 0;
    for (const [key, indices] of groups) {
      const env = Object.fromEntries(
        Object.entries(process.env).filter(
          ([varName]) => !varName.startsWith('ER_AOT_'),
        ),
      );
      Object.assign(env, JSON.parse(key));
      const groupFile = join(work, `group${groupIndex++}.json`);
      const args = [
        fileURLToPath(import.meta.url),
        'replay-group',
        corpusPath,
        compilerPath,
        groupFile,
        JSON.stringify(indices),
      ];
      const child = spawnSync(process.execPath, args, {
        env,
        stdio: ['ignore', 'ignore', 'inherit'],
      });
      if (child.status !== 0) {
        throw new Error(
          `replaying the ${key} group failed (exit ${child.status})`,
        );
      }
      Object.assign(results, JSON.parse(readFileSync(groupFile, 'utf8')));
    }
  } finally {
    rmSync(work, {recursive: true, force: true});
  }
  return results;
}

/** Child process of replay(): compile the given records and write what each returned or threw. */
async function replayGroup(corpusPath, compilerPath, outPath, indicesJson) {
  const records = readCorpus(corpusPath);
  const {compileSource} = await import(pathToFileURL(compilerPath).href);
  const results = {};
  for (const recordIndex of JSON.parse(indicesJson)) {
    const {src, demo, opts} = records[recordIndex];
    try {
      results[recordIndex] = {ok: compileSource(src, demo, opts)};
    } catch (error) {
      results[recordIndex] = {
        error: String(error?.message ?? error),
        aotLoc: error?.aotLoc ?? null,
        aotHint: error?.aotHint ?? null,
      };
    }
  }
  writeFileSync(outPath, JSON.stringify(results));
}

/** A copy of the whole repo at `ref` (the tests read the engine headers and the starters too). */
function checkoutRef(ref) {
  const sha = spawnSync('git', ['rev-parse', '--verify', `${ref}^{commit}`], {
    cwd: REPO,
    encoding: 'utf8',
  }).stdout.trim();
  if (!/^[0-9a-f]{40}$/.test(sha)) {
    throw new Error(`unknown git ref "${ref}"`);
  }
  const dir = mkdtempSync(join(tmpdir(), `aot-same-${sha.slice(0, 7)}-`));
  const archive = spawnSync('git', ['archive', '--format=tar', sha], {
    cwd: REPO,
    maxBuffer: 1 << 30,
  });
  const untar = spawnSync('tar', ['-x', '-C', dir], {input: archive.stdout});
  if (archive.status !== 0 || untar.status !== 0) {
    throw new Error(`could not check out ${ref}`);
  }
  symlinkSync(
    join(JS, 'node_modules'),
    join(dir, 'bridges/quickjs/js/node_modules'),
  );
  return {dir, js: join(dir, 'bridges/quickjs/js'), sha};
}

async function baseline(ref) {
  const tree = ref ? checkoutRef(ref) : {js: JS};
  try {
    rmSync(CACHE, {recursive: true, force: true});
    mkdirSync(CACHE, {recursive: true});

    console.log(
      `Recording the AOT tests' compiles from ${ref ?? 'the working tree'}…`,
    );
    const testRun = spawnSync(
      'npx',
      ['vitest', 'run', '--config', join(here, 'vitest.config.mjs')],
      {
        cwd: tree.js,
        encoding: 'utf8',
        env: {...process.env, AOT_SAME_ROOT: tree.js, AOT_SAME_CORPUS: CORPUS},
      },
    );
    const summary = testRun.stdout.match(/^\s*Tests\s+.*$/m)?.[0].trim();
    if (!existsSync(CORPUS)) {
      process.stderr.write(testRun.stdout + testRun.stderr);
      throw new Error('the test run recorded nothing');
    }
    if (testRun.status !== 0) {
      console.warn(
        `  some tests failed (${summary}); their compiles are recorded all the same`,
      );
    }

    // Both demos at every board size, compiled the way `npm run aot -- <demo>` does.
    const compiler = compilerEntry(tree.js);
    const {bakeSvgArtifacts} = await import(pathToFileURL(compiler).href);
    const demosDir = resolve(tree.js, '../../../demos');
    for (const demo of DEMOS) {
      const src = readFileSync(join(demosDir, demo, 'App.jsx'), 'utf8');
      const svgArtifacts = await bakeSvgArtifacts(src, join(demosDir, demo));
      for (const [width, height] of SIZES) {
        const record = {
          src,
          demo,
          opts: {filename: `demos/${demo}/App.jsx`, svgArtifacts},
          env: {
            ER_AOT_SCREEN_W: String(width),
            ER_AOT_SCREEN_H: String(height),
          },
          test: `demo ${demo} at ${width}×${height}`,
        };
        writeFileSync(CORPUS, JSON.stringify(record) + '\n', {flag: 'a'});
      }
    }

    const results = replay(CORPUS, compiler);
    writeFileSync(BASELINE, JSON.stringify(results));
    const compileCount = Object.keys(results).length;
    const errors = Object.values(results).filter(result => result.error).length;
    const from = ref ? `${ref} (${tree.sha.slice(0, 7)})` : 'the working tree';
    writeFileSync(
      META,
      JSON.stringify({
        from,
        recorded: new Date().toISOString(),
        compiles: compileCount,
      }),
    );
    console.log(
      `Baseline: ${compileCount} compiles from ${from} (${compileCount - errors} build C, ${errors} are expected errors).`,
    );
  } finally {
    if (tree.dir) {
      rmSync(tree.dir, {recursive: true, force: true});
    }
  }
}

/** A unified diff of two strings, cut to `maxLines` lines. */
function unifiedDiff(before, after, maxLines = 80) {
  const dir = mkdtempSync(join(tmpdir(), 'aot-same-diff-'));
  try {
    writeFileSync(join(dir, 'before'), before);
    writeFileSync(join(dir, 'after'), after);
    const out = spawnSync(
      'diff',
      ['-u', '--label', 'baseline', '--label', 'now', 'before', 'after'],
      {cwd: dir, encoding: 'utf8'},
    ).stdout;
    const lines = out.split('\n');
    return lines.length > maxLines
      ? [
          ...lines.slice(0, maxLines),
          `… ${lines.length - maxLines} more lines`,
        ].join('\n')
      : out;
  } finally {
    rmSync(dir, {recursive: true, force: true});
  }
}

/** What differs between two results for one record: a field name, or the kind of result. */
function difference(was, now) {
  if (was.ok && now.ok) {
    const field = Object.keys({...was.ok, ...now.ok}).find(
      key => JSON.stringify(was.ok[key]) !== JSON.stringify(now.ok[key]),
    );
    const text = value =>
      typeof value === 'string' ? value : JSON.stringify(value, null, 2);
    return {
      what: `${field} differs`,
      before: text(was.ok[field]),
      after: text(now.ok[field]),
    };
  }
  if (was.error && now.error) {
    return {
      what: 'the error differs',
      before: JSON.stringify(was, null, 2),
      after: JSON.stringify(now, null, 2),
    };
  }
  return {
    what: was.ok
      ? 'it compiled before and throws now'
      : 'it threw before and compiles now',
    before: was.ok ? '(compiled)' : was.error,
    after: now.ok ? '(compiled)' : now.error,
  };
}

function check() {
  if (!existsSync(BASELINE)) {
    console.error('No baseline yet. Record one first: npm run aot:baseline');
    process.exit(2);
  }
  const meta = JSON.parse(readFileSync(META, 'utf8'));
  const records = readCorpus(CORPUS);
  const was = JSON.parse(readFileSync(BASELINE, 'utf8'));
  const now = replay(CORPUS, compilerEntry(JS));

  const differing = records
    .map((record, index) => ({record, index}))
    .filter(
      ({index}) => JSON.stringify(was[index]) !== JSON.stringify(now[index]),
    );
  const when = new Date(meta.recorded).toLocaleString([], {
    dateStyle: 'medium',
    timeStyle: 'short',
  });
  const from = `baseline from ${meta.from}, recorded ${when}`;
  if (!differing.length) {
    console.log(
      `✓ All ${records.length} compiles are identical to the ${from}.`,
    );
    return;
  }
  console.log(
    `✗ ${differing.length} of ${records.length} compiles differ from the ${from}:\n`,
  );
  for (const {record, index} of differing.slice(0, 20)) {
    console.log(
      `  ${record.test ?? `record ${index}`} — ${difference(was[index], now[index]).what}`,
    );
  }
  if (differing.length > 20) {
    console.log(`  … and ${differing.length - 20} more`);
  }
  const first = differing[0];
  const firstDiff = difference(was[first.index], now[first.index]);
  console.log(
    `\nFirst difference (${first.record.test ?? `record ${first.index}`}), ${firstDiff.what}:\n`,
  );
  console.log(unifiedDiff(firstDiff.before, firstDiff.after));
  process.exit(1);
}

const [command, ...args] = process.argv.slice(2);
if (command === 'replay-group') {
  await replayGroup(...args);
} else if (command === 'baseline') {
  const refFlag = args.indexOf('--ref');
  await baseline(refFlag >= 0 ? args[refFlag + 1] : undefined);
} else if (command === 'check') {
  check();
} else {
  console.error('usage: aot-same.mjs baseline [--ref <git-ref>] | check');
  process.exit(2);
}
