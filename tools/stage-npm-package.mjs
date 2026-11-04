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
 * stage-npm-package.mjs — copy the embedded-react npm package into packages/embedded-react/dist/npm-package with
 * its TypeScript compiled to JavaScript, ready for `npm pack <dir>` / `npm publish <dir>`.
 *
 * Part of the package is written in TypeScript (.mts). In the repo Node runs those files directly by
 * stripping their types, but it refuses to for a file under node_modules, which is where an installed
 * package lives. So the published package ships JavaScript: each .mts file becomes a .mjs file, and every
 * relative import of a .mts file is rewritten to name the .mjs.
 *
 *   node tools/stage-npm-package.mjs      prints the staged directory
 *
 * Needs the package's dev dependencies installed (npm ci in packages/embedded-react) for the TypeScript compiler.
 */

import {execFileSync} from 'node:child_process';
import {
  copyFileSync,
  mkdirSync,
  readFileSync,
  rmSync,
  unlinkSync,
  writeFileSync,
} from 'node:fs';
import {createRequire} from 'node:module';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PACKAGE_DIR = resolve(ROOT, 'packages/embedded-react');
export const STAGE = resolve(PACKAGE_DIR, 'dist/npm-package');

const MODULE_FILE = /\.(m?js|mts)$/;

/** The source with each relative import of a .mts file pointed at its .mjs. */
function retargetImports(typescript, source, fileName) {
  // preProcessFile is TypeScript's own import scanner: static imports, re-exports and import() calls.
  const {importedFiles} = typescript.preProcessFile(source, true, true);
  let result = source;
  for (const importedFile of [...importedFiles].sort(
    (later, earlier) => earlier.pos - later.pos,
  )) {
    if (
      !/^\.\.?\//.test(importedFile.fileName) ||
      !importedFile.fileName.endsWith('.mts')
    ) {
      continue;
    }
    const target = importedFile.fileName.replace(/\.mts$/, '.mjs');
    // `pos` is the specifier's opening quote.
    const start = /['"]/.test(result[importedFile.pos])
      ? importedFile.pos + 1
      : importedFile.pos;
    if (!result.startsWith(importedFile.fileName, start)) {
      throw new Error(
        `${fileName}: could not locate the import of ${importedFile.fileName}`,
      );
    }
    result =
      result.slice(0, start) +
      target +
      result.slice(start + importedFile.fileName.length);
  }
  return result;
}

/** Stages the package and returns the directory. */
export function stageNpmPackage() {
  const typescript = createRequire(join(PACKAGE_DIR, 'package.json'))(
    'typescript',
  );
  // Exactly the files `npm pack` would ship (the `files` whitelist, plus package.json, README, LICENSE).
  const [{files}] = JSON.parse(
    execFileSync('npm', ['pack', '--dry-run', '--json', '--ignore-scripts'], {
      cwd: PACKAGE_DIR,
      encoding: 'utf8',
      shell: process.platform === 'win32',
    }),
  );
  const shipped = new Set(files.map(entry => entry.path));
  for (const {path} of files) {
    if (path.endsWith('.mts') && shipped.has(path.replace(/\.mts$/, '.mjs'))) {
      throw new Error(`${path} and its .mjs twin would both ship`);
    }
  }
  rmSync(STAGE, {recursive: true, force: true});
  for (const {path} of files) {
    mkdirSync(dirname(join(STAGE, path)), {recursive: true});
    copyFileSync(join(PACKAGE_DIR, path), join(STAGE, path));
  }

  let compiled = 0;
  for (const {path} of files.filter(entry => MODULE_FILE.test(entry.path))) {
    const file = join(STAGE, path);
    const source = retargetImports(
      typescript,
      readFileSync(file, 'utf8'),
      path,
    );
    if (!path.endsWith('.mts')) {
      writeFileSync(file, source);
      continue;
    }
    // Erase the types only; verbatimModuleSyntax keeps every import as written, as Node runs it in the repo.
    const {outputText, diagnostics} = typescript.transpileModule(source, {
      fileName: path,
      reportDiagnostics: true,
      compilerOptions: {
        module: typescript.ModuleKind.ESNext,
        target: typescript.ScriptTarget.ES2022, // what tsconfig.aot.json checks against
        verbatimModuleSyntax: true,
      },
    });
    if (diagnostics?.length) {
      throw new Error(
        `${path}: ${typescript.flattenDiagnosticMessageText(diagnostics[0].messageText, '\n')}`,
      );
    }
    writeFileSync(file.replace(/\.mts$/, '.mjs'), outputText);
    unlinkSync(file);
    compiled++;
  }

  // Nothing may still point at TypeScript: an installed package cannot load it.
  for (const {path} of files.filter(entry => MODULE_FILE.test(entry.path))) {
    const staged = join(STAGE, path.replace(/\.mts$/, '.mjs'));
    const leftover = typescript
      .preProcessFile(readFileSync(staged, 'utf8'), true, true)
      .importedFiles.find(
        importedFile =>
          /^\.\.?\//.test(importedFile.fileName) &&
          /\.m?ts$/.test(importedFile.fileName),
      );
    if (leftover) {
      throw new Error(`${path} still imports ${leftover.fileName}`);
    }
  }
  console.error(
    `staged ${files.length} files (${compiled} compiled from TypeScript)`,
  );
  return STAGE;
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  console.log(stageNpmPackage());
}
