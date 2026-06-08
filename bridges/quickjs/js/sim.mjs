// `npm run sim [demo]` — the embedded-react simulator launcher (see /SIMULATOR.md).
//
// Runs esbuild in watch mode (rebundling demos/<demo> → dist/app.bundle.js on every save) and
// launches the simulator binary, which watches that bundle and live-reloads the window on change —
// the React Native inner loop on the desktop. Edit your JSX, save, see it.
//
// One-time setup: build the simulator binary with CMake (tools/simulator). This script tells you how
// if it's missing.
import { context } from 'esbuild';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, basename } from 'node:path';
import { existsSync } from 'node:fs';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');
const demosDir = resolve(repoRoot, 'demos');
const libEntry = resolve(here, 'src/embedded-react/index.js');
const nodeModules = resolve(here, 'node_modules');
const distDir = resolve(here, 'dist');
const bundlePath = resolve(distDir, 'app.bundle.js');

const demo = process.argv[2] || process.env.DEMO || 'thermostat';
const entry = resolve(demosDir, demo, 'index.jsx');
if (!existsSync(entry)) {
  console.error(`Demo "${demo}" not found (expected ${entry}).`);
  process.exit(1);
}

// Locate the built simulator binary (override with ER_SIM_BIN).
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const simBin =
  process.env.ER_SIM_BIN || resolve(repoRoot, 'tools/simulator/build', `embedded-react-simulator${exeSuffix}`);
if (!existsSync(simBin)) {
  console.error(`Simulator binary not found at:\n  ${simBin}\n`);
  console.error('Build it once (then re-run `npm run sim`):');
  console.error('  cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]');
  console.error('  cmake --build tools/simulator/build');
  console.error('(or set ER_SIM_BIN to the binary path)');
  process.exit(1);
}

// 1. Initial full build: bundle + bake the demo's assets. The simulator binary compiled those baked
//    assets, so they resolve while the JS hot-reloads. (Changing an asset needs a sim rebuild.)
console.log(`Building demo "${demo}" (+ assets)...`);
const built = spawnSync(process.execPath, [resolve(here, 'build.mjs'), demo], { stdio: 'inherit' });
if (built.status !== 0) {
  process.exit(built.status || 1);
}

// 2. esbuild watch — rebundle to dist/app.bundle.js on save (bundle only; assets baked in step 1).
const assetName = (p) => basename(p).replace(/\.[^.]+$/, '');
const ctx = await context({
  entryPoints: [entry],
  bundle: true,
  format: 'iife',
  outfile: bundlePath,
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  alias: { 'embedded-react': libEntry },
  nodePaths: [nodeModules],
  plugins: [
    {
      name: 'embedded-react-asset-name',
      setup(b) {
        b.onLoad({ filter: /\.(png|jpe?g|webp|gif|bmp|ttf|otf)$/i }, (args) => ({
          contents: `module.exports = ${JSON.stringify(assetName(args.path))};`,
          loader: 'js',
        }));
      },
    },
    {
      name: 'sim-rebuild-log',
      setup(b) {
        b.onEnd((r) => {
          if (r.errors.length) console.error(`✗ build failed (${r.errors.length} error(s)) — fix and save to retry`);
          else console.log(`↻ rebuilt → dist/app.bundle.js`);
        });
      },
    },
  ],
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'silent',
});
await ctx.watch();
console.log(`Watching demos/${demo} — edit & save to hot-reload. Close the window or Ctrl-C to quit.`);

// 3. Launch the simulator pointed at the watched bundle.
const sim = spawn(simBin, [bundlePath], { stdio: 'inherit' });
const shutdown = async () => {
  try {
    await ctx.dispose();
  } catch {}
  try {
    sim.kill();
  } catch {}
  process.exit(0);
};
sim.on('exit', shutdown);
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
