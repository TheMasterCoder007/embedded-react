// Stages everything the playground page needs into static/playground/ (gitignored). Runs before
// `start` and `build`.
//
// The playground compiles the editor's JSX in the browser and hands the result to the engine, so
// the pieces are: the engine wasm (shipped prebuilt by the `embedded-react` package the site depends
// on, so this needs no Emscripten), one vendor bundle of React + the embedded-react library that the
// browser-compiled app links against, and the starter's source files and baked asset pack.
import {execFileSync} from 'node:child_process';
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, relative, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import * as esbuild from 'esbuild';

const siteDirectory = dirname(dirname(fileURLToPath(import.meta.url)));
const repositoryRoot = resolve(siteDirectory, '..');
const packageDirectory = resolve(siteDirectory, 'node_modules/embedded-react');
const outputDirectory = resolve(siteDirectory, 'static/playground');

// The playground runs the `create-embedded-react` starter, which is what `npm create embedded-react`
// scaffolds: something simple to start from and change.
const demos = [
  {
    id: 'starter',
    directory: resolve(repositoryRoot, 'create-embedded-react/template'),
  },
];

rmSync(outputDirectory, {recursive: true, force: true});
mkdirSync(join(outputDirectory, 'engine'), {recursive: true});

// 1. The engine.
for (const fileName of ['embedded-react.js', 'embedded-react.wasm']) {
  copyFileSync(
    join(packageDirectory, 'sim', fileName),
    join(outputDirectory, 'engine', fileName),
  );
}

// 2. The vendor bundle: what the simulator's own bundler links every app against, exposed on a
// global for the browser-side module loader. React is pinned to the package's copy (the site itself
// uses a newer React), the way the package's dev server pins it.
const pinnedPackages = Object.fromEntries(
  ['react', 'react-reconciler', 'scheduler']
    .map(packageName => [
      packageName,
      join(packageDirectory, 'node_modules', packageName),
    ])
    .filter(([, packagePath]) => existsSync(packagePath)),
);
await esbuild.build({
  stdin: {
    contents: `
      import * as React from 'react';
      import * as JSXRuntime from 'react/jsx-runtime';
      import * as ER from 'embedded-react';
      globalThis.__erVendor = {react: React, 'react/jsx-runtime': JSXRuntime, 'embedded-react': ER};
    `,
    resolveDir: packageDirectory,
    loader: 'js',
  },
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  target: 'es2020',
  alias: {
    'embedded-react': join(packageDirectory, 'src/embedded-react/index.js'),
    ...pinnedPackages,
  },
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  minify: true,
  outfile: join(outputDirectory, 'vendor.js'),
});

// 3. Each demo: its source files for the editor, and its asset pack. The pack comes from the CLI's
// static export (the same bake the simulator does), of which only the pack is kept.
const SOURCE_FILE = /\.(jsx?|tsx?)$/;
const SKIPPED_NAMES = new Set(['node_modules', 'dist', 'sim-export', 'assets']);
const walk = (directory, baseDirectory = directory, collected = {}) => {
  for (const name of readdirSync(directory)) {
    if (SKIPPED_NAMES.has(name)) continue;
    const entryPath = join(directory, name);
    if (statSync(entryPath).isDirectory())
      walk(entryPath, baseDirectory, collected);
    else if (SOURCE_FILE.test(name))
      collected[relative(baseDirectory, entryPath).replace(/\\/g, '/')] =
        readFileSync(entryPath, 'utf8');
  }
  return collected;
};

for (const {id, directory} of demos) {
  const entry = existsSync(join(directory, 'index.jsx'))
    ? 'index.jsx'
    : 'src/index.jsx';
  if (!existsSync(join(directory, entry)))
    throw new Error(`${id}: no entry file in ${directory}`);
  const files = walk(directory);
  // The template registers the app under a placeholder the scaffolder fills in.
  for (const fileName of Object.keys(files))
    files[fileName] = files[fileName].replaceAll('__APP_NAME__', id);
  mkdirSync(join(outputDirectory, id), {recursive: true});
  writeFileSync(
    join(outputDirectory, id, 'files.json'),
    JSON.stringify({entry, files}),
  );

  const temporaryDirectory = mkdtempSync(join(tmpdir(), 'er-playground-'));
  try {
    execFileSync(
      process.execPath,
      [
        join(packageDirectory, 'cli.mjs'),
        'export',
        join(directory, entry),
        '--out',
        temporaryDirectory,
      ],
      {
        stdio: ['ignore', 'ignore', 'inherit'],
      },
    );
    const packPath = join(temporaryDirectory, 'public/assets.pack');
    if (existsSync(packPath))
      copyFileSync(packPath, join(outputDirectory, id, 'assets.pack'));
  } finally {
    rmSync(temporaryDirectory, {recursive: true, force: true});
  }
  console.log(`playground: ${id} (${Object.keys(files).length} file(s))`);
}
