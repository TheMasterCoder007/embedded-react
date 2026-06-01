// Bundles the React app + reconciler + host config into a single classic (IIFE) script that
// QuickJS can run with a plain JS_Eval. Globals the bundle expects at runtime (NativeUI, screen,
// console, and timer shims) are provided by the C host (examples/linux/main_js.c).
import { build } from 'esbuild';

await build({
  entryPoints: ['src/index.jsx'],
  bundle: true,
  format: 'iife',
  outfile: 'dist/app.bundle.js',
  platform: 'neutral',
  target: 'es2020',
  jsx: 'automatic',
  // Production React: smaller and avoids dev-only warning machinery that needs more shims.
  define: { 'process.env.NODE_ENV': '"production"' },
  legalComments: 'none',
  logLevel: 'info',
});

console.log('Bundled -> dist/app.bundle.js');
