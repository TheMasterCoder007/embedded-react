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

const site = dirname(dirname(fileURLToPath(import.meta.url)));
const root = resolve(site, '..');
const pkg = resolve(site, 'node_modules/embedded-react');
const out = resolve(site, 'static/playground');

// The playground runs the `create-embedded-react` starter, which is what `npm create embedded-react`
// scaffolds: something simple to start from and change.
const demos = [
  {id: 'starter', dir: resolve(root, 'create-embedded-react/template')},
];

rmSync(out, {recursive: true, force: true});
mkdirSync(join(out, 'engine'), {recursive: true});

// 1. The engine.
for (const f of ['embedded-react.js', 'embedded-react.wasm']) {
  copyFileSync(join(pkg, 'sim', f), join(out, 'engine', f));
}

// 2. The vendor bundle: what the simulator's own bundler links every app against, exposed on a
// global for the browser-side module loader. React is pinned to the package's copy (the site itself
// uses a newer React), the way the package's dev server pins it.
const pin = Object.fromEntries(
  ['react', 'react-reconciler', 'scheduler']
    .map(p => [p, join(pkg, 'node_modules', p)])
    .filter(([, p]) => existsSync(p)),
);
await esbuild.build({
  stdin: {
    contents: `
      import * as React from 'react';
      import * as JSXRuntime from 'react/jsx-runtime';
      import * as ER from 'embedded-react';
      globalThis.__erVendor = {react: React, 'react/jsx-runtime': JSXRuntime, 'embedded-react': ER};
    `,
    resolveDir: pkg,
    loader: 'js',
  },
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  target: 'es2020',
  alias: {'embedded-react': join(pkg, 'src/embedded-react/index.js'), ...pin},
  define: {'process.env.NODE_ENV': '"production"'},
  legalComments: 'none',
  minify: true,
  outfile: join(out, 'vendor.js'),
});

// 3. Each demo: its source files for the editor, and its asset pack. The pack comes from the CLI's
// static export (the same bake the simulator does), of which only the pack is kept.
const SOURCE = /\.(jsx?|tsx?)$/;
const SKIP = new Set(['node_modules', 'dist', 'sim-export', 'assets']);
const walk = (dir, base = dir, acc = {}) => {
  for (const name of readdirSync(dir)) {
    if (SKIP.has(name)) continue;
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, base, acc);
    else if (SOURCE.test(name))
      acc[relative(base, p).replace(/\\/g, '/')] = readFileSync(p, 'utf8');
  }
  return acc;
};

for (const {id, dir} of demos) {
  const entry = existsSync(join(dir, 'index.jsx'))
    ? 'index.jsx'
    : 'src/index.jsx';
  if (!existsSync(join(dir, entry)))
    throw new Error(`${id}: no entry file in ${dir}`);
  const files = walk(dir);
  // The template registers the app under a placeholder the scaffolder fills in.
  for (const k of Object.keys(files))
    files[k] = files[k].replaceAll('__APP_NAME__', id);
  mkdirSync(join(out, id), {recursive: true});
  writeFileSync(join(out, id, 'files.json'), JSON.stringify({entry, files}));

  const tmp = mkdtempSync(join(tmpdir(), 'er-playground-'));
  try {
    execFileSync(
      process.execPath,
      [join(pkg, 'cli.mjs'), 'export', join(dir, entry), '--out', tmp],
      {
        stdio: ['ignore', 'ignore', 'inherit'],
      },
    );
    const pack = join(tmp, 'public/assets.pack');
    if (existsSync(pack)) copyFileSync(pack, join(out, id, 'assets.pack'));
  } finally {
    rmSync(tmp, {recursive: true, force: true});
  }
  console.log(`playground: ${id} (${Object.keys(files).length} file(s))`);
}
