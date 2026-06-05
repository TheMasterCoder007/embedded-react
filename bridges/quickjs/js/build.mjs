// Bundles a demo (React app + reconciler + host config) into a single classic (IIFE) script that
// QuickJS can run with a plain JS_Eval. Globals the bundle expects at runtime (NativeUI, screen,
// console, and timer shims) are provided by the C host (e.g., examples/linux/main_js.c).
//
// Demos live in the top-level demos/ folder, one folder per demo. Pick one with:
//   npm run build                 # default demo (thermostat)
//   npm run build -- marine-dash  # a specific demo by folder name
// The output is always dist/app.bundle.js — the single "active" bundle the example hosts pick up.
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync, readdirSync } from 'node:fs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');

const DEFAULT_DEMO = 'thermostat';
const demo = process.argv[2] || process.env.DEMO || DEFAULT_DEMO;
const entry = resolve(demosDir, demo, 'index.jsx');

if (!existsSync(entry)) {
  const available = existsSync(demosDir)
    ? readdirSync(demosDir, { withFileTypes: true })
        .filter((d) => d.isDirectory())
        .map((d) => d.name)
    : [];
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  console.error(`Available demos: ${available.join(', ') || '(none)'}`);
  process.exit(1);
}

await build({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: resolve(here, 'dist/app.bundle.js'),
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  // Demos live outside the embedded-react package, so the package self-reference doesn't resolve for
  // them: map the bare `embedded-react` import to the library source and let the demo's bare deps
  // (react, react-reconciler) resolve from this package's node_modules. (The library's own internal
  // imports still resolve relatively / from node_modules as before.)
  alias: { 'embedded-react': libEntry },
  nodePaths: [nodeModules],
  // Production React: smaller and avoids dev-only warning machinery that needs more shims.
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'info',
});

console.log(`Bundled demo "${demo}" -> dist/app.bundle.js`);
